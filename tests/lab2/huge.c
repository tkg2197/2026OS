#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

/* 测试入口，仅复制到验证用临时工程；不要替换原工程 main.c。 */
#ifndef TEST_ERROR
#define TEST_ERROR 0
#endif

#define TEST_2M (1ull << 21)
#define TEST_1G (1ull << 30)
#define TEST_VA 0x40000000ull
#define TEST_PA 0x80000000ull
#define TEST_RW (PTE_R | PTE_W)

extern void _entry(void);

#if TEST_ERROR == 0

static int initialized;
static int ready;
static int finished;
static uint32 concurrent_baseline;

static pgtbl_t new_root(void)
{
    return (pgtbl_t)pmem_alloc(true);
}

/* 软件检查；不要解引用 TEST_PA 的模拟 1GiB 物理映射。 */
static void expect_mapping(pgtbl_t root, uint64 va,
                           uint64 pa, int expected_level)
{
    int level = -1;
    pte_t *pte = vm_lookup(root, va, &level);
    assert(pte != NULL && level == expected_level,
           "huge test: wrong leaf level");
    uint64 size = 1ull << (12 + 9 * level);
    assert(PTE_TO_PA(*pte) + (va & (size - 1)) == pa,
           "huge test: wrong address translation");
    assert((*pte & (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D)) ==
           (PTE_V | PTE_R | PTE_W | PTE_A | PTE_D),
           "huge test: wrong leaf flags");
}

static void expect_unmapped(pgtbl_t root, uint64 va)
{
    assert(vm_lookup(root, va, NULL) == NULL,
           "huge test: address unexpectedly mapped");
}

static void expect_kernel_restored(uint32 before)
{
    assert(pmem_freepages(true) == before,
           "huge test: page-table pages leaked");
}

static void test_buddy_before_paging(void)
{
    uint32 before = pmem_freepages(true);
    assert(before == KERN_PAGES, "huge test: buddy initial capacity");
    uint64 a = (uint64)pmem_alloc_order(0, true);
    uint64 b = (uint64)pmem_alloc_order(0, true);
    assert(b == a + PGSIZE, "huge test: buddy split addresses");
    pmem_free_order(a, 0, true);
    pmem_free_order(b, 0, true);
    uint64 pair = (uint64)pmem_alloc_order(1, true);
    assert(pair == a, "huge test: buddy merge");
    pmem_free_order(pair, 1, true);
    uint64 whole = (uint64)pmem_alloc_order(10, true);
    assert(whole == (uint64)ALLOC_BEGIN, "huge test: buddy whole pool");
    pmem_free_order(whole, 10, true);
    expect_kernel_restored(before);
    printf("buddy single-core test passed\n");
}

static void test_2m_saving(void)
{
    uint32 before = pmem_freepages(true);
    pgtbl_t small = new_root();
    pgtbl_t huge = new_root();
    vm_mappages(small, TEST_VA, TEST_PA, TEST_2M, TEST_RW);
    vm_mappages_huge(huge, TEST_VA, TEST_PA, TEST_2M, TEST_RW);
    assert(vm_count_pgtbl(small) == 3, "huge test: 2MiB small count");
    assert(vm_count_pgtbl(huge) == 2, "huge test: 2MiB huge count");
    expect_mapping(small, TEST_VA + TEST_2M - 1,
                   TEST_PA + TEST_2M - 1, 0);
    expect_mapping(huge, TEST_VA + TEST_2M - 1,
                   TEST_PA + TEST_2M - 1, 1);
    assert(vm_getpte(huge, TEST_VA, false) == NULL,
           "huge test: L0 lookup treated huge leaf as table");
    assert(vm_count_pgtbl(huge) == 2,
           "huge test: read-only lookup split huge page");
    vm_print(huge);
    vm_unmappages(small, TEST_VA, TEST_2M, false);
    vm_unmappages(huge, TEST_VA, TEST_2M, false);
    vm_freewalk(small);
    vm_freewalk(huge);
    expect_kernel_restored(before);
    printf("huge 2MiB table-saving test passed: 3 -> 2 pages\n");
}

