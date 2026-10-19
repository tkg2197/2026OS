#include "../arch/mod.h"
#include "../trap/mod.h"
#include "../lib/mod.h"

// 每个CPU在运行操作系统时需要一个初始的函数栈
__attribute__((aligned(16))) uint8 CPU_stack[4096 * NCPU];

extern void main(void);
extern void _entry(void);

// 固件冷启动hart可能不是0；HSM启动的a1则由main固定传0。
uint64 initial_hart;

void start(uint64 hartid, uint64 opaque)
{
    // OpenSBI已在S-mode进入内核。HSM启动hart时satp也为0。
    w_satp(0);
    sfence_vma();

    // OpenSBI通过a0传入hartid,也就是当前函数的输入参数hartid，我们可以直接使用他,内核继续用tp保存它。
    w_tp(hartid);

    if (opaque != 0)
    {
        __atomic_store_n(&initial_hart, hartid, __ATOMIC_RELEASE);
        if (hartid != 0)
        {
            sbi_ret_t ret = sbi_hart_start(0, (uint64)_entry, 0);
            if (ret.error != SBI_SUCCESS)
            {
                // 打印锁尚未创建，使用已由固件配置的UART直接诊断。
                const char *message = "boot: cannot start hart0\r\n";
                while (*message)
                    uart_putc_sync(*message++);
                while (1)
                    asm volatile("wfi");
            }
        }
    }

    // 进入main函数
    main();
}
