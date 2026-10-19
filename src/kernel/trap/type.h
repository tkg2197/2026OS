#pragma once
#include "../lock/type.h"

// platform-level interrupt controller (PLIC)
// 接收外部中断源: 主要是外设中断
#define PLIC_BASE 0x0c000000ul
#define PLIC_SIZE 0x04000000ul
#define PLIC_PRIORITY(id) (PLIC_BASE + (id) * 4)
#define PLIC_PENDING (PLIC_BASE + 0x1000)
#define PLIC_MENABLE(hart) (PLIC_BASE + 0x2000 + (hart) * 0x100)
#define PLIC_SENABLE(hart) (PLIC_BASE + 0x2080 + (hart) * 0x100)
#define PLIC_MPRIORITY(hart) (PLIC_BASE + 0x200000 + (hart) * 0x2000)
#define PLIC_SPRIORITY(hart) (PLIC_BASE + 0x201000 + (hart) * 0x2000)
#define PLIC_MCLAIM(hart) (PLIC_BASE + 0x200004 + (hart) * 0x2000)
#define PLIC_SCLAIM(hart) (PLIC_BASE + 0x201004 + (hart) * 0x2000)

// 当前内核在S-mode运行，由OpenSBI管理CLINT。
#define CLINT_BASE 0x2000000ul
#define CLINT_MSIP(hart) (CLINT_BASE + 4 * (hart))
#define CLINT_MTIMECMP(hart) (CLINT_BASE + 0x4000 + 8 * (hart))
#define CLINT_MTIME (CLINT_BASE + 0xBFF8)

#define INTERVAL 1000000

typedef struct timer {
    uint64 ticks;
    spinlock_t lk;
} timer_t;
