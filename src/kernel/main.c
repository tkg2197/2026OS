#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

extern void _entry(void);

static volatile int initialized = 0;

// 第三阶段 test-2：建立两个映射，检查物理地址和权限，然后解除映射。
static void test_mapping_and_unmapping(void)
{
    pgtbl_t table = pmem_alloc(true);
    uint64 va1 = 0x100000, va2 = 0x8000;
    uint64 pa1 = (uint64)pmem_alloc(false);
    uint64 pa2 = (uint64)pmem_alloc(false);

    vm_mappages(table, va1, pa1, PGSIZE, PTE_R | PTE_W);
    vm_mappages(table, va2, pa2, PGSIZE, PTE_R);
    pte_t *pte1 = vm_getpte(table, va1, false);
    pte_t *pte2 = vm_getpte(table, va2, false);
    assert(pte1 && PTE_TO_PA(*pte1) == pa1, "first mapping");
    assert(pte2 && PTE_TO_PA(*pte2) == pa2, "second mapping");
    int flags = PTE_V | PTE_R | PTE_W | PTE_X;
    assert((*pte1 & flags) == (PTE_V | PTE_R | PTE_W), "first permissions");
    assert((*pte2 & flags) == (PTE_V | PTE_R), "second permissions");

    vm_unmappages(table, va1, PGSIZE, true);
    vm_unmappages(table, va2, PGSIZE, true);
    assert(!(*pte1 & PTE_V) && !(*pte2 & PTE_V), "mappings still valid");
    vm_freewalk(table);
    printf("test_mapping_and_unmapping passed!\n");
}

void main(void)
{
    uint64 hartid = r_tp();

    if (hartid == 0)
    {
        print_init();
        pmem_init();

        kvm_init();
        kvm_inithart();
        kheap_init();

        printf("cpu %d: paging enabled, satp=%x\n",
               (int)hartid, r_satp());

        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);

        for (uint64 id = 1; id < NCPU; id++)
        {
            sbi_ret_t ret = sbi_hart_start(id, (uint64)_entry, 0);
            assert(ret.error == SBI_SUCCESS,
                   "sbi_hart_start failed");
        }
    }
    else
    {
        while (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE))
            ;

        kvm_inithart();

        printf("cpu %d: paging enabled, satp=%x\n",
               (int)hartid, r_satp());
    }

    if (hartid == 0)
        test_mapping_and_unmapping();

    while (1)
        asm volatile("wfi");
}
