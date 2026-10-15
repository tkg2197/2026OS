#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

// 0 跑正常测试；1~9 分别跑一个应该 panic 的错误输入。
#define BUDDY_BAD_CASE 0
#define HELD 48
#define ROUNDS 12

extern void _entry(void);
static volatile int initialized;
static volatile int ready[NCPU], released[NCPU];
static volatile int allow_free;
static uint64 held[NCPU][HELD]; // 放在全局区，避免占满小容量启动栈。
static unsigned held_order[NCPU][HELD];
static uint8 seen[PHYS_MEM_SIZE / PGSIZE];

static void check_zero(void *block, unsigned order)
{
    uint8 *p = block;
    for (uint64 i = 0; i < ((uint64)PGSIZE << order); i++)
        assert(p[i] == 0, "test: allocated block is not zero");
}

static void check_mark(void *block, unsigned order, uint8 mark)
{
    uint8 *p = block;
    for (uint64 i = 0; i < ((uint64)PGSIZE << order); i++)
        assert(p[i] == mark, "test: block contents changed");
}

static void single_test(void)
{
    uint32 baseline = pmem_freepages(true);
    assert(baseline == KERN_PAGES, "test: initial kernel page count");

    void *a = pmem_alloc_order(0, true);
    void *b = pmem_alloc_order(0, true);
    void *c = pmem_alloc_order(1, true);
    assert((uint64)b == (uint64)a + PGSIZE, "test: split sibling addresses");
    check_zero(a, 0);
    check_zero(b, 0);
    check_zero(c, 1);
    memset(c, 0x6a, PGSIZE * 2);
    pmem_free_order((uint64)a, 0, true);
    pmem_free_order((uint64)b, 0, true);
    void *d = pmem_alloc_order(1, true);
    assert(d == a, "test: siblings did not merge");
    check_zero(d, 1);
    check_mark(c, 1, 0x6a);
    pmem_free_order((uint64)d, 1, true);
    pmem_free_order((uint64)c, 1, true);
    assert(pmem_freepages(true) == baseline, "test: split/merge capacity");
    void *whole = pmem_alloc_order(10, true);
    assert((uint64)whole == (uint64)ALLOC_BEGIN, "test: full region merge");
    check_zero(whole, 10);
    pmem_free_order((uint64)whole, 10, true);
    printf("BUDDY PASS: split, merge, zeroing, order-10 recovery\n");

    for (unsigned round = 0; round < 16; round++)
    {
        for (unsigned i = 0; i < HELD; i++)
        {
            unsigned order = (i + round) % 6; // 每轮消耗 504 页，小于内核区 1024 页。
            held_order[0][i] = order;
            held[0][i] = (uint64)pmem_alloc_order(order, true);
            check_zero((void *)held[0][i], order);
            memset((void *)held[0][i], i + 1, (uint64)PGSIZE << order);
        }
        for (unsigned pass = 0; pass < 2; pass++)
            for (unsigned i = pass; i < HELD; i += 2)
            {
                check_mark((void *)held[0][i], held_order[0][i], i + 1);
                pmem_free_order(held[0][i], held_order[0][i], true);
            }
        assert(pmem_freepages(true) == baseline, "test: mixed-order capacity");
        whole = pmem_alloc_order(10, true); // 容量恢复还不够，必须真的恢复成一个大块。
        pmem_free_order((uint64)whole, 10, true);
    }
    printf("BUDDY PASS: 16 mixed-order rounds, contents and capacity\n");

    uint32 user_baseline = pmem_freepages(false);
    assert((user_baseline & (user_baseline - 1)) != 0,
           "test: this ELF should provide a non-power-of-two user region");
    uint64 user_begin = (uint64)ALLOC_BEGIN + (uint64)KERN_PAGES * PGSIZE;
    uint64 head = 0;
    for (uint32 i = 0; i < user_baseline; i++)
    {
        uint64 pa = (uint64)pmem_alloc(false);
        assert(pa >= user_begin && pa < (uint64)ALLOC_END && pa % PGSIZE == 0,
               "test: user page bounds");
        uint32 index = (pa - user_begin) / PGSIZE;
        assert(seen[index] == 0, "test: duplicate user page");
        seen[index] = 1;
        *(uint64 *)pa = head; // 已分配页由测试独占，暂用它保存下一个待释放页。
        head = pa;
    }
    assert(pmem_freepages(false) == 0, "test: user region not exhausted");
    while (head != 0)
    {
        uint64 next = *(uint64 *)head; // 必须先保存 next；释放后页首会被分配器改写。
        pmem_free(head, false);
        head = next;
    }
    assert(pmem_freepages(false) == user_baseline, "test: user tail lost");
    whole = pmem_alloc_order(14, false);
    pmem_free_order((uint64)whole, 14, false);
    printf("BUDDY PASS: user-region %d pages exhausted and restored, tail preserved\n",
           (int)user_baseline);

    // 每个伙伴对只分配其中一页，空闲总量不少，却没有任何完整的 2 页块。
    for (unsigned i = 0; i < KERN_PAGES; i++)
    {
        uint64 pa = (uint64)pmem_alloc(true);
        *(uint64 *)pa = 0;
    }
    for (unsigned i = 0; i < KERN_PAGES; i += 2)
        pmem_free((uint64)ALLOC_BEGIN + (uint64)i * PGSIZE, true);
    assert(pmem_freepages(true) == KERN_PAGES / 2, "test: fragmentation count");
    for (unsigned i = 1; i < KERN_PAGES; i += 2)
        pmem_free((uint64)ALLOC_BEGIN + (uint64)i * PGSIZE, true);
    whole = pmem_alloc_order(10, true);
    pmem_free_order((uint64)whole, 10, true);
    printf("BUDDY PASS: fragmented region recovered to one order-10 block\n");
}

