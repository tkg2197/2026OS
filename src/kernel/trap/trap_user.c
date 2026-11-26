#include "mod.h"
#include "../proc/mod.h"
#include "../../user/syscall_num.h"

extern char trampoline[], user_vector[], user_return[], kernel_vector[];

void trap_user_handler(void)
{
    uint64 status = r_sstatus();
    assert(!(status & SSTATUS_SPP), "user trap: not from U-mode"); // 确保trap来自用户模式
    assert(!intr_get(), "user trap: interrupts enabled");// 硬件已经关中断
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
    else if (cause == SCAUSE_S_TIMER)
        timer_interrupt_handler();
    else if (cause == SCAUSE_S_EXTERNAL)
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
    // stack grow from high to low
    p->tf->user_to_kern_sp = p->kstack + PGSIZE;
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    p->tf->user_to_kern_hartid = mycpuid();
    // next trap will start from here
    w_stvec(TRAMPOLINE + (uint64)(user_vector - trampoline));// 为什么不直接写user_vector?因为user_vector是内核汇编代码地址的代称，用户态无法使用
    uint64 status = r_sstatus();
    // 返回用户态，不接受中断。这里清除SIE多余了
    status &= ~(SSTATUS_SPP | SSTATUS_SIE);//清零
    status |= SSTATUS_SPIE;// sret是，硬件把SPIE写入SIE，这里置1，让sret后开中断
    w_sstatus(status);
    w_sepc(p->tf->user_to_kern_epc);
    uint64 entry = TRAMPOLINE + (uint64)(user_return - trampoline);
    // 调用entry地址的函数，传入参数TRAPGRAME和页表
    ((void (*)(uint64, uint64))entry)(TRAPFRAME, MAKE_SATP(p->pgtbl)); // TRAPFRAME放入a0，用户根页表地址放入a1
    __builtin_unreachable(); // 保证不执行到这里
}
