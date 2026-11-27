#include "arch/mod.h"
#include "lib/mod.h"

static int started;

int main(void)
{
    int cpuid = r_tp();
    if (cpuid == 0) {
        print_init();
        printf("cpu %d is booting!\n", cpuid);
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    } else {
        while (!__atomic_load_n(&started, __ATOMIC_ACQUIRE))
            ;
        printf("cpu %d is booting!\n", cpuid);
    }

    for (;;)
        asm volatile("wfi");
}
