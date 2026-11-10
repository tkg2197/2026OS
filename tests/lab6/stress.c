#include "sys.h"

#define PGSIZE 4096ul
#define SYS_probe 11
#define PROBE(op, arg) syscall(SYS_probe, op, arg)

static void check(int condition, const char *message)
{
    if (!condition)
    {
        syscall(SYS_print_str, "LAB6 FAIL: ");
        syscall(SYS_print_str, message);
        syscall(SYS_print_str, "\n");
        if (syscall(SYS_getpid) != 1)
            syscall(SYS_exit, 255);
        for (;;)
            ;
    }
}

static void isolation_and_sleep(void)
{
    unsigned long heap = syscall(SYS_brk, 0);
    check(syscall(SYS_brk, heap + PGSIZE) == heap + PGSIZE, "brk");
    volatile char *hp = (volatile char *)heap;
    volatile char *mp = (volatile char *)mmap(0, PGSIZE);
    check(mp != 0, "mmap");

    volatile char stack[3 * PGSIZE];
    stack[0] = 's';
    hp[0] = 'h';
    mp[0] = 'm';

    int child = syscall(SYS_fork);
    check(child >= 0, "fork");
    if (!child)
    {
        check(stack[0] == 's' && hp[0] == 'h' && mp[0] == 'm', "copy");
        stack[0] = hp[0] = mp[0] = 'c';
        check(PROBE(1, 0) == 1, "sleep lock owner");
        PROBE(4, 1);
        check(PROBE(5, 2) == 0, "kernel preemption");
        unsigned long begin = PROBE(0, 0);
        syscall(SYS_sleep, 30);
        check((unsigned long)PROBE(0, 0) - begin >= 30, "slept 30 ticks");
        check(PROBE(2, 0) == 0, "sleep lock release");
        syscall(SYS_exit, 1234);
    }

    check(PROBE(5, 1) == 0, "child started");
    int other = syscall(SYS_fork);
    check(other >= 0, "contender fork");
    if (!other)
    {
        check(PROBE(3, 0) == 0, "non-owner");
        PROBE(4, 2);
        check(PROBE(1, 0) == 1, "contender woke");
        check(PROBE(2, 0) == 0, "contender released");
        syscall(SYS_exit, 42);
    }
    check(syscall(SYS_wait, 1) == -1, "invalid wait address");
    for (int i = 0; i < 2; i++)
    {
        int status = 0;
        int pid = syscall(SYS_wait, &status);
        check((pid == child && status == 1234) || (pid == other && status == 42),
              "wait preserved zombie");
    }
    check(stack[0] == 's' && hp[0] == 'h' && mp[0] == 'm', "isolated data");
    check(munmap((void *)mp, PGSIZE) == 0 && syscall(SYS_brk, heap) == heap, "unmap");
    check(syscall(SYS_wait, 0) == -1, "no children");
}

static void failure_and_reuse(void)
{
    for (int i = 0; i < 31; i++)
    {
        int pid = syscall(SYS_fork);
        check(pid >= 0, "fill proc slots");
        if (!pid)
            syscall(SYS_exit, 0);
    }
    check(syscall(SYS_fork) == -1, "proc table full");
    for (int i = 0; i < 31; i++)
        check(syscall(SYS_wait, 0) > 0, "reap slot");

    const unsigned long span = 70ul << 20;
    unsigned long heap = syscall(SYS_brk, 0);
    check(syscall(SYS_brk, heap + span) == heap + span, "large heap");
    check(syscall(SYS_fork) == -1, "fork OOM");
    check(syscall(SYS_wait, 0) == -1, "failed fork unpublished");
    check(syscall(SYS_brk, heap) == heap, "release large heap");

    int parent = syscall(SYS_fork);
    check(parent >= 0, "reuse after OOM");
    if (!parent)
    {
        int orphan = syscall(SYS_fork);
        check(orphan >= 0, "orphan fork");
        if (!orphan)
        {
            syscall(SYS_sleep, 2);
            volatile unsigned long bad = 0x900000;
            *(volatile char *)bad = 1;
            syscall(SYS_exit, 99);
        }
        syscall(SYS_exit, 7);
    }
    int seen_parent = 0, seen_orphan = 0;
    for (int i = 0; i < 2; i++)
    {
        int status;
        int pid = syscall(SYS_wait, &status);
        check(pid > 0, "reap adopted child");
        seen_parent += pid == parent && status == 7;
        seen_orphan += pid != parent && status == -1;
    }
    check(seen_parent == 1 && seen_orphan == 1, "adopted fault exit status");
}

__attribute__((section(".text.entry")))
int main(void)
{
    check(syscall(SYS_getpid) == 1, "initial pid");
    check(syscall(1ul << 32 | SYS_getpid) == -1, "invalid syscall");
    isolation_and_sleep();
    failure_and_reuse();
    syscall(SYS_print_str, "LAB6 PASS\n");
    for (;;)
        ;
}
