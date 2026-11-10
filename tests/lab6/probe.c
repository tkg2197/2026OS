#include "../proc/mod.h"
#include "../syscall/mod.h"
#include "../trap/mod.h"

static sleeplock_t test_lock;
static uint32 flags;

void lab6_test_init(void)
{
    sleeplock_init(&test_lock, "lab6 test");
}

uint64 lab6_test_probe(void)
{
    uint64 arg = arg_raw(1);
    switch (arg_raw(0))
    {
    case 0:
        return timer_get_ticks();
    case 1:
        sleeplock_acquire(&test_lock);
        return sleeplock_holding(&test_lock);
    case 2:
        sleeplock_release(&test_lock);
        return sleeplock_holding(&test_lock);
    case 3:
        return sleeplock_holding(&test_lock);
    case 4:
        __atomic_fetch_or(&flags, (uint32)arg, __ATOMIC_RELEASE);
        return 0;
    case 5: {
        // On one hart another process can set the bit only after preemption.
        uint64 start = r_time();
        while (!(__atomic_load_n(&flags, __ATOMIC_ACQUIRE) & arg))
        {
            if (r_time() - start > 20 * INTERVAL)
                return -1;
        }
        return 0;
    }
    default:
        return -1;
    }
}
