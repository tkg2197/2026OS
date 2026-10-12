#include "mod.h"

#define HEAP_CLASS_COUNT 8
#define HEAP_DEFAULT_ALIGN 16

static const uint32 heap_sizes[HEAP_CLASS_COUNT] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096
};

typedef struct heap_page {
    uint64 pa; // 本页起始地址
    uint32 class_index; // 属于什么大小，32为0， 4096为7
    uint32 free_count; // 还有多少空闲块
    uint64 used[2]; //每个used有64位，每位1代表该块正在使用，0代表空闲
    struct heap_page *prev;
    struct heap_page *next;
    bool active; // 是否由堆管理
} heap_page_t;

static heap_page_t heap_pages[KERN_PAGES];
static heap_page_t *partial[HEAP_CLASS_COUNT]; // 每个档位大小的空闲链表的第一块
static spinlock_t heap_lock;

static int heap_class_index(uint64 size, uint64 align) {
    if (size == 0 || size > PGSIZE || align == 0 || align > PGSIZE || (align & (align - 1)) != 0)
        return -1;
    uint64 need = size > align ? size : align;
    for (int c = 0; c < HEAP_CLASS_COUNT; c++)
        if (heap_sizes[c] >= need)
            return c;
    return -1;
}

static int heap_page_index(uint64 pa) {
    uint64 begin = (uint64)ALLOC_BEGIN;
    uint64 end = begin + (uint64)KERN_PAGES * PGSIZE;
    if (pa < begin || pa >= end || pa % PGSIZE != 0)
        return -1;
    return (int)((pa - begin) / PGSIZE);
}

static void partial_add(heap_page_t *p) {
    uint32 c = p->class_index;
    p->prev = NULL;
    p->next = partial[c];
    if (partial[c] != NULL)
        partial[c]->prev = p;
    partial[c] = p;
}

static void partial_remove(heap_page_t *p) {
    uint32 c = p->class_index;
    if (p->prev != NULL)
        p->prev->next = p->next;
    else
        partial[c] = p->next;
    if (p->next != NULL)
        p->next->prev = p->prev;
    p->prev = NULL;
    p->next = NULL;
}

static heap_page_t *heap_grow_locked(uint32 c) {
    void *page = pmem_try_alloc(true);
    if (page == NULL)
        return NULL;
    int index = heap_page_index((uint64)page);
    if (index < 0) {
        spinlock_release(&heap_lock);
        panic("heap: buddy returned an invalid page");
        return NULL;
    }
    heap_page_t *p = &heap_pages[index];
    if (p->active) {
        spinlock_release(&heap_lock);
        panic("heap: buddy returned an active page");
        return NULL;
    }
    p->pa = (uint64)page;
    p->class_index = c;
    p->free_count = PGSIZE / heap_sizes[c];
    p->used[0] = 0;
    p->used[1] = 0;
    p->active = true;
    partial_add(p);
    return p;
}

static void *heap_take_locked(heap_page_t *p) {
    uint32 size = heap_sizes[p->class_index];
    uint32 count = PGSIZE / size;
    for (uint32 i = 0; i < count; i++) {
        uint32 word = i / 64;
        uint64 bit = (uint64)1 << (i % 64);
        if ((p->used[word] & bit) == 0) {
            p->used[word] |= bit;
            p->free_count--;
            if (p->free_count == 0)
                partial_remove(p);
            return (void *)(p->pa + (uint64)i * size);
        }
    }
    spinlock_release(&heap_lock);
    panic("heap: free count disagrees with bitmap");
    return NULL;
}

//初始化堆只由cpu0执行一次
void kheap_init(void) {
    spinlock_init(&heap_lock, "heap");
    memset(heap_pages, 0, sizeof(heap_pages));
    memset(partial, 0, sizeof(partial));
}

void *kmalloc(uint64 size) {
    int c = heap_class_index(size, HEAP_DEFAULT_ALIGN);
    if (c < 0)
        return NULL;
    spinlock_acquire(&heap_lock);
    heap_page_t *p = partial[c];
    if (p == NULL)
        p = heap_grow_locked((uint32)c);
    if (p == NULL) {
        spinlock_release(&heap_lock);
        return NULL;
    }
    void *result = heap_take_locked(p);
    spinlock_release(&heap_lock);
    return result;
}

void *kmalloc_aligned(uint64 size, uint64 align)
{
    int c = heap_class_index(size, align);
    if (c < 0)
        return NULL;

    spinlock_acquire(&heap_lock);
    heap_page_t *p = partial[c];
    if (p == NULL)
        p = heap_grow_locked((uint32)c);
    if (p == NULL) {
        spinlock_release(&heap_lock);
        return NULL;
    }

    void *result = heap_take_locked(p);
    spinlock_release(&heap_lock);
    return result;
}

// 完全空闲的页立即归还buddy
void kfree(void *ptr) {
    if (ptr == NULL)
        return;
    uint64 address = (uint64)ptr;
    uint64 pa = address & ~((uint64)PGSIZE - 1);
    int index = heap_page_index(pa);
    if (index < 0) {
        panic("heap free: address outside kernel page pool");
        return;
    }
    spinlock_acquire(&heap_lock);
    heap_page_t *p = &heap_pages[index];
    if (!p->active) {
        spinlock_release(&heap_lock);
        panic("heap free: page does not belong to heap");
        return;
    }
    uint32 size = heap_sizes[p->class_index];
    uint64 offset = address - p->pa;
    if (offset >= PGSIZE || offset % size != 0) {
        spinlock_release(&heap_lock);
        panic("heap free: address is not a block start");
        return;
    }
    uint32 block = (uint32)(offset / size);
    uint32 word = block / 64;
    uint64 bit = (uint64)1 << (block % 64);
    if ((p->used[word] & bit) == 0) {
        spinlock_release(&heap_lock);
        panic("heap free: block already free");
        return;
    }
    bool was_full = p->free_count == 0;
    p->used[word] &= ~bit;
    p->free_count++;
    if (p->free_count == PGSIZE / size) {
        if (!was_full)
            partial_remove(p);
        p->active = false;
        p->pa = 0;
        p->class_index = 0;
        p->free_count = 0;
        p->used[0] = 0;
        p->used[1] = 0;
        p->prev = NULL;
        p->next = NULL;
        pmem_free(pa, true);
    }
    else if (was_full) {
        partial_add(p);
    }
    spinlock_release(&heap_lock);
}

uint32 kheap_pages(void) {
    spinlock_acquire(&heap_lock);
    uint32 count = 0;
    for (uint32 i = 0; i < KERN_PAGES; i++)
        if (heap_pages[i].active)
            count++;
    spinlock_release(&heap_lock);
    return count;
}

void kheap_print(void)
{
    spinlock_acquire(&heap_lock);
    uint32 pages[HEAP_CLASS_COUNT] = {0};
    uint32 live[HEAP_CLASS_COUNT] = {0};
    uint32 total = 0;
    for (uint32 i = 0; i < KERN_PAGES; i++) {
        heap_page_t *p = &heap_pages[i];
        if (!p->active)
            continue;
        uint32 c = p->class_index;
        pages[c]++;
        live[c] += PGSIZE / heap_sizes[c] - p->free_count;
        total++;
    }

    printf("heap: %d pages\n", (int)total);
    printf("block bytes | pages | live blocks\n");
    for (int c = 0; c < HEAP_CLASS_COUNT; c++)
        printf("%d | %d | %d\n", (int)heap_sizes[c],
               (int)pages[c], (int)live[c]);
    spinlock_release(&heap_lock);
}
