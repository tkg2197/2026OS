#include "mod.h"
#include "../proc/mod.h"
#include "../../user/syscall_num.h"

extern char trampoline[], user_vector[], user_return[], kernel_vector[];

void trap_user_handler(void)
{
    uint64 status = r_sstatus();
    assert(!(status & SSTATUS_SPP), "user trap: not from U-mode");
    assert(!intr_get(), "user trap: interrupts enabled");
    // 此后在内核页表执行，内核trap必须改走内核栈上的vector。
    w_stvec((uint64)kernel_vector);
    proc_t *p = myproc();
    assert(p != NULL, "user trap: no current process");
    uint64 cause = r_scause();
    p->tf->user_to_kern_epc = r_sepc();
    if (cause == 8)
    {
        // ecall固定为4字节；其他异步中断不前移用户PC。
        p->tf->user_to_kern_epc += 4;
        if (p->tf->a7 == SYS_helloworld)
        {
            printf("proczero: hello world!\n");
            p->tf->a0 = 0;
        }
        else
            p->tf->a0 = (uint64)-1;
    }
    else if (cause == ((1ull << 63) | 5))
        timer_interrupt_handler();
    else if (cause == ((1ull << 63) | 9))
        external_interrupt_handler();
    else
    {
        printf("user fault: pid=%d scause=%x sepc=%x stval=%x\n",
               p->pid, cause, p->tf->user_to_kern_epc, r_stval());
        // Lab4尚无exit/scheduler：保留故障资源，停止执行，设备仍能工作。
        mycpu()->proc = NULL;
        intr_on();
        while (1)
            asm volatile("wfi");
    }
    trap_user_return();
}

void trap_user_return(void)
{
    intr_off();
    proc_t *p = myproc();
    assert(p != NULL, "user return: no current process");
    p->tf->user_to_kern_satp = r_satp();
    p->tf->user_to_kern_sp = p->kstack + PGSIZE;
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    p->tf->user_to_kern_hartid = mycpuid();
    w_stvec(TRAMPOLINE + (uint64)(user_vector - trampoline));
    uint64 status = r_sstatus();
    status &= ~(SSTATUS_SPP | SSTATUS_SIE);
    status |= SSTATUS_SPIE;
    w_sstatus(status);
    w_sepc(p->tf->user_to_kern_epc);
    uint64 entry = TRAMPOLINE + (uint64)(user_return - trampoline);
    ((void (*)(uint64, uint64))entry)(TRAPFRAME, MAKE_SATP(p->pgtbl));
    __builtin_unreachable();
}
