#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

/* 任务页第二、三阶段测试。只在 runner 创建的临时副本中作为 main.c。 */
#ifndef LAB2_CASE
#define LAB2_CASE 1
#endif
extern void _entry(void);
static int started, allocated, freed;
static uint64 pages[KERN_PAGES];

static void physical_parallel(uint64 hart)
{
    if (hart == 0) {
        print_init();
        pmem_init();
        assert(NCPU == 2, "official: expected two CPUs");
        printf("OFFICIAL PMEM test-1: two CPUs x 512 pages, paging OFF\n");
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
        sbi_ret_t ret = sbi_hart_start(1, (uint64)_entry, 0);
        assert(ret.error == SBI_SUCCESS, "official: start CPU1");
    } else {
        while (!__atomic_load_n(&started, __ATOMIC_ACQUIRE));
    }
    for (unsigned i = hart * 512; i < (hart + 1) * 512; i++) {
        pages[i] = (uint64)pmem_alloc(true);
        memset((void *)pages[i], 1, PGSIZE);
        printf("cpu %d mem = %p, data = %d\n", (int)hart,
               (void *)pages[i], *(int *)pages[i]);
    }
    printf("cpu %d alloc over\n", (int)hart);
    __atomic_add_fetch(&allocated, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&allocated, __ATOMIC_ACQUIRE) != 2);
    if (hart == 0) {
        assert(pmem_freepages(true) == 0, "official: not exhausted");
        for (unsigned i = 0; i < KERN_PAGES; i++) {
            for (unsigned j = 0; j < i; j++)
                assert(pages[i] != pages[j], "official: duplicate physical page");
            for (unsigned j = 0; j < PGSIZE; j++)
                assert(((uint8 *)pages[i])[j] == 1, "official: content changed");
        }
        __atomic_store_n(&started, 2, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) != 2);
    for (unsigned i = hart * 512; i < (hart + 1) * 512; i++)
        pmem_free(pages[i], true);
    printf("cpu %d free over\n", (int)hart);
    __atomic_add_fetch(&freed, 1, __ATOMIC_ACQ_REL);
    if (hart == 0) {
        while (__atomic_load_n(&freed, __ATOMIC_ACQUIRE) != 2);
        assert(pmem_freepages(true) == KERN_PAGES, "official: capacity not restored");
        printf("OFFICIAL PMEM PARALLEL PASS: unique=1024, free=1024\n");
    }
}

static void user_lifecycle(void)
{
    uint64 p[10];
    uint32 baseline = pmem_freepages(false);
    uint64 begin = (uint64)ALLOC_BEGIN + KERN_PAGES * (uint64)PGSIZE;
    printf("=== test_case_2: Phase 1 - Allocate User Pages ===\n");
    for (int i = 0; i < 10; i++) {
        p[i] = (uint64)pmem_alloc(false);
        assert(p[i] >= begin && p[i] < (uint64)ALLOC_END, "official: user bounds");
        memset((void *)p[i], 0xAA, PGSIZE);
        printf("Allocated user page[%d] @ %p\n", i, (void *)p[i]);
    }
    printf("=== Phase 2 - Pre-free Check ===\n");
    printf("Expected allocable: %d, Actual: %d\n", (int)baseline - 10,
           (int)pmem_freepages(false));
    assert(pmem_freepages(false) == baseline - 10, "official: pre-free count");
    printf("=== Phase 3 - Free Pages ===\n");
    for (int i = 0; i < 10; i++) pmem_free(p[i], false);
    printf("=== Phase 4 - Post-free Check ===\n");
    printf("Expected allocable: %d, Actual: %d\n", (int)baseline,
           (int)pmem_freepages(false));
    assert(pmem_freepages(false) == baseline, "official: post-free count");
    printf("=== Phase 5 - Reallocate & Verify Zero ===\n");
    for (int i = 0; i < 10; i++) {
        p[i] = (uint64)pmem_alloc(false);
        for (unsigned j = 0; j < PGSIZE; j++)
            assert(((uint8 *)p[i])[j] == 0, "official: page not zeroed");
        printf("Reallocated page[%d] @ %p: Zero verification passed\n", i, (void *)p[i]);
    }
    /* 原示例最后未回收这10页；这里补上，检查测试自身没有泄漏。 */
    for (int i = 0; i < 10; i++) pmem_free(p[i], false);
    assert(pmem_freepages(false) == baseline, "official: lifecycle leak");
    printf("test_case_2 passed!\nOFFICIAL PMEM LIFECYCLE PASS\n");
}

static void expect_pte(pgtbl_t root, uint64 va, uint64 pa, int perm)
{
    pte_t *pte = vm_getpte(root, va, false);
    assert(pte != NULL && (*pte & PTE_V) && PTE_TO_PA(*pte) == pa,
           "official: wrong PTE address");
    assert((*pte & (PTE_R | PTE_W | PTE_X)) == (uint64)perm,
           "official: wrong permissions");
}

