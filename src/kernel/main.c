#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"

extern void _entry(void);
extern uint64 initial_hart;

static volatile int initialized = 0;

void main(void)
{
    uint64 hartid = r_tp();

    if (hartid == 0)
    {
        print_init();
        printf("cpu %d is booting with OpenSBI!\n", (int)hartid);
        pmem_init();

        kvm_init();
        kheap_init();

        // Shared trap state must be ready before secondary harts start.
        trap_kernel_init();

        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);

        for (uint64 id = 1; id < NCPU; id++)
        {
            // 冷启动hart若为1，它已经在等待共享初始化，无需再次启动。
            if (id == __atomic_load_n(&initial_hart, __ATOMIC_ACQUIRE))
                continue;
            sbi_ret_t ret = sbi_hart_start(id, (uint64)_entry, 0);
            assert(ret.error == SBI_SUCCESS,
                   "sbi_hart_start failed");
        }
    }
    else
    {
        while (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE))
            ;

        printf("cpu %d is booting with OpenSBI!\n", (int)hartid);
    }

    // 各核分别启用分页和中断。
    kvm_inithart();

    // PLIC开关, 中断入口stvec, 中断使能位
    trap_kernel_inithart();

    // 设置本CPU的第一次时钟事件, 并打开S-mode时钟中断
    timer_init();

    if (hartid == 0)
        uart_enable_rx_interrupt();

    uint64 last_tick = 0;
    for (;;) {
        asm volatile("wfi");
        if (hartid == 0) {
            uint64 ticks = timer_get_ticks();
            if (ticks != last_tick) {
                printf("ticks = %d\n", (int)ticks);
                last_tick = ticks;
            }
        }
    }
}
