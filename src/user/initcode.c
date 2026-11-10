#include "sys.h"

#define PGSIZE 4096ul

static void check(int condition)
{
    if (!condition)
    {
        syscall(SYS_printf, "LAB5 FAIL\n");
        for (;;)
            ;
    }
}

__attribute__((section(".text.entry")))
int main(void)
{
    int values[5];
    check(syscall(SYS_copyout, values) == 5);
    check(values[0] == 1 && values[4] == 5);
    check(syscall(SYS_copyin, values, 5) == 0);
    check(syscall(SYS_copyinstr, "hello, world\n") == 0);

    unsigned long heap = syscall(SYS_brk, 0);
    check(syscall(SYS_brk, heap + 9 * PGSIZE) == heap + 9 * PGSIZE);
    check(syscall(SYS_brk, heap + 4 * PGSIZE) == heap + 4 * PGSIZE);
    check(syscall(SYS_brk, heap) == heap);

    volatile char stack[4 * PGSIZE];
    stack[0] = 'S';
    stack[1] = '\0';
    check(syscall(SYS_copyinstr, stack) == 0);
    check(syscall(SYS_printf, "\n") == 0);

    unsigned long addr = mmap(0, 3 * PGSIZE);
    check(addr != 0);
    check(munmap((void *)(addr + PGSIZE), PGSIZE) == 0);
    check(munmap((void *)addr, 3 * PGSIZE) == -1);
    check(mmap(0, PGSIZE) == addr + PGSIZE);
    check(munmap((void *)addr, 3 * PGSIZE) == 0);

    check(syscall(1ul << 32) == -1);
    check(syscall(SYS_brk, heap + (1ul << 32) + PGSIZE) == -1);
    check(syscall(SYS_brk, 0) == heap);
    check(mmap(0, (1ul << 32) + PGSIZE) == 0);
    check(munmap((void *)addr, (1ul << 32) + PGSIZE) == -1);
    syscall(SYS_printf, "LAB5 PASS\n");
    for (;;)
        ;
}
