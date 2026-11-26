#pragma once
#include "../arch/type.h"

/* 自旋锁 */
typedef struct spinlock
{
    int locked; // 是否上锁
    const char *name; // 锁的名字
    uint64 owner;  // 持有该锁的CPU
} spinlock_t;

typedef struct sleeplock
{
    spinlock_t lock;
    int locked;
    const char *name;
    int pid;
} sleeplock_t;
