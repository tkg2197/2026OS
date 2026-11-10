#include "mod.h"

static cpu_t cpus[NCPU];

cpu_t *mycpu(void)
{
    uint64 hartid = r_tp();
    if (hartid - FIRST_HART >= NCPU - FIRST_HART)
        panic("invalid hart id");
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