static void mapping_print(void)
{
    uint32 kb = pmem_freepages(true), ub = pmem_freepages(false);
    pgtbl_t root = (pgtbl_t)pmem_alloc(true);
    uint64 mem[5];
    for (int i = 0; i < 5; i++) mem[i] = (uint64)pmem_alloc(false);
    printf("OFFICIAL VM test-1: satp=%x\n\ntest-1 BEFORE\n", r_satp());
    vm_mappages(root, 0, mem[0], PGSIZE, PTE_R);
    vm_mappages(root, PGSIZE * 10, mem[1], PGSIZE / 2, PTE_R | PTE_W);
    vm_mappages(root, PGSIZE * 512, mem[2], PGSIZE - 1, PTE_R | PTE_X);
    /* 使用mem[3]，避免示例mem[2]别名在freeit后悬空。W同时带R。 */
    vm_mappages(root, (uint64)PGSIZE * 512 * 512, mem[3], PGSIZE, PTE_R | PTE_X);
    vm_mappages(root, VA_MAX - PGSIZE, mem[4], PGSIZE, PTE_R | PTE_W);
    expect_pte(root, 0, mem[0], PTE_R);
    expect_pte(root, PGSIZE * 10, mem[1], PTE_R | PTE_W);
    expect_pte(root, PGSIZE * 512, mem[2], PTE_R | PTE_X);
    expect_pte(root, (uint64)PGSIZE * 512 * 512, mem[3], PTE_R | PTE_X);
    expect_pte(root, VA_MAX - PGSIZE, mem[4], PTE_R | PTE_W);
    vm_print(root);
    printf("\ntest-2 AFTER\n");
    vm_mappages(root, 0, mem[0], PGSIZE, PTE_R | PTE_W);
    vm_unmappages(root, PGSIZE * 10, PGSIZE, true);
    vm_unmappages(root, PGSIZE * 512, PGSIZE, true);
    expect_pte(root, 0, mem[0], PTE_R | PTE_W);
    assert(vm_lookup(root, PGSIZE * 10, NULL) == NULL, "official: unmap failed");
    assert(vm_lookup(root, PGSIZE * 512, NULL) == NULL, "official: unmap failed");
    vm_print(root);
    vm_unmappages(root, 0, PGSIZE, true);
    vm_unmappages(root, (uint64)PGSIZE * 512 * 512, PGSIZE, true);
    vm_unmappages(root, VA_MAX - PGSIZE, PGSIZE, true);
    vm_freewalk(root);
    assert(pmem_freepages(true) == kb && pmem_freepages(false) == ub,
           "official: print test leaked pages");
    printf("OFFICIAL VM PRINT PASS: leaves 5 -> 3; all resources restored\n");
}

static void mapping_assertions(void)
{
    uint32 kb = pmem_freepages(true), ub = pmem_freepages(false);
    pgtbl_t root = (pgtbl_t)pmem_alloc(true);
    uint64 a = (uint64)pmem_alloc(false), b = (uint64)pmem_alloc(false);
    vm_mappages(root, 0x100000, a, PGSIZE, PTE_R | PTE_W);
    vm_mappages(root, 0x8000, b, PGSIZE, PTE_R);
    expect_pte(root, 0x100000, a, PTE_R | PTE_W);
    expect_pte(root, 0x8000, b, PTE_R);
    vm_unmappages(root, 0x100000, PGSIZE, true);
    vm_unmappages(root, 0x8000, PGSIZE, true);
    pte_t *pte = vm_getpte(root, 0x100000, false);
    assert(pte != NULL && !(*pte & PTE_V), "official: pte1 still valid");
    pte = vm_getpte(root, 0x8000, false);
    assert(pte != NULL && !(*pte & PTE_V), "official: pte2 still valid");
    vm_freewalk(root);
    assert(pmem_freepages(true) == kb && pmem_freepages(false) == ub,
           "official: assertion test leaked pages");
    printf("test_mapping_and_unmapping passed!\nOFFICIAL VM ASSERT PASS\n");
}

void main(void)
{
    uint64 hart = r_tp();
    /* 引用所有函数，便于在-Wall -Werror下按宏选择不同独立启动用例。 */
    if (LAB2_CASE == 1) physical_parallel(hart);
    else if (hart == 0) {
        print_init();
        pmem_init();
        if (LAB2_CASE == 2 || LAB2_CASE == 3) {
            bool kernel = LAB2_CASE == 2;
            uint32 n = pmem_freepages(kernel);
            printf("OFFICIAL EXHAUST: region=%s, available=%d\n",
                   kernel ? "kernel" : "user", (int)n);
            for (uint32 i = 0; i < n; i++) pmem_alloc(kernel);
            assert(pmem_freepages(kernel) == 0, "official: exhaustion count");
            printf("allocated=%d, free=0; next allocation must panic\n", (int)n);
            pmem_alloc(kernel);
            panic("official: exhaustion not rejected");
        } else if (LAB2_CASE == 4) user_lifecycle();
        else {
            kvm_init();
            kvm_inithart();
            assert((r_satp() >> 60) == 8, "official: paging off");
            if (LAB2_CASE == 5) mapping_print();
            else mapping_assertions();
            sbi_ret_t ret = sbi_hart_start(1, (uint64)_entry, 0);
            assert(ret.error == SBI_SUCCESS, "official: VM CPU1 startup");
            while (!__atomic_load_n(&started, __ATOMIC_ACQUIRE));
            printf("OFFICIAL VM TWO-HART PAGING PASS\n");
        }
    } else if (LAB2_CASE >= 5) {
        kvm_inithart();
        assert((r_satp() >> 60) == 8, "official: CPU1 paging off");
        printf("cpu 1: paging enabled, satp=%x\n", r_satp());
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    }
    while (1) asm volatile("wfi");
}
