#include "mod.h"

static uint64 user_pa(pgtbl_t pgtbl, uint64 va, int perm)
{
    pte_t *pte = vm_getpte(pgtbl, va, false);
    int required = PTE_V | PTE_U | perm;
    if (!pte || (*pte & required) != required)
        return 0;
    return PTE_TO_PA(*pte) + va % PGSIZE;
}

static int copy_user(pgtbl_t pgtbl, uint64 user, uint64 kernel,
                     uint32 len, bool to_user)
{
    if (!len)
        return 0;
    if (user >= VA_MAX || len > VA_MAX - user || len > ~kernel)
        return -1;

    while (len)
    {
        uint64 pa = user_pa(pgtbl, user, to_user ? PTE_W : PTE_R);
        if (!pa)
            return -1;
        uint32 count = MIN(len, PGSIZE - user % PGSIZE);
        if (to_user)
            memmove((void *)pa, (const void *)kernel, count);
        else
            memmove((void *)kernel, (const void *)pa, count);
        user += count;
        kernel += count;
        len -= count;
    }
    return 0;
}

int uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    return copy_user(pgtbl, src, dst, len, false);
}

int uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    return copy_user(pgtbl, dst, src, len, true);
}

int uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    if (maxlen > ~dst)
        return -1;
    char *out = (char *)dst;
    while (maxlen)
    {
        uint64 pa = user_pa(pgtbl, src, PTE_R);
        if (!pa)
            return -1;
        uint32 count = MIN(maxlen, PGSIZE - src % PGSIZE);
        const char *in = (const char *)pa;
        for (uint32 i = 0; i < count; i++)
        {
            *out++ = in[i];
            if (in[i] == '\0')
                return 0;
        }
        src += count;
        maxlen -= count;
    }
    return -1;
}

static int stage_range(pgtbl_t pgtbl, pgtbl_t source, uint64 begin,
                       uint64 end, int perm)
{
    for (uint64 va = begin; va < end; va += PGSIZE)
    {
        uint64 pa = 0;
        if (source)
        {
            pte_t *pte = vm_getpte(source, va, false);
            if (!pte || (*pte & (PTE_V | PTE_U)) != (PTE_V | PTE_U))
                return -1;
            pa = PTE_TO_PA(*pte);
            perm = *pte & PTE_PERM;
        }
        void *page = pmem_alloc(false);
        if (!page)
            return -1;
        if (source)
            memmove(page, (const void *)pa, PGSIZE);
        if (vm_stage_page(pgtbl, va, (uint64)page, perm) < 0)
        {
            pmem_free((uint64)page, false);
            return -1;
        }
    }
    return 0;
}

static int alloc_range(pgtbl_t pgtbl, uint64 begin, uint64 end, int perm)
{
    if (begin == end)
        return 0;
    int result = stage_range(pgtbl, NULL, begin, end, perm);
    vm_finish(pgtbl, result == 0, true);
    return result;
}

static uint64 region_end(const mmap_region_t *node)
{
    return node->begin + (uint64)node->npages * PGSIZE;
}

void uvm_show_mmaplist(mmap_region_t *mmap)
{
    for (; mmap; mmap = mmap->next)
        printf("mmap: %x ~ %x (perm %p)\n",
               mmap->begin, region_end(mmap), (uint32)mmap->perm);
}

static uint64 uvm_mmap_find(mmap_region_t *head, uint64 len,
                            mmap_region_t **prev, mmap_region_t **next)
{
    uint64 begin = MMAP_BEGIN;
    *prev = NULL;
    *next = head;
    while (*next && (*next)->begin - begin < len)
    {
        begin = region_end(*next);
        *prev = *next;
        *next = (*next)->next;
    }
    return len <= MMAP_END - begin ? begin : 0;
}

uint64 uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    uint64 size = (uint64)npages * PGSIZE;
    if (!size || size > MMAP_END - MMAP_BEGIN || begin % PGSIZE ||
        !vm_valid_perm(perm | PTE_U))
        return 0;

    proc_t *p = myproc();
    mmap_region_t *prev = NULL;
    mmap_region_t *next = p->mmap;
    if (!begin)
    {
        begin = uvm_mmap_find(next, size, &prev, &next);
        if (!begin)
            return 0;
    }
    else
    {
        if (begin < MMAP_BEGIN || begin > MMAP_END - size)
            return 0;
        while (next && region_end(next) <= begin)
        {
            prev = next;
            next = next->next;
        }
        if (next && next->begin < begin + size)
            return 0;
    }

    perm |= PTE_U;
    bool join_prev = prev && region_end(prev) == begin && prev->perm == perm;
    bool join_next = next && next->begin == begin + size && next->perm == perm;
    mmap_region_t *node = NULL;
    if (!join_prev && !join_next)
    {
        node = mmap_region_alloc();
        if (!node)
            return 0;
    }
    if (alloc_range(p->pgtbl, begin, begin + size, perm) < 0)
    {
        if (node)
            mmap_region_free(node);
        return 0;
    }

    if (join_prev)
    {
        prev->npages += npages;
        if (join_next)
        {
            prev->npages += next->npages;
            prev->next = next->next;
            mmap_region_free(next);
        }
    }
    else if (join_next)
    {
        next->begin = begin;
        next->npages += npages;
    }
    else
    {
        *node = (mmap_region_t){ .begin = begin, .npages = npages,
                                .perm = perm, .next = next };
        if (prev)
            prev->next = node;
        else
            p->mmap = node;
    }
    return begin;
}

