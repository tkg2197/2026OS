#include "mod.h"
#include "../trap/mod.h"
#include "../../user/initcode.h"
#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

extern char trampoline[];
extern void swtch(context_t *old, context_t *new);
static proc_t proczero;

pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    pgtbl_t table = pmem_try_alloc(true);
    if (table == NULL)
        return NULL;
    if (!vm_try_mappage(table, TRAMPOLINE, (uint64)trampoline, PTE_R | PTE_X))
    {
        vm_freewalk(table);
        return NULL;
    }
    if (!vm_try_mappage(table, TRAPFRAME, trapframe, PTE_R | PTE_W))
    {
        vm_unmappages(table, TRAMPOLINE, PGSIZE, false);
        vm_freewalk(table);
        return NULL;
    }
    return table;
}

void proc_make_first(void)
{
    assert(mycpuid() == 0 && myproc() == NULL, "first process: wrong cpu/state");
    proc_t *p = &proczero;
    void *stack = NULL, *code = NULL;
    bool stack_mapped = false, code_mapped = false;
    memset(p, 0, sizeof(*p));
    if (initcode_len == 0 || initcode_len > PGSIZE)
        goto fail;
    p->tf = pmem_try_alloc(true);
    if (p->tf == NULL)
        goto fail;
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);
    if (p->pgtbl == NULL)
        goto fail;
    stack = pmem_try_alloc(false);
    if (stack == NULL)
        goto fail;
    if (!vm_try_mappage(p->pgtbl, USTACK, (uint64)stack, PTE_R | PTE_W | PTE_U))
        goto fail;
    stack_mapped = true;
    code = pmem_try_alloc(false);
    if (code == NULL)
        goto fail;
    memmove(code, initcode, initcode_len);
    asm volatile("fence.i" ::: "memory");
    if (!vm_try_mappage(p->pgtbl, USER_BASE, (uint64)code, PTE_R | PTE_X | PTE_U))
        goto fail;
    code_mapped = true;
    p->pid = 0;
    p->heap_top = USER_BASE + PGSIZE;
    p->ustack_npage = 1;
    p->tf->user_to_kern_epc = USER_BASE;
    p->tf->sp = USTACK + PGSIZE;
    p->kstack = KSTACK(0);
    p->ctx.ra = (uint64)trap_user_return;
    p->ctx.sp = p->kstack + PGSIZE;
    // 所有资源就绪后才发布。swtch之后由trap_user_return准备sret。
    intr_off();
    cpu_t *cpu = mycpu();
    cpu->proc = p;
    swtch(&cpu->ctx, &p->ctx);
    panic("first process: unexpected context return");
fail:
    if (p->pgtbl != NULL)
    {
        if (code_mapped)
            vm_unmappages(p->pgtbl, USER_BASE, PGSIZE, false);
        if (stack_mapped)
            vm_unmappages(p->pgtbl, USTACK, PGSIZE, false);
        vm_unmappages(p->pgtbl, TRAPFRAME, PGSIZE, false);
        vm_unmappages(p->pgtbl, TRAMPOLINE, PGSIZE, false);
        vm_freewalk(p->pgtbl);
    }
    if (code != NULL)
        pmem_free((uint64)code, false);
    if (stack != NULL)
        pmem_free((uint64)stack, false);
    if (p->tf != NULL)
        pmem_free((uint64)p->tf, true);
    memset(p, 0, sizeof(*p));
    printf("proczero: creation failed (resources or image)\n");
}
