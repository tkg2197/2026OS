#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"

/*
 * 将此文件复制到临时工程的 src/kernel/main.c 后运行。
 * HEAP_BAD_CASE=0 运行正常测试；1/2/3 必须分别启动，预期触发 panic。
 * 原工程的 main.c 不需要为这些验证而修改。
 */
#ifndef HEAP_BAD_CASE
#define HEAP_BAD_CASE 0
#endif

extern void _entry(void);

#if HEAP_BAD_CASE == 0

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define STRESS_SLOTS 12
#define STRESS_ROUNDS 64

static volatile int initialized;
static uint32 concurrent_baseline;
static void *exhausted_blocks[KERN_PAGES];

typedef struct heap_test_slot {
    uint8 *ptr;
    uint32 size;
    uint8 mark;
} heap_test_slot_t;

static heap_test_slot_t shared_slots[NCPU][STRESS_SLOTS];
static int barrier_arrivals;
static int barrier_generation;

static void check_bytes(uint8 *ptr, uint32 size, uint8 mark)
{
    for (uint32 i = 0; i < size; i++)
        assert(ptr[i] == mark, "heap test: live object overwritten");
}

static void check_restored(uint32 baseline)
{
    assert(kheap_pages() == 0, "heap test: empty pages retained");
    assert(pmem_freepages(true) == baseline,
           "heap test: physical pages not restored");
}

static void check_separate(uint8 *a, uint32 a_size,
                           uint8 *b, uint32 b_size)
{
    uint64 a_begin = (uint64)a;
    uint64 b_begin = (uint64)b;

    assert(a_begin + a_size <= b_begin || b_begin + b_size <= a_begin,
           "heap test: live objects overlap");
}

static void heap_test_boundaries(void)
{
    static const uint32 sizes[] = {
        1, 31, 32, 33, 64, 65, 2048, 2049, 4096
    };
    uint8 *blocks[ARRAY_LEN(sizes)];
    uint32 baseline = pmem_freepages(true);

    assert(kheap_pages() == 0, "heap test: eager page allocation");

    for (unsigned i = 0; i < ARRAY_LEN(sizes); i++) {
        blocks[i] = kmalloc(sizes[i]);
        assert(blocks[i] != NULL, "heap test: boundary allocation failed");
        assert((uint64)blocks[i] % 16 == 0,
               "heap test: default alignment failed");

        for (unsigned j = 0; j < i; j++)
            check_separate(blocks[i], sizes[i], blocks[j], sizes[j]);

        memset(blocks[i], (uint8)(i + 1), sizes[i]);
    }

    for (unsigned i = 0; i < ARRAY_LEN(sizes); i++)
        check_bytes(blocks[i], sizes[i], (uint8)(i + 1));

    kheap_print();

    for (unsigned i = ARRAY_LEN(sizes); i > 0; i--)
        kfree(blocks[i - 1]);

    check_restored(baseline);
    printf("heap boundary/content test passed\n");
}

static void heap_test_growth_and_reclaim(void)
{
    uint8 *blocks[65];
    uint32 baseline = pmem_freepages(true);

    for (unsigned i = 0; i < 65; i++) {
        blocks[i] = kmalloc(64);
        assert(blocks[i] != NULL, "heap test: growth allocation failed");

        for (unsigned j = 0; j < i; j++)
            check_separate(blocks[i], 64, blocks[j], 64);

        memset(blocks[i], (uint8)(i + 1), 64);

        if (i == 63) {
            assert(kheap_pages() == 1, "heap test: 64 blocks need one page");
            assert(pmem_freepages(true) + 1 == baseline,
                   "heap test: first page usage wrong");
        }
    }

    assert(kheap_pages() == 2, "heap test: 65 blocks need two pages");
    assert(pmem_freepages(true) + 2 == baseline,
           "heap test: growth page usage wrong");
    kheap_print();

    kfree(blocks[0]);
    assert(kheap_pages() == 2, "heap test: live page reclaimed");

    for (unsigned i = 1; i < 63; i++) {
        check_bytes(blocks[i], 64, (uint8)(i + 1));
        kfree(blocks[i]);
    }

    assert(kheap_pages() == 2, "heap test: last live block ignored");
    check_bytes(blocks[63], 64, 64);
    kfree(blocks[63]);

    assert(kheap_pages() == 1, "heap test: empty first page not reclaimed");
    assert(pmem_freepages(true) + 1 == baseline,
           "heap test: reclaimed page not returned to buddy");

    check_bytes(blocks[64], 64, 65);
    kfree(blocks[64]);
    check_restored(baseline);

    /* 回收整页后重新申请：只检查可用性，不要求返回同一个地址。 */
    uint8 *again = kmalloc(64);
    assert(again != NULL, "heap test: allocation after reclaim failed");
    memset(again, 0x5a, 64);
    check_bytes(again, 64, 0x5a);
    kfree(again);
    check_restored(baseline);

    printf("heap growth/reclaim test passed\n");
}

