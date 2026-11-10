#include "mod.h"
#include "../proc/mod.h"

void sleeplock_init(sleeplock_t *lk, const char *name)
{
    spinlock_init(&lk->lock, name);
    lk->locked = 0;
    lk->pid = 0;
    lk->name = name;
}

bool sleeplock_holding(sleeplock_t *lk)
{
    proc_t *p = myproc();
    spinlock_acquire(&lk->lock);
    bool owned = p && lk->locked && lk->pid == p->pid;
    spinlock_release(&lk->lock);
    return owned;
}

void sleeplock_acquire(sleeplock_t *lk)
{
    proc_t *p = myproc();
    assert(p != NULL, "sleeplock_acquire: no process");
    spinlock_acquire(&lk->lock);
    assert(!lk->locked || lk->pid != p->pid, "recursive sleeplock acquire");
    while (lk->locked)
        proc_sleep(lk, &lk->lock);
    lk->locked = 1;
    lk->pid = p->pid;
    spinlock_release(&lk->lock);
}

void sleeplock_release(sleeplock_t *lk)
{
    proc_t *p = myproc();
    spinlock_acquire(&lk->lock);
    assert(p && lk->locked && lk->pid == p->pid, "sleeplock release by non-owner");
    lk->locked = 0;
    lk->pid = 0;
    proc_wakeup(lk);
    spinlock_release(&lk->lock);
}
