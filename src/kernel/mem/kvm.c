#include "mod.h"
#include "../proc/mod.h"

static pgtbl_t kernel_pgtbl;
extern char trampoline[];

// Only a detached or inactive page table may carry these temporary RSW bits.
// Both newly linked tables and newly installed leaves are marked for rollback.
#define PTE_STAGED (1ul << 8)
#define PA_MAX (1ull << 56)
#define PT_ENTRIES (PGSIZE / sizeof(pte_t))

static pte_t *walk(pgtbl_t pgtbl, uint64 va, bool alloc, pte_t new_flag)
{
    if (!pgtbl || va >= VA_MAX)
        return NULL;

    pte_t *first_new = NULL;
    for (int level = 2; level > 0; level--)
    {
        pte_t *entry = &pgtbl[VA_TO_VPN(va, level)];
        if (*entry & PTE_V)
        {
            assert(PTE_CHECK(*entry), "walk: unexpected upper leaf");
            pgtbl = (pgtbl_t)PTE_TO_PA(*entry);
            continue;
        }
        if (!alloc)
            return NULL;
        pgtbl = pmem_alloc(true);
        if (!pgtbl)
        {
            // Sv39 has two intermediate levels. Only the first can have
            // been created when allocating the second fails.
            if (first_new)
            {
                pmem_free(PTE_TO_PA(*first_new), true);
                *first_new = 0;
            }
            return NULL;
        }
        *entry = PA_TO_PTE(pgtbl) | PTE_V | new_flag;
        first_new = entry;
    }
    return &pgtbl[VA_TO_VPN(va, 0)];
}

pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    return walk(pgtbl, va, alloc, 0);
}

bool vm_valid_perm(int perm)
{
    return !(perm & ~PTE_PERM) && (perm & (PTE_R | PTE_X)) &&
           (!(perm & PTE_W) || (perm & PTE_R));
}

int vm_stage_page(pgtbl_t pgtbl, uint64 va, uint64 pa, int perm)
{
    if (va % PGSIZE || pa % PGSIZE || pa >= PA_MAX || !vm_valid_perm(perm))
        return -1;
    pte_t *pte = walk(pgtbl, va, true, PTE_STAGED);
    if (!pte || (*pte & PTE_V))
        return -1;
    *pte = PA_TO_PTE(pa) | perm | PTE_V | PTE_MACHINE | PTE_STAGED;
    return 0;
}

static void finish(pgtbl_t pgtbl, int level, bool commit, bool free_data)
{
    for (uint32 i = 0; i < PT_ENTRIES; i++)
    {
        pte_t pte = pgtbl[i];
        if (!(pte & PTE_V))
            continue;
        if (level)
            finish((pgtbl_t)PTE_TO_PA(pte), level - 1, commit, free_data);
        if (!(pte & PTE_STAGED))
            continue;
        pgtbl[i] = commit ? pte & ~PTE_STAGED : 0;
        if (!commit && (level || free_data))
            pmem_free(PTE_TO_PA(pte), level != 0);
    }
}

void vm_finish(pgtbl_t pgtbl, bool commit, bool free_data)
{
    if (pgtbl)
        finish(pgtbl, 2, commit, free_data);
}

int vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    if (!pgtbl || va >= VA_MAX || !len || len > VA_MAX - va ||
        va % PGSIZE || pa % PGSIZE || pa >= PA_MAX || !vm_valid_perm(perm))
        return -1;
    uint64 size = ALIGN_UP(len, PGSIZE);
    if (size > PA_MAX - pa)
        return -1;

    for (uint64 off = 0; off < size; off += PGSIZE)
    {
        if (vm_stage_page(pgtbl, va + off, pa + off, perm) < 0)
        {
            vm_finish(pgtbl, false, false);
            return -1;
        }
    }
    vm_finish(pgtbl, true, false);
    return 0;
}

void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    assert(pgtbl && va < VA_MAX && len && len <= VA_MAX - va &&
           va % PGSIZE == 0, "vm_unmappages: range");
    uint64 end = va + ALIGN_UP(len, PGSIZE);
    for (; va < end; va += PGSIZE)
    {
        pte_t *pte = vm_getpte(pgtbl, va, false);
        assert(pte && (*pte & PTE_V) && !PTE_CHECK(*pte),
               "vm_unmappages: missing leaf");
        uint64 pa = PTE_TO_PA(*pte);
        *pte = 0;
        if (freeit)
            pmem_free(pa, check_inkernel(pa));
    }
}

static void freewalk(pgtbl_t pgtbl, int level, bool free_user)
{
    for (uint32 i = 0; i < PT_ENTRIES; i++)
    {
        pte_t pte = pgtbl[i];
        if (!(pte & PTE_V))
            continue;
        uint64 pa = PTE_TO_PA(pte);
        if (level)
            freewalk((pgtbl_t)pa, level - 1, free_user);
        else if (free_user && (pte & PTE_U))
            pmem_free(pa, false);
    }
    pmem_free((uint64)pgtbl, true);
}

void vm_freewalk(pgtbl_t pgtbl)
{
    if (pgtbl)
        freewalk(pgtbl, 2, false);
}

void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    if (pgtbl)
        freewalk(pgtbl, 2, true);
}

void kvm_init(void)
{
    pgtbl_t root = pmem_alloc(true);
    uint64 data = (uint64)KERNEL_DATA;
    if (!root ||
        vm_mappages(root, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W) ||
        vm_mappages(root, PLIC_BASE, PLIC_BASE, PLIC_SIZE, PTE_R | PTE_W) ||
        vm_mappages(root, KERNEL_BASE, KERNEL_BASE, data - KERNEL_BASE, PTE_R | PTE_X) ||
        vm_mappages(root, data, data, (uint64)ALLOC_END - data, PTE_R | PTE_W) ||
        vm_mappages(root, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X))
        goto fail;
    for (int i = 0; i < N_PROC; i++)
    {
        void *stack = pmem_alloc(true);
        if (!stack)
            goto free_stacks;
        if (vm_mappages(root, KSTACK(i), (uint64)stack, PGSIZE, PTE_R | PTE_W))
        {
            pmem_free((uint64)stack, true);
            goto free_stacks;
        }
    }
    kernel_pgtbl = root;
    return;

free_stacks:
    for (int i = 0; i < N_PROC; i++)
    {
        pte_t *pte = vm_getpte(root, KSTACK(i), false);
        if (!pte || !(*pte & PTE_V))
            break;
        vm_unmappages(root, KSTACK(i), PGSIZE, true);
    }
fail:
    vm_freewalk(root);
    panic("kvm_init");
}

void kvm_inithart(void)
{
    sfence_vma();
    w_satp(MAKE_SATP(kernel_pgtbl));
    sfence_vma();
}

static void print_entries(pgtbl_t table, int level)
{
    for (uint32 i = 0; i < PT_ENTRIES; i++)
    {
        pte_t pte = table[i];
        if (!(pte & PTE_V))
            continue;
        printf("level %d index %d: pte %x pa %x\n",
               level, i, pte, PTE_TO_PA(pte));
        if (level && PTE_CHECK(pte))
            print_entries((pgtbl_t)PTE_TO_PA(pte), level - 1);
    }
}

void vm_print(pgtbl_t pgtbl)
{
    printf("pgtbl: %x\n", (uint64)pgtbl);
    print_entries(pgtbl, 2);
}
