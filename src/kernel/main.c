#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
//_entry在kernel.ld中定义 将后续hart中的启动地址定义为该地址
extern void _entry(void);

static volatile int initialized = 0;

void main(void)
{
    uint64 hartid = r_tp();

    if (hartid == 0)
    {
        print_init();
        pmem_init();
        kvm_init();
        kvm_inithart();
        __sync_synchronize();
        initialized = 1;

        for (uint64 id = 1; id < NCPU; id++)
        {
            sbi_ret_t ret = sbi_hart_start(id, (uint64)_entry, 0);
            if (ret.error != SBI_SUCCESS)
            {
                printf("failed to start cpu %d: error %d\n",
                       (int)id, (int)ret.error);
                panic("sbi_hart_start");
            }
        }
    }
    else
    {
        while (!initialized)
            ;
        __sync_synchronize();
        printf("cpu %d is booting with OpenSBI!\n", (int)hartid);
    }

    while (1)
        asm volatile("wfi");
}
