#include "arch/mod.h"
#include "lib/mod.h"

static volatile int started;
static volatile int sum;
static volatile int finish;

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

    for (int i = 0; i < 1000000; i++)
        __sync_fetch_and_add(&sum, 1);
    __sync_fetch_and_add(&finish, 1);
    while (finish < 2)
        ;
    __sync_synchronize();
    printf("cpu %d report: sum = %d\n", cpuid, sum);
    for (;;)
        ;
}
