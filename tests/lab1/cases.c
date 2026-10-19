#include "arch/mod.h"
#include "lib/mod.h"
#include "lock/mod.h"

#ifndef LAB1_CASE
#define LAB1_CASE 1
#endif
#define LOOPS 1000000
#define RACE_ROUNDS 5
#define PRINT_ROWS 32

extern char CPU_stack[];
extern int main(void);
extern void lab1_mtrap(void);
uint64 observed_cause[NCPU];
static uint64 boot_status[NCPU], boot_epc[NCPU], boot_pmp[NCPU];
static int initialized, arrivals, generation;
static volatile int sum;
static spinlock_t sum_lock, bad_lock;

/* 仅启动诊断用例在临时start.c中调用：记录mret之前的寄存器并装入测试陷阱入口。 */
void lab1_prepare_trap(void)
{
    uint64 hart = r_mhartid();
    boot_status[hart] = r_mstatus();
    asm volatile("csrr %0, mepc" : "=r"(boot_epc[hart]));
    asm volatile("csrr %0, pmpcfg0" : "=r"(boot_pmp[hart]));
    w_mtvec((uint64)lab1_mtrap);
}

static void barrier(void)
{
    int old = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
    if (__atomic_add_fetch(&arrivals, 1, __ATOMIC_ACQ_REL) == NCPU) {
        __atomic_store_n(&arrivals, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&generation, old + 1, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&generation, __ATOMIC_ACQUIRE) == old);
    }
}

static void boot_test(int hart)
{
    uint64 sp = r_sp();
    uint64 low = (uint64)CPU_stack + (uint64)hart * 4096;
    assert(sp >= low && sp < low + 4096 && sp % 16 == 0,
           "boot test: stack bounds or alignment");
    assert(mycpuid() == hart && mycpu() != NULL, "boot test: tp or cpu lookup");
    assert(r_satp() == 0, "boot test: paging unexpectedly enabled");
    assert((boot_status[hart] & MSTATUS_MPP_MASK) == MSTATUS_MPP_S,
           "boot test: MPP not set to S");
    assert(boot_epc[hart] == (uint64)main, "boot test: mepc not main");
    assert((boot_pmp[hart] & 0xff) == PMP_RWX_TOR, "boot test: PMP setup");
    /* S-mode执行ecall应进入测试M-mode入口，mcause=9；M-mode则是11。 */
    asm volatile("ecall" ::: "memory");
    assert(observed_cause[hart] == 9, "boot test: main not running in S-mode");
    printf("cpu %d is booting!\n", hart);
    printf("BOOT hart=%d sp=%x stack=[%x,%x) satp=%x\n", hart, sp,
           low, low + 4096, r_satp());
    printf("BOOT hart=%d saved_MPP=1 PMP=%x ecall_mcause=%d\n",
           hart, boot_pmp[hart], (int)observed_cause[hart]);
    barrier();
    if (hart == 0) printf("BOOT MODE/STACK PASS: two harts in S-mode\n");
}

static void add_test(int hart, int mode)
{
    int rounds = mode == 0 ? RACE_ROUNDS : 1;
    int lost = 0;
    for (int round = 0; round < rounds; round++) {
        if (hart == 0) sum = 0;
        barrier();
        if (mode == 2) spinlock_acquire(&sum_lock);
        for (int i = 0; i < LOOPS; i++) {
            if (mode == 1) spinlock_acquire(&sum_lock);
            sum++;
            if (mode == 1) spinlock_release(&sum_lock);
        }
        if (mode == 2) spinlock_release(&sum_lock);
        barrier();
        if (hart == 0) {
            printf("ADD mode=%s round=%d final=%d expected=2000000\n",
                   mode == 0 ? "unlocked" : mode == 1 ? "fine-lock" : "coarse-lock",
                   round + 1, sum);
            if (mode != 0) assert(sum == 2 * LOOPS, "add test: protected count wrong");
            else if (sum != 2 * LOOPS) lost++;
        }
        barrier();
    }
    if (hart == 0) {
        if (mode == 0) {
            printf("RACE OBSERVED: %d of %d rounds lost updates\n", lost, rounds);
            assert(lost > 0, "race test: scheduling did not expose race; rerun");
            printf("UNLOCKED ADD DEMO COMPLETE\n");
        } else {
            printf("LOCKED ADD PASS: mode=%s lock_acquisitions=%d\n",
                   mode == 1 ? "fine" : "coarse", mode == 1 ? 2 * LOOPS : 2);
        }
    }
}

