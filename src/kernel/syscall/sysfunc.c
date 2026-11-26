#include "mod.h"
#include "../proc/mod.h"
#include "../trap/mod.h"

uint64 sys_print_str(void)
{
    char buf[STR_MAXLEN + 1];
    if (arg_str(0, buf, sizeof(buf)) < 0)
        return -1;
    printf("%s", buf);
    return 0;
}

uint64 sys_brk(void)
{
    proc_t *p = myproc();
    uint64 old = p->heap_top;
    uint64 top = arg_raw(0);
    if (!top)
        return old;
    if (top < USER_HEAP_BASE || top > MMAP_BEGIN ||
        (top < old ? old - top : top - old) > (uint32)-1)
        return -1;
    uint64 result = top < old ? uvm_heap_ungrow(p->pgtbl, old, old - top)
                              : uvm_heap_grow(p->pgtbl, old, top - old);
    if (result != (uint64)-1)
        p->heap_top = result;
    return result;
}

static bool valid_map_length(uint64 len)
{
    return len && len <= (uint32)-1 && len % PGSIZE == 0;
}

uint64 sys_mmap(void)
{
    uint64 len = arg_raw(1);
    if (!valid_map_length(len))
        return 0;
    return uvm_mmap(arg_raw(0), len / PGSIZE, PTE_R | PTE_W);
}

uint64 sys_munmap(void)
{
    uint64 len = arg_raw(1);
    if (!valid_map_length(len) || uvm_munmap(arg_raw(0), len / PGSIZE) < 0)
        return -1;
    return 0;
}

uint64 sys_print_int(void)
{
    printf("%d", (int)(uint32)arg_raw(0));
    return 0;
}

uint64 sys_getpid(void)
{
    return myproc()->pid;
}

uint64 sys_fork(void)
{
    return proc_fork();
}

uint64 sys_wait(void)
{
    return proc_wait(arg_raw(0));
}

uint64 sys_exit(void)
{
    proc_exit((int)(uint32)arg_raw(0));
}

uint64 sys_sleep(void)
{
    timer_wait(arg_raw(0));
    return 0;
}