static void bad_test(unsigned which)
{
    if (which == 1)
        pmem_alloc_order(16, true);
    else if (which == 2)
    {
        uint64 pa = (uint64)pmem_alloc(true);
        pmem_free(pa + 1, true);
    }
    else if (which == 3)
    {
        uint64 pa = (uint64)pmem_alloc(true);
        pmem_free(pa, false);
    }
    else if (which == 4)
    {
        uint64 pa = (uint64)pmem_alloc_order(2, true);
        pmem_free(pa + PGSIZE, true);
    }
    else if (which == 5)
    {
        uint64 pa = (uint64)pmem_alloc_order(2, true);
        pmem_free_order(pa, 1, true);
    }
    else if (which == 6)
    {
        uint64 pa = (uint64)pmem_alloc(true);
        pmem_free(pa, true);
        pmem_free(pa, true);
    }
    else if (which == 7)
    {
        pmem_alloc_order(10, true);
        pmem_alloc(true);
    }
    else if (which == 8)
    {
        for (unsigned i = 0; i < KERN_PAGES; i++)
            pmem_alloc(true);
        for (unsigned i = 0; i < KERN_PAGES; i += 2)
            pmem_free((uint64)ALLOC_BEGIN + (uint64)i * PGSIZE, true);
        assert(pmem_freepages(true) == 512, "test: fragmentation setup");
        pmem_alloc_order(1, true); // 512 页空闲，但不存在 2 页伙伴块。
    }
    else if (which == 9)
    {
        uint32 count = pmem_freepages(false);
        for (uint32 i = 0; i < count; i++)
            pmem_alloc(false);
        pmem_alloc(false);
    }
    panic("test: expected panic did not occur");
}

static void concurrent_test(uint64 hartid)
{
    for (int round = 0; round < ROUNDS; round++)
    {
        uint32 baseline = 0;
        if (hartid == 0)
            baseline = pmem_freepages(true);
        // CPU1 等待 CPU0 发布新轮次，避免在基准计数之前分配。
        if (hartid == 0)
            __atomic_store_n(&allow_free, -(round + 1), __ATOMIC_RELEASE);
        else
            while (__atomic_load_n(&allow_free, __ATOMIC_ACQUIRE) != -(round + 1))
                ;
        for (unsigned i = 0; i < HELD; i++)
        {
            unsigned order = (i + round + hartid) % 4; // 每核 180 页，共 360 页。
            uint8 mark = 1 + (uint8)(hartid * HELD + i);
            held_order[hartid][i] = order;
            held[hartid][i] = (uint64)pmem_alloc_order(order, true);
            check_zero((void *)held[hartid][i], order);
            memset((void *)held[hartid][i], mark, (uint64)PGSIZE << order);
        }
        __atomic_store_n(&ready[hartid], round + 1, __ATOMIC_RELEASE);
        if (hartid == 0)
        {
            while (__atomic_load_n(&ready[1], __ATOMIC_ACQUIRE) != round + 1)
                ;
            for (unsigned x = 0; x < NCPU * HELD; x++)
                for (unsigned y = x + 1; y < NCPU * HELD; y++)
                {
                    uint64 a = held[x / HELD][x % HELD];
                    uint64 b = held[y / HELD][y % HELD];
                    uint64 aend = a + ((uint64)PGSIZE << held_order[x / HELD][x % HELD]);
                    uint64 bend = b + ((uint64)PGSIZE << held_order[y / HELD][y % HELD]);
                    assert(aend <= b || bend <= a, "test: live blocks overlap");
                }
            __atomic_store_n(&allow_free, round + 1, __ATOMIC_RELEASE);
        }
        else
            while (__atomic_load_n(&allow_free, __ATOMIC_ACQUIRE) != round + 1)
                ;
        for (int i = HELD - 1; i >= 0; i--)
        {
            uint8 mark = 1 + (uint8)(hartid * HELD + i);
            check_mark((void *)held[hartid][i], held_order[hartid][i], mark);
            pmem_free_order(held[hartid][i], held_order[hartid][i], true);
        }
        __atomic_store_n(&released[hartid], round + 1, __ATOMIC_RELEASE);
        if (hartid == 0)
        {
            while (__atomic_load_n(&released[1], __ATOMIC_ACQUIRE) != round + 1)
                ;
            assert(pmem_freepages(true) == baseline, "test: concurrent capacity");
        }
    }
    if (hartid == 0)
        printf("BUDDY PASS: 12 two-hart rounds, no overlaps, contents and capacity\n");
}

void main(void)
{
    uint64 hartid = r_tp();
    if (hartid == 0)
    {
        print_init();
        pmem_init();
        if (BUDDY_BAD_CASE != 0)
            bad_test(BUDDY_BAD_CASE);
        single_test(); // 只有主核在运行，先验证基本算法。
        kvm_init();
        kvm_inithart(); // 继续使用原 kvm.c，证明 order-0 兼容接口正常。
        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
        for (uint64 id = 1; id < NCPU; id++)
        {
            sbi_ret_t ret = sbi_hart_start(id, (uint64)_entry, 0);
            assert(ret.error == SBI_SUCCESS, "test: start secondary hart");
        }
    }
    else
    {
        while (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE))
            ;
        kvm_inithart();
    }
    concurrent_test(hartid);
    if (hartid == 0)
        printf("BUDDY ALL TESTS PASSED\n");
    while (1)
        asm volatile("wfi");
}