static void test_1g_and_partial_unmap(void)
{
    uint32 before = pmem_freepages(true);
    pgtbl_t root = new_root();
    uint64 hole = TEST_VA + TEST_2M + 3 * PGSIZE;
    vm_mappages_huge(root, TEST_VA, TEST_PA, TEST_1G, TEST_RW);
    assert(vm_count_pgtbl(root) == 1, "huge test: 1GiB count");
    expect_mapping(root, TEST_VA, TEST_PA, 2);
    expect_mapping(root, TEST_VA + TEST_1G - 1,
                   TEST_PA + TEST_1G - 1, 2);
    assert(vm_getpte(root, TEST_VA, false) == NULL,
           "huge test: L0 lookup accepted 1GiB leaf");
    vm_print(root);

    /* 一页大小的洞：先拆 1GiB，再只拆其中一个 2MiB。 */
    vm_unmappages(root, hole, PGSIZE, false);
    assert(vm_count_pgtbl(root) == 3, "huge test: nested split count");
    expect_unmapped(root, hole);
    expect_mapping(root, TEST_VA, TEST_PA, 1);
    expect_mapping(root, hole - PGSIZE, TEST_PA + hole - TEST_VA - PGSIZE, 0);
    expect_mapping(root, hole + PGSIZE, TEST_PA + hole - TEST_VA + PGSIZE, 0);
    expect_mapping(root, TEST_VA + 2 * TEST_2M,
                   TEST_PA + 2 * TEST_2M, 1);
    expect_mapping(root, TEST_VA + TEST_1G - 1,
                   TEST_PA + TEST_1G - 1, 1);

    /* 解除剩余两个区间，不能把已经解除的洞再次传给 unmappages。 */
    vm_unmappages(root, TEST_VA, hole - TEST_VA, false);
    vm_unmappages(root, hole + PGSIZE,
                  TEST_VA + TEST_1G - hole - PGSIZE, false);
    expect_unmapped(root, TEST_VA);
    expect_unmapped(root, TEST_VA + TEST_1G - 1);
    vm_freewalk(root);
    expect_kernel_restored(before);
    printf("huge 1GiB/nested-split test passed\n");
}

static void test_tail_and_alignment(void)
{
    uint32 before = pmem_freepages(true);
    pgtbl_t root = new_root();
    vm_mappages_huge(root, TEST_VA, TEST_PA, TEST_2M + 1, TEST_RW);
    expect_mapping(root, TEST_VA, TEST_PA, 1);
    expect_mapping(root, TEST_VA + TEST_2M, TEST_PA + TEST_2M, 0);
    expect_mapping(root, TEST_VA + TEST_2M + PGSIZE - 1,
                   TEST_PA + TEST_2M + PGSIZE - 1, 0);
    expect_unmapped(root, TEST_VA + TEST_2M + PGSIZE);
    expect_unmapped(root, TEST_VA + 2 * TEST_2M - PGSIZE);
    assert(vm_count_pgtbl(root) == 3, "huge test: tail table count");
    vm_unmappages(root, TEST_VA, TEST_2M + 1, false);
    vm_freewalk(root);
    expect_kernel_restored(before);

    /* 二者相差4KiB，逐页前进时永远不可能同时2MiB对齐。 */
    root = new_root();
    vm_mappages_huge(root, TEST_VA, TEST_PA + PGSIZE, 2 * TEST_2M, TEST_RW);
    for (uint64 off = 0; off < 2 * TEST_2M; off += PGSIZE)
        expect_mapping(root, TEST_VA + off, TEST_PA + PGSIZE + off, 0);
    assert(vm_count_pgtbl(root) == 4, "huge test: alignment fallback count");
    vm_unmappages(root, TEST_VA, 2 * TEST_2M, false);
    vm_freewalk(root);
    expect_kernel_restored(before);

    /* VA和PA都只4KiB对齐，但同步前进可在中间得到2MiB叶子。 */
    root = new_root();
    vm_mappages_huge(root, TEST_VA + PGSIZE, TEST_PA + PGSIZE,
                     2 * TEST_2M, TEST_RW);
    expect_mapping(root, TEST_VA + PGSIZE, TEST_PA + PGSIZE, 0);
    expect_mapping(root, TEST_VA + TEST_2M, TEST_PA + TEST_2M, 1);
    expect_mapping(root, TEST_VA + 2 * TEST_2M, TEST_PA + 2 * TEST_2M, 0);
    vm_unmappages(root, TEST_VA + PGSIZE, 2 * TEST_2M, false);
    vm_freewalk(root);
    expect_kernel_restored(before);
    printf("huge tail/alignment test passed\n");
}

