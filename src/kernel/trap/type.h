#pragma once
#include "../lock/type.h"

// platform-level interrupt controller (PLIC)
// 接收外部中断源: 主要是外设中断
#define PLIC_BASE 0x0c000000ul // PLIC MMIO空间的起始地址
#define PLIC_SIZE 0x04000000ul // PLIC空间的大小
#define PLIC_PRIORITY(id) (PLIC_BASE + (id) * 4) // 为每个中断源分配了一个优先级寄存器，其中储存这个中断的优先级。UART是10
#define PLIC_PENDING (PLIC_BASE + 0x1000) // PENDING寄存器中每一位记录是否有一个id中断请求处于待处理状态。例如，将第10位置1代表有一个UART中断待处理
#define PLIC_MENABLE(hart) (PLIC_BASE + 0x2000 + (hart) * 0x100) // 每一位记录对应id的中断请求是否使能，这是M-mode
#define PLIC_SENABLE(hart) (PLIC_BASE + 0x2080 + (hart) * 0x100) // 这是S-mode
#define PLIC_MPRIORITY(hart) (PLIC_BASE + 0x200000 + (hart) * 0x2000) // 中断优先级阈值
#define PLIC_SPRIORITY(hart) (PLIC_BASE + 0x201000 + (hart) * 0x2000)
#define PLIC_MCLAIM(hart) (PLIC_BASE + 0x200004 + (hart) * 0x2000) // 读取这个寄存器可以得到一个IQR,代表领取了一个中断。写入这个寄存器代表通知PLIC中断处理完成
#define PLIC_SCLAIM(hart) (PLIC_BASE + 0x201004 + (hart) * 0x2000)

// 当前内核在S-mode运行，由OpenSBI管理CLINT。
#define CLINT_BASE 0x2000000ul
#define CLINT_MSIP(hart) (CLINT_BASE + 4 * (hart))
#define CLINT_MTIMECMP(hart) (CLINT_BASE + 0x4000 + 8 * (hart))
#define CLINT_MTIME (CLINT_BASE + 0xBFF8)

#define INTERVAL 1000000

typedef struct timer {
    uint64 ticks; // 中断的次数
    spinlock_t lk;
} timer_t;