static void heap_test_bitmap_boundary(void)
{
    uint8 *blocks[129];
    uint32 baseline = pmem_freepages(true);
    for (unsigned i = 0; i < 129; i++) {
        blocks[i] = kmalloc(32);
        assert(blocks[i] != NULL, "heap test: bitmap allocation failed");
        for (unsigned j = 0; j < i; j++)
            check_separate(blocks[i], 32, blocks[j], 32);
        memset(blocks[i], (uint8)(i + 1), 32);
    }
    assert(kheap_pages() == 2, "heap test: bitmap page usage wrong");
    for (unsigned i = 0; i < 129; i++)
        check_bytes(blocks[i], 32, (uint8)(i + 1));

    /* 分别复用两个位图字的边界位置，保留同页其他对象。 */
    static const unsigned edges[] = {63, 64, 127};
    for (unsigned e = 0; e < ARRAY_LEN(edges); e++) {
        unsigned i = edges[e];
        kfree(blocks[i]);
        blocks[i] = kmalloc(32);
        assert(blocks[i] != NULL, "heap test: bitmap reuse failed");
        memset(blocks[i], (uint8)(i + 1), 32);
    }
    for (unsigned i = 0; i < 129; i++) {
        check_bytes(blocks[i], 32, (uint8)(i + 1));
        kfree(blocks[i]);
    }
    check_restored(baseline);
    printf("heap bitmap-boundary test passed\n");
}

static void heap_test_alignment(void)
{
    uint8 *blocks[13];
    uint32 sizes[13];
    uint32 baseline = pmem_freepages(true);

    for (unsigned i = 0; i < 13; i++) {
        uint64 align = (uint64)1 << i;
        sizes[i] = 1 + i * 13;
        blocks[i] = kmalloc_aligned(sizes[i], align);

        assert(blocks[i] != NULL, "heap test: aligned allocation failed");
        assert((uint64)blocks[i] % align == 0,
               "heap test: requested alignment failed");

        for (unsigned j = 0; j < i; j++)
            check_separate(blocks[i], sizes[i], blocks[j], sizes[j]);

        memset(blocks[i], (uint8)(i + 101), sizes[i]);
    }

    for (unsigned i = 0; i < 13; i++)
        check_bytes(blocks[i], sizes[i], (uint8)(i + 101));

    for (unsigned i = 0; i < 13; i++)
        kfree(blocks[i]);

    check_restored(baseline);
    printf("heap alignment test passed\n");
}

