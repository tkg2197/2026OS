#include "mod.h"

// PLIC初始化
void plic_init()
{
    // 设置UART中断优先级
    *(volatile uint32 *)(PLIC_PRIORITY(UART_IRQ)) = 1;
}

// PLIC核心初始化
void plic_inithart()
{
    int hartid = mycpuid();
    volatile uint32 *enable = (volatile uint32 *)PLIC_SENABLE(hartid);
#ifdef VISIONFIVE2
    // 清零已有的中断使能
    for (int word = 0; word < 5; word++)
        enable[word] = 0;
#endif
    enable[UART_IRQ / 32] = 1u << (UART_IRQ % 32);
    // 设置响应阈值
    *(volatile uint32 *)PLIC_SPRIORITY(hartid) = 0;
}

// 获取中断号
int plic_claim(void)
{
    int hartid = mycpuid();
    int irq = *(volatile uint32 *)PLIC_SCLAIM(hartid);
    return irq;
}

// 确认该中断号对应中断已经完成
void plic_complete(int irq)
{
    int hartid = mycpuid();
    *(volatile uint32 *)PLIC_SCLAIM(hartid) = irq;
}