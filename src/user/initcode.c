#include "sys.h"

// 测试 1：把内核数组读入用户空间，再把数组和字符串传回内核。
__attribute__((section(".text.entry")))
int main(void)
{
    int values[5];
    syscall(SYS_copyout, values);
    syscall(SYS_copyin, values, 5);
    syscall(SYS_copyinstr, "hello, world\n");
    for (;;)
        ;
}
