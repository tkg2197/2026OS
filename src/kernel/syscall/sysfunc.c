#include "mod.h"

uint64 sys_helloworld(void)
{
    printf("hello world\n");
    return 0;
}

uint64 sys_copyin(void)
{
    int values[5];
    uint64 n = arg_raw(1);
    if (n > sizeof(values) / sizeof(values[0]) ||
        uvm_copyin(myproc()->pgtbl, (uint64)values, arg_raw(0), n * sizeof(int)) < 0)
        return -1;
    for (uint32 i = 0; i < n; i++)
        printf("%d%c", values[i], i + 1 == n ? '\n' : ' ');
    return 0;
}

uint64 sys_copyout(void)
{
    const int values[] = {1, 2, 3, 4, 5};
    if (uvm_copyout(myproc()->pgtbl, arg_raw(0), (uint64)values, sizeof(values)) < 0)
        return -1;
    return sizeof(values) / sizeof(values[0]);
}

uint64 sys_copyinstr(void)
{
    return sys_printf();
}

uint64 sys_printf(void)
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
    {
        p->heap_top = result;
        printf("brk: %x -> %x\n", old, result);
    }
    return result;
}

static bool valid_map_length(uint64 len)
{
    return len && len <= (uint32)-1 && len % PGSIZE == 0;
}

static void trace_mappings(void)
{
#ifdef LAB5_TRACE
    proc_t *p = myproc();
    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
#endif
}

uint64 sys_mmap(void)
{
    uint64 len = arg_raw(1);
    if (!valid_map_length(len))
        return 0;
    uint64 begin = uvm_mmap(arg_raw(0), len / PGSIZE, PTE_R | PTE_W);
    if (begin)
        trace_mappings();
    return begin;
}

uint64 sys_munmap(void)
{
    uint64 len = arg_raw(1);
    if (!valid_map_length(len) || uvm_munmap(arg_raw(0), len / PGSIZE) < 0)
        return -1;
    trace_mappings();
    return 0;
}
