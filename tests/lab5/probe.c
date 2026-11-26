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

// 测试 5：副本拥有独立的用户页，销毁后原进程仍能读取自己的数据。
void lab5_vm_test(void)
{
    proc_t *p = myproc();
    uint64 va = uvm_mmap(0, 1, PTE_R | PTE_W);
    char value = 'A';
    assert(va && uvm_copyout(p->pgtbl, va, (uint64)&value, 1) == 0,
           "prepare source page");

    void *tf = pmem_alloc(true);
    assert(tf != NULL, "allocate trapframe");
    pgtbl_t copy = proc_pgtbl_init((uint64)tf);
    assert(copy != NULL, "allocate page table");
    assert(uvm_copy_pgtbl(p->pgtbl, copy, p->heap_top, p->ustack_npage, p->mmap) == 0,
           "copy page table");

    value = 0;
    assert(uvm_copyin(copy, (uint64)&value, va, 1) == 0 && value == 'A',
           "read copied page");
    value = 'B';
    assert(uvm_copyout(copy, va, (uint64)&value, 1) == 0, "write copied page");
    uvm_destroy_pgtbl(copy);
    pmem_free((uint64)tf, true);

    assert(uvm_copyin(p->pgtbl, (uint64)&value, va, 1) == 0 && value == 'A',
           "source page survived");
    assert(uvm_munmap(va, 1) == 0, "release source page");
    printf("VM PASS\n");
}
