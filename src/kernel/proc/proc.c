#include "mod.h"
#include "initcode.h"

extern char trampoline[];
extern void swtch(context_t *old, context_t *new);

static proc_t proc_list[N_PROC];
static proc_t *proczero;
static spinlock_t pid_lock;
static spinlock_t wait_lock;
static uint32 next_pid;

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

void proc_init(void)
{
    spinlock_init(&pid_lock, "pid");
    spinlock_init(&wait_lock, "wait");
    next_pid = 1;
    for (int i = 0; i < N_PROC; i++)
    {
        proc_t *p = &proc_list[i];
        spinlock_init(&p->lk, "proc");
        p->state = UNUSED;
        p->kstack = KSTACK(i);
    }
}

static int alloc_pid(void)
{
    spinlock_acquire(&pid_lock);
    int pid = -1;
    if (next_pid <= 0x7fffffffu)
        pid = next_pid++;
    spinlock_release(&pid_lock);
    return pid;
}

proc_t *proc_alloc(void)
{
    for (proc_t *p = proc_list; p < proc_list + N_PROC; p++)
    {
        spinlock_acquire(&p->lk);
        if (p->state != UNUSED)
        {
            spinlock_release(&p->lk);
            continue;
        }

        p->state = EMBRYO;
        p->pid = alloc_pid();
        if (p->pid > 0)
        {
            p->tf = pmem_alloc(true);
            if (p->tf)
                p->pgtbl = proc_pgtbl_init((uint64)p->tf);
        }
        if (!p->pgtbl)
        {
            proc_free(p);
            spinlock_release(&p->lk);
            return NULL;
        }

        p->ctx = (context_t){ .ra = (uint64)proc_return, .sp = p->kstack + PGSIZE };
        return p;
    }
    return NULL;
}

void proc_free(proc_t *p)
{
    assert(spinlock_holding(&p->lk) &&
           (p->state == EMBRYO || p->state == ZOMBIE), "proc_free state");
    assert(p->state != ZOMBIE || spinlock_holding(&wait_lock), "proc_free wait lock");

    uvm_destroy_pgtbl(p->pgtbl);
    if (p->tf)
        pmem_free((uint64)p->tf, true);
    while (p->mmap)
    {
        mmap_region_t *node = p->mmap;
        p->mmap = node->next;
        mmap_region_free(node);
    }

    p->pgtbl = NULL;
    p->tf = NULL;
    p->pid = 0;
    p->heap_top = 0;
    p->ustack_npage = 0;
    p->exit_code = 0;
    p->sleep_space = NULL;
    p->name[0] = '\0';
    p->ctx = (context_t){0};
    if (p->state == ZOMBIE)
        p->parent = NULL;
    p->state = UNUSED;
}

void proc_make_first(void)
{
    proc_t *p = proc_alloc();
    if (!p)
        panic("cannot allocate proczero");
    if (load_initcode(p) < 0)
    {
        proc_free(p);
        spinlock_release(&p->lk);
        panic("cannot load proczero");
    }
    memmove(p->name, "proczero", sizeof("proczero"));
    proczero = p;
    p->state = RUNNABLE;
    spinlock_release(&p->lk);
}

void proc_return(void)
{
    spinlock_release(&myproc()->lk);
    trap_user_return();
}

static int copy_mmap(proc_t *child, const mmap_region_t *node)
{
    mmap_region_t **tail = &child->mmap;
    for (; node; node = node->next)
    {
        mmap_region_t *copy = mmap_region_alloc();
        if (!copy)
            return -1;
        *copy = *node;
        copy->next = NULL;
        *tail = copy;
        tail = &copy->next;
    }
    return 0;
}

int proc_fork(void)
{
    proc_t *parent = myproc();
    proc_t *child = proc_alloc();
    if (!child)
        return -1;

    if (copy_mmap(child, parent->mmap) < 0 ||
        uvm_copy_pgtbl(parent->pgtbl, child->pgtbl, parent->heap_top,
                      parent->ustack_npage, parent->mmap) < 0)
    {
        proc_free(child);
        spinlock_release(&child->lk);
        return -1;
    }
    child->heap_top = parent->heap_top;
    child->ustack_npage = parent->ustack_npage;
    *child->tf = *parent->tf;
    child->tf->a0 = 0;
    memmove(child->name, parent->name, sizeof(child->name));
    int pid = child->pid;
    spinlock_release(&child->lk);

    // While EMBRYO, the child can neither run nor be reaped.
    spinlock_acquire(&wait_lock);
    spinlock_acquire(&child->lk);
    child->parent = parent;
    child->state = RUNNABLE;
    spinlock_release(&child->lk);
    spinlock_release(&wait_lock);
#ifdef LAB6_TRACE
    printf("fork: %d -> %d\n", parent->pid, pid);
#endif
    return pid;
}

