#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"
#include "proc/mod.h"
//_entry_secondary是HSM启动其他hart的入口
extern void _entry_secondary(void);

static int initialized = 0;

void main(void)
{
    uint64 hartid = r_tp();

    if (hartid == boot_hartid)
    {
        print_init();
        printf("cpu %d is booting with OpenSBI!\n", (int)hartid);
        pmem_init();
        kvm_init();
        mmap_init();

        // trap中各个CPU共享的初始化: PLIC和系统时钟
        // 必须放在其他hart启动之前, 否则它们可能在初始化完成前就走进中断
        trap_kernel_init();

        __atomic_store_n(&initialized, 1, __ATOMIC_RELEASE);

        for (uint64 id = FIRST_HART; id < NCPU; id++)
        {
            if (id == boot_hartid)
                continue;
            sbi_ret_t ret = sbi_hart_start(id, (uint64)_entry_secondary, 0);
            if (ret.error != SBI_SUCCESS)
            {
                printf("failed to start cpu %d: error %d\n",
                       (int)id, (int)ret.error);
                panic("sbi_hart_start");
            }
        }
    }
    else
    {
        while (!__atomic_load_n(&initialized, __ATOMIC_ACQUIRE))
            ;
        printf("cpu %d is booting with OpenSBI!\n", (int)hartid);
    }

    // 以下初始化是每个CPU各自独有的

    // 切换到内核页表 (必须等启动核把内核页表建好)
    kvm_inithart();

    // PLIC开关, 中断入口stvec, 中断使能位
    trap_kernel_inithart();
    if (hartid == boot_hartid)
        uart_enable_rx_interrupt();

    // 设置本CPU的第一次时钟事件, 并打开S-mode时钟中断
    timer_init();

    if (hartid == boot_hartid)
        proc_make_first();

    while (1)
        asm volatile("wfi");
}
