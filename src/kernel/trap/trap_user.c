#include "mod.h"

extern char trampoline[], user_vector[], user_return[];
extern char kernel_vector[];

void trap_user_handler(void)
{
    assert(!(r_sstatus() & SSTATUS_SPP) && !intr_get(), "user trap state");
    w_stvec((uint64)kernel_vector);
    proc_t *p = myproc();
    p->tf->user_to_kern_epc = r_sepc();

    uint64 cause = r_scause();
    if (cause == 8)
    {
        p->tf->user_to_kern_epc += 4;
        intr_on();
        syscall();
    }
    else if (cause == ((1ull << 63) | 5))
        timer_interrupt_handler();
    else if (cause == ((1ull << 63) | 9))
        external_interrupt_handler();
    else if (cause == 13 || cause == 15)
    {
        int64 pages = uvm_ustack_grow(p->pgtbl, p->ustack_npage, r_stval());
        if (pages < 0)
            goto fault;
        printf("stack: %d -> %d pages\n", (int)p->ustack_npage, (int)pages);
        p->ustack_npage = pages;
    }
    else
        goto fault;

    trap_user_return();

fault:
    printf("user fault pid=%d cause=%x addr=%x\n", p->pid, cause, r_stval());
    intr_on();
    for (;;)
        asm volatile("wfi");
}

void trap_user_return(void)
{
    intr_off();
    proc_t *p = myproc();
    trapframe_t *tf = p->tf;
    tf->user_to_kern_satp = r_satp();
    tf->user_to_kern_sp = p->kstack + PGSIZE;
    tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    tf->user_to_kern_hartid = r_tp();

    w_stvec(TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline));
    w_sstatus((r_sstatus() & ~SSTATUS_SPP) | SSTATUS_SPIE);
    w_sepc(tf->user_to_kern_epc);
    void (*enter)(uint64, uint64) = (void *)(TRAMPOLINE +
        ((uint64)user_return - (uint64)trampoline));
    enter(TRAPFRAME, MAKE_SATP(p->pgtbl));
    __builtin_unreachable();
}
