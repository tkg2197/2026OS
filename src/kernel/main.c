#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"

extern void _entry(void);

static volatile int initialized = 0;
static int ready = 0;
static int finished = 0;
static uint32 concurrent_baseline;

// 在分页初始化之前测试，此时内核区全部空闲。
static void buddy_test_single(void)
{
    uint32 before = pmem_freepages(true);
    assert(before == KERN_PAGES, "buddy test: initial capacity");

    void *a = pmem_alloc_order(0, true);
    void *b = pmem_alloc_order(0, true);

    assert((uint64)b == (uint64)a + PGSIZE,
           "buddy test: split addresses");

    pmem_free_order((uint64)a, 0, true);
    pmem_free_order((uint64)b, 0, true);

    // 两个单页伙伴释放后，应合并成两页块。
    void *c = pmem_alloc_order(1, true);
    assert(c == a, "buddy test: merge failed");
    pmem_free_order((uint64)c, 1, true);

    // 全部释放后，应能重新分配整个 1024 页内核区。
    void *whole = pmem_alloc_order(10, true);
    assert((uint64)whole == (uint64)ALLOC_BEGIN,
           "buddy test: full-region merge failed");

    pmem_free_order((uint64)whole, 10, true);

    assert(pmem_freepages(true) == before,
           "buddy test: single-core capacity");

    printf("buddy single-core test passed\n");
}

static void buddy_test_parallel(uint64 hartid)
{
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);

    // 两个 CPU 都到齐后，再开始测试。
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) < NCPU)
        ;

    for (unsigned round = 0; round < 32; round++)
    {
        unsigned order = (round + hartid) % 4;
        uint64 len = (uint64)PGSIZE << order;
        uint8 mark = (uint8)(hartid + 1);

        uint8 *block = pmem_alloc_order(order, true);

        for (uint64 i = 0; i < len; i++)
            assert(block[i] == 0, "buddy test: block not zero");

        memset(block, mark, len);

        for (uint64 i = 0; i < len; i++)
            assert(block[i] == mark,
                   "buddy test: block overwritten");

        pmem_free_order((uint64)block, order, true);
    }

    __atomic_add_fetch(&finished, 1, __ATOMIC_RELEASE);

    if (hartid == 0)
    {
        while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) < NCPU)
            ;

        assert(pmem_freepages(true) == concurrent_baseline,
               "buddy test: concurrent capacity");

        printf("buddy two-core test passed\n");
    }
}

void main(void)
{
    uint64 hartid = r_tp();

    if (hartid == 0)
    {
        print_init();
        printf("cpu %d is booting with OpenSBI!\n", (int)hartid);
        pmem_init();

        buddy_test_single();

        kvm_init();
        kvm_inithart();
        kheap_init();

        // Shared trap state must be ready before secondary harts start.
        trap_kernel_init();

        printf("cpu %d: paging enabled, satp=%x\n",
               (int)hartid, r_satp());

        // 页表占用的页属于永久分配，基准应在其初始化后记录。
        concurrent_baseline = pmem_freepages(true);

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

    buddy_test_parallel(hartid);

    // 以下初始化是每个CPU各自独有的

    // PLIC开关, 中断入口stvec, 中断使能位
    trap_kernel_inithart();

    // 设置本CPU的第一次时钟事件, 并打开S-mode时钟中断
    timer_init();

    while (1)
        asm volatile("wfi");
}