static void proc_try_wakeup(proc_t *p)
{
    assert(spinlock_holding(&p->lk), "proc_try_wakeup lock");
    if (p->state == SLEEPING && p->sleep_space == p)
        p->state = RUNNABLE;
}

static void wake_waiter(proc_t *p)
{
    if (!p)
        return;
    spinlock_acquire(&p->lk);
    proc_try_wakeup(p);
    spinlock_release(&p->lk);
}

static void proc_reparent(proc_t *parent)
{
    if (parent == proczero)
        return;
    bool adopted = false;
    for (proc_t *p = proc_list; p < proc_list + N_PROC; p++)
    {
        if (p->parent == parent)
        {
            p->parent = proczero;
            adopted = true;
        }
    }
    if (adopted)
        wake_waiter(proczero);
}

void proc_exit(int exit_code)
{
    proc_t *p = myproc();
    spinlock_acquire(&wait_lock);
    proc_reparent(p);
    wake_waiter(p->parent);

    spinlock_acquire(&p->lk);
    p->exit_code = exit_code;
    p->state = ZOMBIE;
    spinlock_release(&wait_lock);
    proc_sched();
    panic("zombie was scheduled");
}

int proc_wait(uint64 user_addr)
{
    proc_t *parent = myproc();
    spinlock_acquire(&wait_lock);
    for (;;)
    {
        bool has_child = false;
        for (proc_t *child = proc_list; child < proc_list + N_PROC; child++)
        {
            if (child->parent != parent)
                continue;
            has_child = true;
            spinlock_acquire(&child->lk);
            if (child->state == ZOMBIE)
            {
                int pid = child->pid;
                if (user_addr && uvm_copyout(parent->pgtbl, user_addr,
                                              (uint64)&child->exit_code, sizeof(int)) < 0)
                    pid = -1;
                else
                    proc_free(child);
                spinlock_release(&child->lk);
                spinlock_release(&wait_lock);
                return pid;
            }
            spinlock_release(&child->lk);
        }
        if (!has_child)
        {
            spinlock_release(&wait_lock);
            return -1;
        }
        proc_sleep(parent, &wait_lock);
    }
}

void proc_sleep(void *sleep_space, spinlock_t *lock)
{
    proc_t *p = myproc();
    assert(p && sleep_space && lock != &p->lk && spinlock_holding(lock),
           "proc_sleep: condition lock");

    // Holding both locks briefly closes the check-to-sleep window.
    spinlock_acquire(&p->lk);
    spinlock_release(lock);
    p->sleep_space = sleep_space;
    p->state = SLEEPING;
    proc_sched();

    p->sleep_space = NULL;
    spinlock_release(&p->lk);
    spinlock_acquire(lock);
}

void proc_wakeup(void *sleep_space)
{
    proc_t *caller = myproc();
    for (proc_t *p = proc_list; p < proc_list + N_PROC; p++)
    {
        if (p == caller)
            continue;
        spinlock_acquire(&p->lk);
        if (p->state == SLEEPING && p->sleep_space == sleep_space)
            p->state = RUNNABLE;
        spinlock_release(&p->lk);
    }
}

void proc_yield(void)
{
    proc_t *p = myproc();
    spinlock_acquire(&p->lk);
    assert(p->state == RUNNING, "proc_yield state");
    p->state = RUNNABLE;
    proc_sched();
    spinlock_release(&p->lk);
}

void proc_sched(void)
{
    proc_t *p = myproc();
    cpu_t *cpu = mycpu();
    assert(spinlock_holding(&p->lk) && cpu->noff == 1 &&
           p->state != RUNNING && !intr_get(), "proc_sched state");
    int intena = cpu->intena;
    swtch(&p->ctx, &cpu->ctx);
    // A different hart may have resumed this saved context.
    mycpu()->intena = intena;
}

void proc_scheduler(void)
{
    cpu_t *cpu = mycpu();
    cpu->proc = NULL;
    for (;;)
    {
        // Give pending interrupts a chance to run before the scan. Keep
        // interrupts off across scan -> wfi so a pending tick wakes wfi.
        intr_on();
        intr_off();
        bool ran = false;
        for (proc_t *p = proc_list; p < proc_list + N_PROC; p++)
        {
            spinlock_acquire(&p->lk);
            if (p->state == RUNNABLE)
            {
                p->state = RUNNING;
                cpu->proc = p;
                asm volatile("fence.i" ::: "memory");
                swtch(&cpu->ctx, &p->ctx);
                cpu->proc = NULL;
                ran = true;
            }
            spinlock_release(&p->lk);
        }
        if (!ran)
            asm volatile("wfi");
    }
}
