#include "mod.h"

#define BUDDY_MAX_ORDER 15
#define BUDDY_PAGE_CAP (PHYS_MEM_SIZE / PGSIZE)

// -1表示不是独立块首，-2-order表示是第order阶已分配的块首
#define STATE_UNUSED (-1)
#define STATE_ALLOC(order) (-(int)(order) - 2)

typedef struct buddy_region {
    // 保存地址所以用64位
    uint64 begin, end;
    uint32 npages, allocable;
    spinlock_t lk;
    // 为每个阶数的块首建立链表
    page_node_t *free[BUDDY_MAX_ORDER + 1];
    // 多个buddy区域共用一个state数组,但在初始化的时候指向state数组的不同位置
    int16 *state;
} buddy_region_t;

static buddy_region_t kern_region, user_region;
static int16 page_state[BUDDY_PAGE_CAP];

static buddy_region_t *region_of(bool in_kernel) {
    return in_kernel ? &kern_region : &user_region;
}

static uint32 page_index(buddy_region_t *r, uint64 pa) {
    return (pa - r->begin) / PGSIZE;
}

static uint64 page_address(buddy_region_t *r, uint32 index) {
    return r->begin + (uint64)index * PGSIZE;
}

//以下链表辅助函数调用要求持有区域锁 
static void list_push(buddy_region_t *r, uint32 index, unsigned order) {
    page_node_t *node = (page_node_t *)page_address(r, index);
    node->next = r->free[order];
    r->free[order] = node;
    r->state[index] = (int16)order;
}

static uint32 list_pop(buddy_region_t *r, unsigned order) {
    page_node_t *node = r->free[order];
    uint32 index = page_index(r, (uint64)node);
    r->free[order] = node->next;
    r->state[index] = STATE_UNUSED;
    return index;
}

static void list_remove(buddy_region_t *r, uint32 index, unsigned order){
    page_node_t *target = (page_node_t *)page_address(r, index);
    page_node_t **link = &r->free[order];
    while (*link != NULL && *link != target) {
        link = &(*link)->next;
    }
    if (*link == NULL) {
        spinlock_release(&r->lk);
        panic("buddy: state/list mismatch");
        return;
    }
    *link = target->next;
    r->state[index] = STATE_UNUSED;
}

static void region_init(buddy_region_t *r, uint64 begin, uint64 end, int16 *state, char *name) {
    r->begin = begin;
    r->end = end;
    r->npages = (end - begin) / PGSIZE;
    r->allocable = r->npages;
    r->state = state;

    spinlock_init(&r->lk, name);
    spinlock_acquire(&r->lk);
    for (unsigned order = 0; order <= BUDDY_MAX_ORDER; order++) {
        r->free[order] = NULL;
    }
    for (uint32 i = 0; i < r->npages; i++) {
        r->state[i] = STATE_UNUSED;
    }
    uint32 index = 0;
    while (index < r->npages) {
        unsigned order = BUDDY_MAX_ORDER;
        while ((index & ((1u << order) - 1)) != 0 || (1u << order) > r->npages - index)
            order--;
        list_push(r, index, order);
        index += 1u << order;
    }
    spinlock_release(&r->lk);
}

void pmem_init(void) {
    uint64 begin = (uint64)ALLOC_BEGIN;
    uint64 end = (uint64)ALLOC_END;
    assert(begin % PGSIZE == 0 && end % PGSIZE == 0, "pmem_init: unaligned region");
    assert(begin < end && (end - begin) / PGSIZE >= KERN_PAGES, "pmem_init: region too small");
    assert((end - begin) / PGSIZE <= BUDDY_PAGE_CAP, "pmem_init: metadata too small");    
    uint64 split = begin + (uint64)KERN_PAGES *PGSIZE;
    region_init(&kern_region, begin, split, page_state, "kern_buddy");
    region_init(&user_region, split, end, page_state + KERN_PAGES, "user_buddy");
}

void *pmem_try_alloc_order(unsigned order, bool in_kernel) {
    assert(order <= BUDDY_MAX_ORDER, "buddy alloc: bad order");
    buddy_region_t *r = region_of(in_kernel);
    spinlock_acquire(&r->lk);
    unsigned found = order;
    while (found <= BUDDY_MAX_ORDER && r->free[found] == NULL)
        found++;
    if (found > BUDDY_MAX_ORDER) {
        spinlock_release(&r->lk);
        return NULL;
    }
    uint32 index = list_pop(r, found);
    while (found > order) {
        found--;
        list_push(r, index + (1u << found), found);
    }
    r->state[index] = (int16)STATE_ALLOC(order);
    r->allocable -= 1u <<order;
    spinlock_release(&r->lk);
    void *block = (void *)page_address(r, index);
    memset(block, 0, (uint64)PGSIZE << order);
    return block;
}

void *pmem_alloc_order(unsigned order, bool in_kernel)
{
    void *block = pmem_try_alloc_order(order, in_kernel);

    if (block == NULL)
        panic("buddy alloc: no suitable block");

    return block;
}

void *pmem_try_alloc(bool in_kernel)
{
    return pmem_try_alloc_order(0, in_kernel);
}

void pmem_free_order(uint64 pa, unsigned order, bool in_kernel) {
    assert(order <= BUDDY_MAX_ORDER, "buddy free: bad order");
    buddy_region_t *r = region_of(in_kernel);
    uint32 pages = 1u << order;
    assert(pa % PGSIZE == 0, "buddy free: unaligned page");
    assert(pa >= r->begin && pa < r->end, "buddy free: wrong region");
    uint32 index = page_index(r, pa);
    assert((index & (pages - 1)) == 0 && pages <= r->npages - index, "buddy free: bad block bounds");
    spinlock_acquire(&r->lk);
    if (r->state[index] != STATE_ALLOC(order)) {
        spinlock_release(&r->lk);
        panic("buddy free: invalid block or order");
        return;
    }
    r->state[index] = STATE_UNUSED;
    r->allocable += pages;
    while (order < BUDDY_MAX_ORDER) {
        // 找伙伴
        uint32 buddy = index ^ (1u << order);
        if (buddy >= r->npages || r->state[buddy] != (int16)order)
            break;
        list_remove(r, buddy, order);
        if (buddy < index)
            index = buddy;
        order++;
    }
    list_push(r, index, order);
    spinlock_release(&r->lk);
}

void *pmem_alloc(bool in_kernel) {
    return pmem_alloc_order(0, in_kernel);
}

void pmem_free(uint64 page, bool in_kernel){
    pmem_free_order(page, 0, in_kernel);
}

uint32 pmem_freepages(bool in_kernel) {
    buddy_region_t *r = region_of(in_kernel);
    spinlock_acquire(&r->lk);
    uint32 result = r->allocable;
    spinlock_release(&r->lk);
    return result;
}
