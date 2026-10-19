#include "sys.h"

__attribute__((section(".text.entry"))) int main(void)
{
    syscall(SYS_helloworld);
    syscall(SYS_helloworld);
    while (1)
        ;
    return 0;
}