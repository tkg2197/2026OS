#include "mod.h"

/*
    开关中断的基本逻辑:
    1. 多个地方可能开关中断, 因此它不是“开/关”的二元状态，而是“关 关 关 开 开 开”的stack
    2. 在第一次执行关中断时, 记录中断的初始状态为X
    3. 每次关中断, stack中的元素加1
    4. 每次开中断, stack中的元素减1
    5. 如果stack中元素清空, 将中断状态设为初始的X
*/

// 带层数叠加的关中断
void push_off(void)
{
    int old = intr_get();
    intr_off();
    cpu_t *cpu = mycpu();
    if (cpu->noff == 0)
        cpu->intena = old;
    cpu->noff++;
}

// 带层数叠加的开中断
void pop_off(void)
{
    cpu_t *cpu = mycpu();
    assert(intr_get() == 0, "push_off: 1\n"); // 确保此时中断是关闭的
    assert(cpu->noff >= 1, "push_off: 2\n");  // 确保push和pop的对应
    cpu->noff--;
    if (cpu->noff == 0 && cpu->intena == 1) // 只有所有push操作都被抵消且原来状态是开着时
        intr_on();
}

// 自选锁初始化
void spinlock_init(spinlock_t *lk, const char *name)
{
    lk->name = name;
    lk->owner = ~0ull;
    lk->locked = 0;
}

// 是否持有自旋锁
bool spinlock_holding(spinlock_t *lk)
{
    return __atomic_load_n(&lk->locked, __ATOMIC_RELAXED) &&
           __atomic_load_n(&lk->owner, __ATOMIC_RELAXED) == r_tp();
}

// 获取自选锁
void spinlock_acquire(spinlock_t *lk)
{
    push_off();
    assert(!spinlock_holding(lk), "recursive spinlock acquire");
    while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE))
        ;
    __atomic_store_n(&lk->owner, r_tp(), __ATOMIC_RELAXED);
}

// 释放自旋锁
void spinlock_release(spinlock_t *lk)
{
    assert(spinlock_holding(lk), "spinlock release by non-owner");
    __atomic_store_n(&lk->owner, ~0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);
    pop_off();
}
