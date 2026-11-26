#include "mod.h"
#include "../trap/mod.h"
#include "../../user/initcode.h"
#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

// 在kernel.ld中已经完成申请
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
    memset(p, 0, sizeof(*p));
    if (initcode_len == 0 || initcode_len > PGSIZE)
        goto fail;
    p->tf = pmem_try_alloc(true);
    if (p->tf == NULL)
        goto fail;
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);
    if (p->pgtbl == NULL)
        goto free_tf;

    void *stack = pmem_try_alloc(false);
    if (stack == NULL)
        goto free_table;
    if (!vm_try_mappage(p->pgtbl, USTACK, (uint64)stack, PTE_R | PTE_W | PTE_U)) {
        pmem_free((uint64)stack, false);
        goto free_table;
    }

    void *code = pmem_try_alloc(false);
    if (code == NULL)
        goto free_stack;
    memmove(code, initcode, initcode_len);
    asm volatile("fence.i" ::: "memory");
    if (!vm_try_mappage(p->pgtbl, USER_BASE, (uint64)code, PTE_R | PTE_X | PTE_U)) {
        pmem_free((uint64)code, false);
        goto free_stack;
    }

    p->pid = 0;
    p->heap_top = USER_BASE + PGSIZE;
    p->ustack_npage = 1;
    p->tf->user_to_kern_epc = USER_BASE; // 切换上下文后会从这里开始执行
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
free_stack:
    vm_unmappages(p->pgtbl, USTACK, PGSIZE, true);
free_table:
    vm_unmappages(p->pgtbl, TRAPFRAME, PGSIZE, false);
    vm_unmappages(p->pgtbl, TRAMPOLINE, PGSIZE, false);
    vm_freewalk(p->pgtbl);
free_tf:
    pmem_free((uint64)p->tf, true);
fail:
    memset(p, 0, sizeof(*p));
    printf("proczero: creation failed (resources or image)\n");
}
