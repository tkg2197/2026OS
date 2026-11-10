#include "mod.h"

static alloc_region_t kern_region, user_region;

// A free-page bit lets pmem_free catch double frees without scanning the list.
#define MANAGED_PAGES ((128ul << 20) / PGSIZE)
static uint32 free_pages[MANAGED_PAGES / 32];

static uint32 *free_word(uint64 page, uint32 *mask)
{
    uint64 index = (page - KERNEL_BASE) / PGSIZE;
    *mask = 1u << (index % 32);
    return &free_pages[index / 32];
}

bool check_inkernel(uint64 page)
{
    return page >= kern_region.begin && page < kern_region.end;
}

static void init_region(alloc_region_t *region, uint64 begin, uint64 end,
                        const char *name, bool in_kernel)
{
    region->begin = begin;
    region->end = end;
    spinlock_init(&region->lk, name);
    for (uint64 page = begin; page < end; page += PGSIZE)
        pmem_free(page, in_kernel);
}

void pmem_init(void)
{
    uint64 begin = ALIGN_UP((uint64)ALLOC_BEGIN, PGSIZE);
    uint64 end = ALIGN_DOWN((uint64)ALLOC_END, PGSIZE);
    uint64 user_begin = begin + KERN_PAGES * PGSIZE;
    assert(begin >= KERNEL_BASE && user_begin < end &&
           end - KERNEL_BASE <= MANAGED_PAGES * PGSIZE, "pmem layout");
    init_region(&kern_region, begin, user_begin, "kernel pages", true);
    init_region(&user_region, user_begin, end, "user pages", false);
}

void *pmem_alloc(bool in_kernel)
{
    alloc_region_t *region = in_kernel ? &kern_region : &user_region;
    spinlock_acquire(&region->lk);
    page_node_t *page = region->list_head.next;
    if (page)
    {
        uint32 mask;
        uint32 *word = free_word((uint64)page, &mask);
        __atomic_fetch_and(word, ~mask, __ATOMIC_RELAXED);
        region->list_head.next = page->next;
        region->allocable--;
    }
    spinlock_release(&region->lk);
    if (page)
        memset(page, 0, PGSIZE);
    return page;
}

void pmem_free(uint64 page, bool in_kernel)
{
    alloc_region_t *region = in_kernel ? &kern_region : &user_region;
    assert(page >= region->begin && page < region->end &&
           page % PGSIZE == 0, "pmem_free: invalid page");
    spinlock_acquire(&region->lk);
    uint32 mask;
    uint32 *word = free_word(page, &mask);
    assert(!(__atomic_fetch_or(word, mask, __ATOMIC_RELAXED) & mask),
           "pmem_free: double free");
    page_node_t *node = (page_node_t *)page;
    memset(node, 1, PGSIZE);
    node->next = region->list_head.next;
    region->list_head.next = node;
    region->allocable++;
    spinlock_release(&region->lk);
}
