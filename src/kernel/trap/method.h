#pragma once

// PLIC相关

void plic_init(void);        // 设置中断优先级
void plic_inithart(void);    // 使能中断开关
int plic_claim(void);        // 获取中断号
void plic_complete(int irq); // 告知中断响应完成

// OpenSBI TIME与共享时钟

void timer_init(void);        // 时钟初始化
void timer_create(void);      // 时钟创建
void timer_update(void);      // 时钟更新(ticks++)
uint64 timer_get_ticks(void); // 获取时钟的tick

// trap的初始化和处理逻辑

void trap_kernel_init(void);
void trap_kernel_inithart(void);
void trap_kernel_handler(void);

// 辅助函数: 外设中断和时钟中断处理

void external_interrupt_handler(void);
void timer_interrupt_handler(void);

void trap_user_handler(void) __attribute__((noreturn));
void trap_user_return(void) __attribute__((noreturn));