static void format_test(void)
{
    printf("FMT signed: %d %d %d %d %d\n", 0, 1, -1, 2147483647, (-2147483647 - 1));
    /* Lab1的%p约定取uint32，不能按标准C printf传入任意RV64指针。 */
    printf("FMT p32: %p %p %p\n", (uint32)0, (uint32)0x80000000u, (uint32)0xffffffffu);
    printf("FMT x64: %x %x\n", (uint64)0x123456789abcdef0ull, ~(uint64)0);
    printf("FMT text: %c %s %s %%\n", 'Z', "hello", (char *)NULL);
    printf("FMT unknown: %q\n");
    printf("FMT trailing: %");
    printf("\nPRINTF FORMAT PASS\n");
}

static void print_test(int hart)
{
    const char *payload = hart == 0
        ? "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        : "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    barrier();
    for (int row = 0; row < PRINT_ROWS; row++)
        printf("ROW hart=%d seq=%d payload=%s END\n", hart, row, payload);
    barrier();
    if (hart == 0) printf("\nPRINTF RECORDS DONE\n");
}

static void interrupt_test(void)
{
    cpu_t *cpu = mycpu();
    intr_off();
    assert(cpu->noff == 0, "intr test: initial depth");
    push_off(); push_off();
    assert(cpu->noff == 2 && cpu->origin == 0 && !intr_get(), "intr test: disabled nesting");
    pop_off(); pop_off();
    assert(cpu->noff == 0 && !intr_get(), "intr test: disabled restore");
    printf("INTR initially-off: depth 0 -> 2 -> 0; SIE stays 0\n");
    /* sie仍为0，没有使能具体中断源；只检查sstatus.SIE的保存恢复。 */
    w_sie(0);
    intr_on();
    push_off(); push_off();
    assert(cpu->noff == 2 && cpu->origin == 1 && !intr_get(), "intr test: enabled nesting");
    pop_off();
    assert(cpu->noff == 1 && !intr_get(), "intr test: enabled too early");
    pop_off();
    assert(cpu->noff == 0 && intr_get(), "intr test: enabled restore");
    intr_off();
    printf("INTR initially-on: depth 0 -> 2 -> 1 -> 0; restore SIE only at depth 0\n");
    spinlock_acquire(&sum_lock);
    assert(spinlock_holding(&sum_lock) && cpu->noff == 1, "intr test: lock ownership");
    spinlock_release(&sum_lock);
    assert(!spinlock_holding(&sum_lock) && cpu->noff == 0, "intr test: lock release");
    assert(true, "intr test: assert(true) unexpectedly stopped");
    printf("NESTED INTERRUPT/LOCK OWNERSHIP PASS\n");
}

static void uart_input_test(void)
{
    assert(uart_getc_sync() == -1, "uart test: unexpected initial input");
    printf("UART INPUT READY\n");
    const char *expected = "Lab1!\n";
    for (int i = 0; i < 6; i++) {
        int value;
        do { value = uart_getc_sync(); } while (value == -1);
        assert(value == expected[i], "uart test: received wrong byte");
        printf("UART_RX[%d]=%d\n", i, value);
    }
    printf("UART INPUT PASS: 6 bytes matched\n");
}

int main(void)
{
    int hart = r_tp();
    if (hart == 0) {
        print_init();
        spinlock_init(&sum_lock, "test_sum");
        spinlock_init(&bad_lock, "test_bad");
        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);
    } else {
        while (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE));
    }
    barrier();
    if (LAB1_CASE == 1) boot_test(hart);
    else if (LAB1_CASE >= 2 && LAB1_CASE <= 4) add_test(hart, LAB1_CASE - 2);
    else if (LAB1_CASE == 6 || LAB1_CASE == 7) print_test(hart);
    else if (LAB1_CASE == 12) {
        if (hart == 0) spinlock_acquire(&bad_lock);
        barrier();
        if (hart == 1) spinlock_release(&bad_lock);
    } else if (hart == 0) {
        if (LAB1_CASE == 5) format_test();
        else if (LAB1_CASE == 8) interrupt_test();
        else if (LAB1_CASE == 9) uart_input_test();
        else if (LAB1_CASE == 10) {
            spinlock_acquire(&bad_lock);
            spinlock_acquire(&bad_lock);
        } else if (LAB1_CASE == 11) spinlock_release(&bad_lock);
        else if (LAB1_CASE == 13) { intr_off(); pop_off(); }
        else if (LAB1_CASE == 14) { intr_on(); pop_off(); }
        else if (LAB1_CASE == 15) assert(false, "lab1 expected assertion");
    }
    if (LAB1_CASE >= 10 && hart == 0 && LAB1_CASE != 12)
        panic("negative test: invalid operation accepted");
    while (1) asm volatile("wfi");
}
