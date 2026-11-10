#include "mod.h"
#include "../proc/mod.h"

static mmap_region_t *nodes[N_MMAP];
static uint32 arrivals[3];

static void barrier(uint32 phase)
{
    __atomic_add_fetch(&arrivals[phase], 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&arrivals[phase], __ATOMIC_ACQUIRE) != NCPU - FIRST_HART)
        ;
}

void lab5_pool_test(uint64 hart)
{
    uint32 count = N_MMAP / (NCPU - FIRST_HART);
    uint32 start = (hart - FIRST_HART) * count;
    if (hart == boot_hartid)
    {
        mmap_show_nodelist();
        printf("POOL INITIAL\n");
    }
    barrier(0);
    for (uint32 i = start; i < start + count; i++)
    {
        nodes[i] = mmap_region_alloc();
        assert(nodes[i] != NULL, "pool allocation");
    }
    barrier(1);
    assert(!mmap_region_alloc(), "pool exhaustion");
    for (uint32 i = start; i < start + count; i++)
        for (uint32 j = i + 1; j < N_MMAP; j++)
            assert(nodes[i] != nodes[j], "pool duplicate");

    // All harts finish observing the empty pool before any node is returned.
    barrier(2);
    for (uint32 i = start; i < start + count; i++)
        mmap_region_free(nodes[i]);

    static uint32 returned;
    __atomic_add_fetch(&returned, 1, __ATOMIC_ACQ_REL);
    if (hart == boot_hartid)
    {
        while (__atomic_load_n(&returned, __ATOMIC_ACQUIRE) != NCPU - FIRST_HART)
            ;
        mmap_show_nodelist();
        printf("POOL PASS\n");
    }
}

static uint64 leaf(pgtbl_t root, uint64 va)
{
    pte_t *pte = vm_getpte(root, va, false);
    return pte ? *pte : 0;
}

static void *drain_pages(bool kernel)
{
    page_node_t *head = NULL;
    page_node_t *page;
    while ((page = pmem_alloc(kernel)))
    {
        page->next = head;
        head = page;
    }
    return head;
}

static void return_pages(page_node_t *head, bool kernel)
{
    while (head)
    {
        page_node_t *next = head->next;
        pmem_free((uint64)head, kernel);
        head = next;
    }
}

static void test_copy(proc_t *p, uint64 base)
{
    pgtbl_t src = p->pgtbl;
    char bytes[] = "abcd";
    char out[sizeof(bytes)] = {0};
    uint64 cross = base + PGSIZE - 2;
    assert(uvm_copyout(src, cross, (uint64)bytes, sizeof(bytes)) == 0, "copyout cross-page");
    assert(uvm_copyin(src, (uint64)out, cross, sizeof(out)) == 0 &&
           strncmp(out, bytes, sizeof(out)) == 0, "copyin cross-page");
    assert(uvm_copyin_str(src, (uint64)out, cross, 4) == -1 &&
           uvm_copyin_str(src, (uint64)out, cross, 5) == 0, "string boundary");
    assert(uvm_copyin(src, (uint64)out, ~0ull, 0) == 0 &&
           uvm_copyout(src, ~0ull, (uint64)bytes, 0) == 0, "zero length");
    assert(uvm_copyin(src, (uint64)out, ~0ull, 2) == -1 &&
           uvm_copyout(src, USER_BASE, (uint64)bytes, 1) == -1 &&
           uvm_copyin(src, (uint64)out, TRAPFRAME, 1) == -1, "range and permissions");

    uint64 exec = base + 3 * PGSIZE;
    assert(uvm_mmap(exec, 1, PTE_X) == exec, "execute-only map");
    assert(uvm_copyin(src, (uint64)out, exec, 1) == -1, "execute-only read");

    char edge = 'z';
    uint64 last = exec - 1;
    assert(uvm_copyout(src, last, (uint64)&edge, 1) == 0, "last writable byte");
    assert(uvm_copyin(src, (uint64)out, last, 2) == -1 && out[0] == edge,
           "copy retains prefix");
    assert(p->mmap->next && p->mmap->npages == 3, "different permissions stay separate");
}

static void test_munmap(proc_t *p, uint64 base)
{
    uint32 held = 0;
    while (held < N_MMAP && (nodes[held] = mmap_region_alloc()))
        held++;
    assert(held == N_MMAP - 2, "mmap pool accounting");
    assert(uvm_munmap(base + PGSIZE, 1) == -1 && p->mmap->npages == 3 &&
           leaf(p->pgtbl, base + PGSIZE), "split exhaustion rollback");

    mmap_region_free(nodes[--held]);
    assert(uvm_munmap(base + PGSIZE, 1) == 0, "split");
    assert(uvm_munmap(base, 4) == -1 && p->mmap->begin == base &&
           leaf(p->pgtbl, base), "hole rollback");
    assert(uvm_mmap(0, 1, PTE_R | PTE_W) == base + PGSIZE &&
           p->mmap->npages == 3 && p->mmap->next->perm == (PTE_X | PTE_U),
           "first fit and merge");
    assert(uvm_mmap(base, 1, PTE_R) == 0, "reject overlap");
    while (held)
        mmap_region_free(nodes[--held]);
}