/* 32B 档的一页有 128 块，专门覆盖 used[1] 中的后 64 个分配位。 */
static void heap_test_bitmap_capacity(void)
{
    uint8 *blocks[129];
    uint32 baseline = pmem_freepages(true);

    for (unsigned i = 0; i < 129; i++) {
        blocks[i] = kmalloc(32);
        assert(blocks[i] != NULL, "heap test: bitmap capacity allocation failed");

        for (unsigned j = 0; j < i; j++)
            check_separate(blocks[i], 32, blocks[j], 32);

        memset(blocks[i], (uint8)(i + 1), 32);

        if (i == 127)
            assert(kheap_pages() == 1,
                   "heap test: 32-byte class did not fit 128 blocks");
    }

    assert(kheap_pages() == 2, "heap test: 129 small blocks need two pages");

    for (unsigned i = 0; i < 129; i++)
        check_bytes(blocks[i], 32, (uint8)(i + 1));

    for (unsigned i = 129; i > 0; i--)
        kfree(blocks[i - 1]);

    check_restored(baseline);
    printf("heap 128-bit-bitmap test passed\n");
}

static void heap_test_invalid_requests(void)
{
    uint32 baseline = pmem_freepages(true);

    assert(kmalloc(0) == NULL, "heap test: zero size accepted");
    assert(kmalloc(4097) == NULL, "heap test: oversized request accepted");
    assert(kmalloc(~(uint64)0) == NULL, "heap test: huge size accepted");
    assert(kmalloc_aligned(0, 16) == NULL,
           "heap test: aligned zero size accepted");
    assert(kmalloc_aligned(4097, 16) == NULL,
           "heap test: aligned oversized request accepted");
    assert(kmalloc_aligned(1, 0) == NULL,
           "heap test: zero alignment accepted");
    assert(kmalloc_aligned(1, 3) == NULL,
           "heap test: non-power-of-two alignment accepted");
    assert(kmalloc_aligned(1, 8192) == NULL,
           "heap test: oversized alignment accepted");
    assert(kmalloc_aligned(1, ~(uint64)0) == NULL,
           "heap test: huge alignment accepted");

    kfree(NULL);
    check_restored(baseline);
    printf("heap invalid-request test passed\n");
}

static void heap_test_exhaustion(void)
{
    uint32 baseline = pmem_freepages(true);
    uint32 count = 0;

    assert(baseline > 0 && baseline <= KERN_PAGES,
           "heap test: exhaustion baseline invalid");

    while (count < KERN_PAGES) {
        uint8 *block = kmalloc(4096);

        if (block == NULL)
            break;

        exhausted_blocks[count] = block;
        assert((uint64)block % 16 == 0,
               "heap test: exhausted block misaligned");
        memset(block, (uint8)(1 + count % 251), 4096);
        count++;
    }

    assert(count == baseline, "heap test: wrong exhaustion capacity");
    assert(pmem_freepages(true) == 0, "heap test: exhaustion left free pages");
    assert(kheap_pages() == baseline, "heap test: exhausted page count wrong");
    assert(kmalloc(4096) == NULL, "heap test: exhaustion did not return NULL");
    assert(kmalloc(1) == NULL, "heap test: other class ignored exhaustion");
    assert(pmem_try_alloc(true) == NULL,
           "heap test: physical try allocation ignored exhaustion");

    /* 失败请求后，所有已分配的对象仍应完好。 */
    for (uint32 i = 0; i < count; i++)
        check_bytes(exhausted_blocks[i], 4096, (uint8)(1 + i % 251));

    /* 打乱释放顺序，也覆盖 buddy 逐步合并的路径。 */
    for (uint32 i = 0; i < count; i += 2)
        kfree(exhausted_blocks[i]);
    for (uint32 i = 1; i < count; i += 2)
        kfree(exhausted_blocks[i]);

    check_restored(baseline);
    printf("heap exhaustion/recovery test passed\n");
}

/* 可重复使用的屏障：每轮必须等全部 CPU 到齐，才能进入下一阶段。 */
static void heap_test_barrier(void)
{
    int generation = __atomic_load_n(&barrier_generation, __ATOMIC_ACQUIRE);

    if (__atomic_add_fetch(&barrier_arrivals, 1, __ATOMIC_ACQ_REL) == NCPU) {
        __atomic_store_n(&barrier_arrivals, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&barrier_generation, generation + 1, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&barrier_generation, __ATOMIC_ACQUIRE)
               == generation)
            ;
    }
}