int uvm_munmap(uint64 begin, uint32 npages)
{
    uint64 size = (uint64)npages * PGSIZE;
    if (!size || size > MMAP_END - MMAP_BEGIN || begin % PGSIZE ||
        begin < MMAP_BEGIN || begin > MMAP_END - size)
        return -1;

    uint64 end = begin + size;
    proc_t *p = myproc();
    mmap_region_t **link = &p->mmap;
    while (*link && region_end(*link) <= begin)
        link = &(*link)->next;

    // Verify full coverage before touching any leaf or list link.
    uint64 covered = begin;
    for (mmap_region_t *node = *link; covered < end; node = node->next)
    {
        if (!node || node->begin > covered)
            return -1;
        covered = region_end(node);
    }

    mmap_region_t *first = *link;
    mmap_region_t *right = NULL;
    if (first->begin < begin && region_end(first) > end)
    {
        right = mmap_region_alloc();
        if (!right)
            return -1;
        *right = (mmap_region_t){ .begin = end, .npages = (region_end(first) - end) / PGSIZE,
                                 .perm = first->perm, .next = first->next };
    }

    vm_unmappages(p->pgtbl, begin, size, true);
    if (right)
    {
        first->npages = (begin - first->begin) / PGSIZE;
        first->next = right;
        return 0;
    }
    if (first->begin < begin)
    {
        first->npages = (begin - first->begin) / PGSIZE;
        link = &first->next;
    }
    while (*link && (*link)->begin < end)
    {
        mmap_region_t *node = *link;
        uint64 old_end = region_end(node);
        if (old_end > end)
        {
            node->begin = end;
            node->npages = (old_end - end) / PGSIZE;
            break;
        }
        *link = node->next;
        mmap_region_free(node);
    }
    return 0;
}

uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur, uint32 len)
{
    if (cur < USER_HEAP_BASE || cur > MMAP_BEGIN || len > MMAP_BEGIN - cur)
        return -1;
    uint64 end = cur + len;
    if (alloc_range(pgtbl, ALIGN_UP(cur, PGSIZE), ALIGN_UP(end, PGSIZE),
                    PTE_R | PTE_W | PTE_U) < 0)
        return -1;
    return end;
}

uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 cur, uint32 len)
{
    if (cur < USER_HEAP_BASE || cur > MMAP_BEGIN || len > cur - USER_HEAP_BASE)
        return -1;
    uint64 end = cur - len;
    uint64 from = ALIGN_UP(end, PGSIZE);
    uint64 to = ALIGN_UP(cur, PGSIZE);
    if (from < to)
        vm_unmappages(pgtbl, from, to - from, true);
    return end;
}

int64 uvm_ustack_grow(pgtbl_t pgtbl, uint64 old_pages, uint64 fault_addr)
{
    if (old_pages > (USER_STACK_TOP - MMAP_END) / PGSIZE)
        return -1;
    uint64 old_base = USER_STACK_TOP - old_pages * PGSIZE;
    if (fault_addr < MMAP_END || fault_addr >= old_base)
        return -1;

    uint64 base = ALIGN_DOWN(fault_addr, PGSIZE);
    if (alloc_range(pgtbl, base, old_base, PTE_R | PTE_W | PTE_U) < 0)
        return -1;
    return (USER_STACK_TOP - base) / PGSIZE;
}

int uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top,
                   uint64 stack_pages, mmap_region_t *mmap)
{
    if (!old || !new || old == new || heap_top < USER_HEAP_BASE ||
        heap_top > MMAP_BEGIN || stack_pages > (USER_STACK_TOP - MMAP_END) / PGSIZE)
        return -1;

    int result = stage_range(new, old, USER_BASE, ALIGN_UP(heap_top, PGSIZE), 0);
    if (result == 0)
        result = stage_range(new, old, USER_STACK_TOP - stack_pages * PGSIZE,
                             USER_STACK_TOP, 0);
    for (; mmap && result == 0; mmap = mmap->next)
        result = stage_range(new, old, mmap->begin, region_end(mmap), 0);

    vm_finish(new, result == 0, true);
    return result;
}