static void test_remap_and_4k_free(void)
{
    uint32 before = pmem_freepages(true);
    pgtbl_t root = new_root();
    uint64 replaced = TEST_VA + 17 * PGSIZE;
    uint64 replacement_pa = TEST_PA + 2 * TEST_2M;
    vm_mappages_huge(root, TEST_VA, TEST_PA, TEST_2M, TEST_RW);
    vm_mappages(root, replaced, replacement_pa, PGSIZE, TEST_RW);
    assert(vm_count_pgtbl(root) == 3, "huge test: remap split count");
    for (uint64 off = 0; off < TEST_2M; off += PGSIZE) {
        uint64 expected = off == 17 * PGSIZE ? replacement_pa : TEST_PA + off;
        expect_mapping(root, TEST_VA + off, expected, 0);
    }
    vm_unmappages(root, TEST_VA, TEST_2M, false);
    vm_freewalk(root);
    expect_kernel_restored(before);

    /* freeit=true 沿用旧契约：只有独占的用户order0物理页。 */
    uint32 user_before = pmem_freepages(false);
    uint64 user_page = (uint64)pmem_alloc(false);
    root = new_root();
    vm_mappages(root, TEST_VA, user_page, PGSIZE, TEST_RW);
    vm_unmappages(root, TEST_VA, PGSIZE, true);
    assert(pmem_freepages(false) == user_before,
           "huge test: 4KiB freeit failed");
    vm_freewalk(root);
    expect_kernel_restored(before);
    printf("huge remap/4KiB-freeit test passed\n");
}

static void map_kernel_identity(pgtbl_t root)
{
    uint64 data = (uint64)KERNEL_DATA;
    uint64 begin = (uint64)ALLOC_BEGIN;
    uint64 end = (uint64)ALLOC_END;
    vm_mappages(root, UART_BASE, UART_BASE, PGSIZE, TEST_RW);
    vm_mappages(root, PLIC_BASE, PLIC_BASE, PLIC_SIZE, TEST_RW);
    vm_mappages_huge(root, KERNEL_BASE, KERNEL_BASE,
                     data - KERNEL_BASE, PTE_R | PTE_X);
    vm_mappages_huge(root, data, data, begin - data, TEST_RW);
    vm_mappages_huge(root, begin, begin, end - begin, TEST_RW);
}

static void unmap_kernel_identity(pgtbl_t root)
{
    uint64 data = (uint64)KERNEL_DATA;
    uint64 begin = (uint64)ALLOC_BEGIN;
    uint64 end = (uint64)ALLOC_END;
    vm_unmappages(root, UART_BASE, PGSIZE, false);
    vm_unmappages(root, PLIC_BASE, PLIC_SIZE, false);
    vm_unmappages(root, KERNEL_BASE, data - KERNEL_BASE, false);
    vm_unmappages(root, data, begin - data, false);
    vm_unmappages(root, begin, end - begin, false);
}

static void switch_satp(uint64 satp)
{
    /* 编译器屏障和SFENCE都要有：测试编译优化及真实硬件页表。 */
    __sync_synchronize();
    sfence_vma();
    w_satp(satp);
    sfence_vma();
    __sync_synchronize();
}

