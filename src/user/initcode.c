#include "sys.h"

__attribute__((section(".text.entry"))) int main(void) // 将main函数放入.text.entry
{
    syscall(SYS_helloworld);
    syscall(SYS_helloworld);
    while (1)
        ;
    return 0;
}