static void heap_test_parallel(uint64 hartid)
{
    static const uint32 sizes[] = {
        1, 31, 32, 33, 64, 65, 127, 128, 129, 255, 256, 257,
        511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049, 4095, 4096
    };
    uint64 other = (hartid + 1) % NCPU;

    heap_test_barrier();

    for (unsigned round = 0; round < STRESS_ROUNDS; round++) {
        for (unsigned i = 0; i < STRESS_SLOTS; i++) {
            heap_test_slot_t *slot = &shared_slots[hartid][i];
            uint64 align = (uint64)1 << ((round + i + hartid) % 13);

            slot->size = sizes[(round * 3 + i + hartid) % ARRAY_LEN(sizes)];
            slot->mark = (uint8)(1 + (17 + hartid * 97 + i * 7 + round * 3) % 251);
            slot->ptr = kmalloc_aligned(slot->size, align);

            assert(slot->ptr != NULL, "heap test: parallel allocation failed");
            assert((uint64)slot->ptr % align == 0,
                   "heap test: parallel alignment failed");
            memset(slot->ptr, slot->mark, slot->size);
        }

        /* 双方完成分配与写入后，再读取另一核发布的对象。 */
        heap_test_barrier();

        for (unsigned i = 0; i < STRESS_SLOTS; i++) {
            heap_test_slot_t *slot = &shared_slots[other][i];
            check_bytes(slot->ptr, slot->size, slot->mark);

            for (unsigned j = 0; j < STRESS_SLOTS; j++) {
                heap_test_slot_t *own = &shared_slots[hartid][j];
                check_separate(slot->ptr, slot->size, own->ptr, own->size);
            }
        }

        /* 全部核完成内容检查后才能释放，避免核验与释放互相竞态。 */
        heap_test_barrier();

        for (unsigned i = 0; i < STRESS_SLOTS; i++)
            kfree(shared_slots[other][i].ptr);

        /* 跨核释放完成后，下一轮才可以重写共享槽位。 */
        heap_test_barrier();
    }

    if (hartid == 0) {
        check_restored(concurrent_baseline);
        kheap_print();
        printf("heap two-core/cross-core-free test passed\n");
        printf("HEAP ALL TESTS PASSED\n");
    }
}

void main(void)
{
    uint64 hartid = r_tp();

    if (hartid == 0) {
        print_init();
        pmem_init();
        kvm_init();
        kvm_inithart();
        kheap_init();

        assert(NCPU == 2, "heap test: run with two CPUs");

        heap_test_boundaries();
        heap_test_growth_and_reclaim();
        heap_test_bitmap_boundary();
        heap_test_alignment();
        heap_test_bitmap_capacity();
        heap_test_invalid_requests();
        heap_test_exhaustion();

        concurrent_baseline = pmem_freepages(true);
        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);

        for (uint64 id = 1; id < NCPU; id++) {
            sbi_ret_t ret = sbi_hart_start(id, (uint64)_entry, 0);
            assert(ret.error == SBI_SUCCESS, "sbi_hart_start failed");
        }
    } else {
        while (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE))
            ;

        kvm_inithart();
    }

    heap_test_parallel(hartid);

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
        kheap_init();

#if HEAP_BAD_CASE == 1
        /* 留一个活对象，确保第二次 free 检查同一页的分配位图。 */
        void *ptr = kmalloc(64);
        void *keep_page_live = kmalloc(64);
        assert(ptr != NULL && keep_page_live != NULL,
               "heap bad test: setup allocation failed");
        kfree(ptr);
        kfree(ptr);
#elif HEAP_BAD_CASE == 2
        uint8 *ptr = kmalloc(64);
        assert(ptr != NULL, "heap bad test: setup allocation failed");
        kfree(ptr + 1);
#elif HEAP_BAD_CASE == 3
        uint64 local = 0;
        kfree(&local);
#else
#error HEAP_BAD_CASE must be 0, 1, 2, or 3
#endif

        panic("heap bad test: invalid free was not rejected");
    }

    while (1)
        asm volatile("wfi");
}

#endif
