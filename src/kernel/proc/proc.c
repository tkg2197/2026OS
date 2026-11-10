#include "mod.h"
#include "initcode.h"

extern char trampoline[];
extern void swtch(context_t *old, context_t *new);

static proc_t proczero;

pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    pgtbl_t root = pmem_alloc(true);
    if (!root)
        return NULL;
    if (vm_mappages(root, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X) ||
        vm_mappages(root, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W))
    {
        vm_freewalk(root);
        return NULL;
    }
    return root;
}

static int load_initcode(proc_t *p)
{
    void *code = pmem_alloc(false);
    if (!code)
        return -1;
    assert(target_user_initcode_len <= PGSIZE, "initcode size");
    memmove(code, target_user_initcode, target_user_initcode_len);
    if (vm_mappages(p->pgtbl, USER_BASE, (uint64)code, PGSIZE, PTE_U | PTE_R | PTE_X))
    {
        pmem_free((uint64)code, false);
        return -1;
    }
    if (uvm_ustack_grow(p->pgtbl, 0, USER_STACK_TOP - PGSIZE) < 0)
        return -1;

    p->heap_top = USER_HEAP_BASE;
    p->ustack_npage = 1;
    p->mmap = NULL;
    p->tf->sp = USER_STACK_TOP;
    p->tf->user_to_kern_epc = USER_BASE;
    // The instruction page was written as data before being executable.
    asm volatile("fence.i" ::: "memory");
    return 0;
}

void proc_make_first(void)
{
    proc_t *p = &proczero;
    p->pid = 1;
    p->kstack = KSTACK(0);
    p->tf = pmem_alloc(true);
    if (p->tf)
        p->pgtbl = proc_pgtbl_init((uint64)p->tf);
    if (!p->pgtbl || load_initcode(p) < 0)
    {
        uvm_destroy_pgtbl(p->pgtbl);
        if (p->tf)
            pmem_free((uint64)p->tf, true);
        panic("proc_make_first");
    }

    p->ctx.ra = (uint64)trap_user_return;
    p->ctx.sp = p->kstack + PGSIZE;
    intr_off();
    cpu_t *cpu = mycpu();
    cpu->proc = p;
    swtch(&cpu->ctx, &p->ctx);
    panic("proczero returned");
}
