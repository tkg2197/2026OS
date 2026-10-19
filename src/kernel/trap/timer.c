#include "mod.h"

// 当前内核从OpenSBI进入S-mode，时钟需要通过SBI接口设置。
// S-mode不能直接访问M-mode的CLINT、mtimecmp和mstatus, 只能通过ecall请求OpenSBI代劳。

// 全局共享的时钟, 所有CPU看到的是同一份数据, 由自旋锁保护
static timer_t system_timer;

// 设置当前CPU的第一次定时事件, 并使能S-mode时钟中断
// 时钟是每个CPU独享的资源(CLINT中每个hart有自己的mtimecmp), 因此每个CPU都要调用一次
void timer_init(void)
{
    // 注意顺序: 必须先设置首次定时事件, 再打开时钟中断开关
    // 否则stimecmp中可能残留着已经过期的值, 中断会立刻触发
    sbi_set_timer(r_time() + INTERVAL);

    // 使能S-mode时钟中断 (sie中的STIE位)
    w_sie(r_sie() | SIE_STIE);
}

// 系统时钟的创建, 只需要做一次(位于trap_kernel_init中)
void timer_create(void)
{
}

// 时钟滴答: 更新共享的滴答计数
// 多核情况下只允许CPU-0调用, 否则ticks会涨得比时钟还快
void timer_update(void)
{
}

// 获取当前的滴答数
uint64 timer_get_ticks(void)
{
}
