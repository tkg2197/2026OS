#include "mod.h"

static cpu_t cpus[NCPU];

cpu_t *mycpu(void)
{
    uint64 hartid = r_tp();
    if (hartid - FIRST_HART >= NCPU - FIRST_HART)
    {
        // No valid CPU slot exists for panic's normal locking path.
        intr_off();
        for (;;)
            asm volatile("wfi");
    }
    return &cpus[hartid];
}

int mycpuid(void)
{
    int hartid = r_tp();
    return hartid;
}
proc_t *myproc(void)
{
    push_off();
    proc_t *p = mycpu()->proc;
    pop_off();
    return p;
}