static void test_hardware_alias(void)
{
    uint32 before = pmem_freepages(true);
    uint32 user_before = pmem_freepages(false);
    uint64 old_satp = r_satp();
    pgtbl_t old_root = (pgtbl_t)((old_satp & ((1ull << 44) - 1)) << 12);
    int level = -1;
    assert(vm_lookup(old_root, UART_BASE, &level) != NULL && level == 0,
           "huge test: UART must remain 4KiB");
    assert(vm_lookup(old_root, PLIC_BASE, &level) != NULL && level == 0,
           "huge test: PLIC must remain 4KiB");
    uint64 ram = ((uint64)ALLOC_BEGIN + TEST_2M - 1) & ~(TEST_2M - 1);
    assert(vm_lookup(old_root, ram, &level) != NULL && level == 1,
           "huge test: kernel RAM not using 2MiB mapping");

    /* Buddy对齐是相对内存池的，order9未必满足绝对2MiB对齐。
     * 借order10的4MiB块，在内部选2MiB对齐区间；最后整体free_order10。 */
    uint64 block = (uint64)pmem_alloc_order(10, false);
    uint64 pa = (block + TEST_2M - 1) & ~(TEST_2M - 1);
    assert(pa + TEST_2M <= block + 2 * TEST_2M,
           "huge test: aligned interval outside buddy block");
    pgtbl_t root = new_root();
    map_kernel_identity(root);
    uint64 alias_va = 0x100000000ull;
    vm_mappages_huge(root, alias_va, pa, TEST_2M, TEST_RW);
    expect_mapping(root, alias_va + TEST_2M - 1, pa + TEST_2M - 1, 1);

    static const uint64 offsets[] = {0, PGSIZE, TEST_2M - PGSIZE, TEST_2M - 8};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
        *(volatile uint64 *)(pa + offsets[i]) = 0x12340000ull + i;
    switch_satp(MAKE_SATP(root));
    assert(r_satp() == MAKE_SATP(root), "huge test: satp switch failed");
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        volatile uint64 *alias = (volatile uint64 *)(alias_va + offsets[i]);
        assert(*alias == 0x12340000ull + i, "huge test: hardware alias read");
        *alias = 0x56780000ull + i;
        assert(*(volatile uint64 *)(pa + offsets[i]) == 0x56780000ull + i,
               "huge test: hardware alias write");
    }

    /* 当前根正在使用，且上述读取已让2MiB翻译进入TLB。
     * 拆出4KiB洞再映射到另一PA，验证sfence确实清掉旧大页翻译。
     * 不读取洞本身：本实验尚未安装用于捕获page fault的异常入口。 */
    *(volatile uint64 *)(pa + 2 * PGSIZE) = 0x9abc0002ull;
    vm_unmappages(root, alias_va + PGSIZE, PGSIZE, false);
    expect_unmapped(root, alias_va + PGSIZE);
    assert(*(volatile uint64 *)alias_va == 0x56780000ull,
           "huge test: active split damaged left neighbor");
    assert(*(volatile uint64 *)(alias_va + 2 * PGSIZE) == 0x9abc0002ull,
           "huge test: active split damaged right neighbor");
    vm_mappages(root, alias_va + PGSIZE, pa + 2 * PGSIZE, PGSIZE, TEST_RW);
    expect_mapping(root, alias_va + PGSIZE, pa + 2 * PGSIZE, 0);
    assert(*(volatile uint64 *)(alias_va + PGSIZE) == 0x9abc0002ull,
           "huge test: active remap retained old TLB translation");
    *(volatile uint64 *)(alias_va + PGSIZE) = 0xdef00002ull;
    assert(*(volatile uint64 *)(pa + 2 * PGSIZE) == 0xdef00002ull,
           "huge test: active remap wrote wrong physical page");
    assert(*(volatile uint64 *)(pa + PGSIZE) == 0x56780001ull,
           "huge test: active remap wrote old physical page");
    assert(*(volatile uint64 *)alias_va == 0x56780000ull &&
           *(volatile uint64 *)(alias_va + 2 * PGSIZE) == 0xdef00002ull,
           "huge test: active remap damaged surrounding translations");
    printf("huge active split/remap/TLB test passed\n");
    switch_satp(old_satp);
    assert(r_satp() == old_satp, "huge test: original satp not restored");

    /* 离开测试页表并刷新TLB后，才允许释放它的页表页和物理块。 */
    vm_unmappages(root, alias_va, TEST_2M, false);
    unmap_kernel_identity(root);
    vm_freewalk(root);
    pmem_free_order(block, 10, false);
    expect_kernel_restored(before);
    assert(pmem_freepages(false) == user_before,
           "huge test: hardware alias block leaked");
    printf("huge hardware satp/alias test passed\n");
}

