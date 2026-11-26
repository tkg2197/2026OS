#include "arch/mod.h"
#include "lib/mod.h"

static volatile int started;

int main(void)
{
    int cpuid = r_tp();
    if (cpuid == 0) {
        print_init();
        printf("cpu %d is booting!\n", cpuid);
        __sync_synchronize();
        started = 1;
    } else {
        while (!started)
            ;
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
    }

    for (;;)
        ;
}