static void test_exhaustion(proc_t *p, pgtbl_t dest, uint64 base)
{
    pte_t tf = leaf(dest, TRAPFRAME);
    pte_t trampoline = leaf(dest, TRAMPOLINE);
    page_node_t *held = drain_pages(false);
    assert(held != NULL, "user pages available");
    page_node_t *one = held;
    held = held->next;
    pmem_free((uint64)one, false);

    assert(uvm_copy_pgtbl(p->pgtbl, dest, p->heap_top, p->ustack_npage, p->mmap) == -1 &&
           !leaf(dest, USER_BASE) && leaf(dest, TRAPFRAME) == tf &&
           leaf(dest, TRAMPOLINE) == trampoline, "copy rollback");

    assert(uvm_heap_grow(p->pgtbl, p->heap_top, 2 * PGSIZE) == (uint64)-1 &&
           !leaf(p->pgtbl, p->heap_top), "heap rollback");

    uint64 stack = USER_STACK_TOP - 3 * PGSIZE;
    assert(uvm_ustack_grow(p->pgtbl, 1, stack) == -1 &&
           !leaf(p->pgtbl, stack), "stack rollback");

    uint64 end = base + 4 * PGSIZE;
    assert(uvm_mmap(end, 2, PTE_R | PTE_W) == 0 &&
           !leaf(p->pgtbl, end) && !p->mmap->next->next, "mmap rollback");
    assert(pmem_alloc(false) == one && !pmem_alloc(false), "user page accounting");
    pmem_free((uint64)one, false);
    return_pages(held, false);

    held = drain_pages(true);
    assert(held != NULL, "kernel pages available");
    one = held;
    held = held->next;
    pmem_free((uint64)one, true);
    // No table exists below the root at this address. Fail on the second one.
    uint64 va = 1ul << 30;
    assert(vm_mappages(dest, va, PGSIZE, PGSIZE, PTE_R) == -1 &&
           !vm_getpte(dest, va, false), "intermediate allocation rollback");
    assert(pmem_alloc(true) == one && !pmem_alloc(true), "kernel page accounting");
    pmem_free((uint64)one, true);
    return_pages(held, true);
}

void lab5_vm_test(void)
{
    proc_t *p = myproc();
    uint64 base = uvm_mmap(0, 3, PTE_R | PTE_W);
    assert(base == MMAP_BEGIN, "first mmap address");
    test_copy(p, base);
    test_munmap(p, base);

    void *tf = pmem_alloc(true);
    pgtbl_t copy = proc_pgtbl_init((uint64)tf);
    assert(tf && copy, "test page table");
    test_exhaustion(p, copy, base);
    assert(uvm_copy_pgtbl(p->pgtbl, copy, p->heap_top, p->ustack_npage, p->mmap) == 0,
           "copy success");
    uint64 exec = base + 3 * PGSIZE;
    assert((leaf(copy, exec) & PTE_PERM) == (PTE_X | PTE_U) &&
           PTE_TO_PA(leaf(copy, exec)) != PTE_TO_PA(leaf(p->pgtbl, exec)) &&
           PTE_TO_PA(leaf(copy, TRAPFRAME)) == (uint64)tf &&
           leaf(copy, TRAMPOLINE) == leaf(p->pgtbl, TRAMPOLINE), "copy permissions and special pages");

    char out[5];
    assert(uvm_copyin_str(copy, (uint64)out, base + PGSIZE - 2, sizeof(out)) == 0 && strncmp(out, "ab", sizeof(out)) == 0,
           "remapped page zero-filled or changed after split");
    // A preserved byte on the last writable page demonstrates actual copying.
    assert(uvm_copyin(copy, (uint64)out, exec - 1, 1) == 0 && out[0] == 'z', "copied contents");
    uvm_destroy_pgtbl(copy);
    pmem_free((uint64)tf, true);

    assert(uvm_munmap(base, 4) == 0 && !p->mmap, "cross-node unmap");
    char buf[] = "abcd";
    memmove(buf + 1, buf, 3);
    assert(strncmp(buf, "aabc", sizeof(buf)) == 0, "overlapping memmove");
    printf("printf probe: %d %p %x %f %%\n", (-2147483647 - 1), 42u, 42ull, -1.25);
    printf("VM PASS\n");
}