static void test_heap_smoke(void)
{
    uint32 before = pmem_freepages(true);
    uint8 *small = kmalloc(64);
    uint8 *page = kmalloc(4096);
    assert(small != NULL && page != NULL, "huge test: heap allocation");
    memset(small, 0x3a, 64);
    memset(page, 0xc5, 4096);
    for (unsigned i = 0; i < 64; i++)
        assert(small[i] == 0x3a, "huge test: heap small overwritten");
    for (unsigned i = 0; i < 4096; i++)
        assert(page[i] == 0xc5, "huge test: heap page overwritten");
    kfree(small);
    kfree(page);
    assert(kheap_pages() == 0, "huge test: heap pages not reclaimed");
    expect_kernel_restored(before);
    printf("heap compatibility smoke test passed\n");
}

static void test_buddy_parallel(uint64 hartid)
{
    assert((r_satp() >> 60) == 8, "huge test: hart paging disabled");
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) < NCPU)
        ;
    for (unsigned round = 0; round < 32; round++) {
        unsigned order = (round + hartid) % 4;
        uint64 len = (uint64)PGSIZE << order;
        uint8 *block = pmem_alloc_order(order, true);
        for (uint64 i = 0; i < len; i++)
            assert(block[i] == 0, "huge test: buddy block not zero");
        memset(block, (uint8)(hartid + 1), len);
        for (uint64 i = 0; i < len; i++)
            assert(block[i] == hartid + 1, "huge test: buddy block overwritten");
        pmem_free_order((uint64)block, order, true);
    }
    __atomic_add_fetch(&finished, 1, __ATOMIC_RELEASE);
    if (hartid == 0) {
        while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) < NCPU)
            ;
        expect_kernel_restored(concurrent_baseline);
        printf("buddy two-core test passed\n");
        printf("HUGE ALL TESTS PASSED\n");
    }
}

void main(void)
{
    uint64 hartid = r_tp();
    if (hartid == 0) {
        print_init();
        pmem_init();
        test_buddy_before_paging();
        kvm_init();
        kvm_inithart();
        kheap_init();
        assert(NCPU == 2, "huge test: run with two CPUs");
        test_2m_saving();
        test_1g_and_partial_unmap();
        test_tail_and_alignment();
        test_remap_and_4k_free();
        test_hardware_alias();
        test_heap_smoke();
        concurrent_baseline = pmem_freepages(true);
        printf("cpu %d: paging enabled, satp=%x\n", (int)hartid, r_satp());
        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
        for (uint64 id = 1; id < NCPU; id++) {
            sbi_ret_t ret = sbi_hart_start(id, (uint64)_entry, 0);
            assert(ret.error == SBI_SUCCESS, "huge test: sbi_hart_start");
        }
    } else {
        while (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE))
            ;
        kvm_inithart();
        printf("cpu %d: paging enabled, satp=%x\n", (int)hartid, r_satp());
    }
    test_buddy_parallel(hartid);
    while (1)
        asm volatile("wfi");
}

#else

void main(void)
{
    if (r_tp() == 0) {
        print_init();
        pmem_init();
        kvm_init();
        kvm_inithart();
        pgtbl_t root = (pgtbl_t)pmem_alloc(true);
        vm_mappages_huge(root, TEST_VA, TEST_PA, TEST_2M, TEST_RW);
#if TEST_ERROR == 1
        vm_unmappages(root, TEST_VA, TEST_2M, true);
#elif TEST_ERROR == 2
        vm_freewalk(root);
#else
#error TEST_ERROR must be 0, 1, or 2
#endif
        panic("huge test: invalid operation accepted");
    }
    while (1)
        asm volatile("wfi");
}

#endif
