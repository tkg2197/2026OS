#include "mod.h"

static pgtbl_t kernel_pgtbl;

static void vm_sync(void) {
    __sync_synchronize();
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

static uint64 vm_round_len(uint64 va, uint64 len) {
    assert((va & (PGSIZE - 1)) == 0, "vm: unaligned va");
    assert(va < VA_MAX && len > 0 && len <= VA_MAX - va, "vm: bad range");
    return (len + PGSIZE - 1) & ~((uint64)PGSIZE - 1);
}

static void vm_check_leaf(pte_t pte, int level)
{
    assert(PTE_LEAF(pte), "vm: not a leaf");
    assert(!(pte & PTE_W) || (pte & PTE_R), "vm: W requires R");
    assert((PTE_TO_PA(pte) & (VM_LEVEL_SIZE(level) - 1)) == 0,
           "vm: misaligned leaf pa");
}

static void vm_split(pte_t *pte, int level) {
    assert(level > 0 && level <= 2, "vm_split: bad level");
    pte_t old = *pte;
    assert(old & PTE_V, "vm_split: invalid pte");
    vm_check_leaf(old, level);
    pgtbl_t child = (pgtbl_t)pmem_alloc(true);
    uint64 pa = PTE_TO_PA(old);
    uint64 step = VM_LEVEL_SIZE(level - 1);
    for (int i = 0; i < 512; i++)
        child[i] = PA_TO_PTE(pa + (uint64)i * step) | PTE_FLAGS(old); 
    __sync_synchronize();
    *pte = PA_TO_PTE(child) | PTE_V | (old & PTE_G);
    vm_sync();
}

static pte_t *vm_walk_level(pgtbl_t pgtbl, uint64 va, int target, bool alloc) {
    assert(pgtbl != NULL, "vm_walk: null page table");
    assert(va < VA_MAX, "vm_walk: va out of range");
    assert(target >= 0 && target <= 2, "vm_walk: bad level");
    for (int level = 2; level > target; level--) {
        pte_t *pte = &pgtbl[VA_TO_VPN(va, level)];
        if (*pte & PTE_V) {
            if (PTE_LEAF(*pte)) {
                if (!alloc)
                    return NULL;
                vm_split(pte, level);
            }
        }
        else {
            if (!alloc)
                return NULL;
            pgtbl_t child = (pgtbl_t)pmem_alloc(true);
            __sync_synchronize();
            *pte = PA_TO_PTE(child) | PTE_V;
        }
        pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
    }
    return &pgtbl[VA_TO_VPN(va, target)];
}

pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc) {
    return vm_walk_level(pgtbl, va, 0, alloc);
}

pte_t *vm_lookup(pgtbl_t pgtbl, uint64 va, int *level_out) {
    assert(pgtbl != NULL, "vm_lookup: null page table");
    assert(va < VA_MAX, "vm_lookup: va out of range");
    if (level_out != NULL)
        *level_out = -1;
    for (int level = 2; level >= 0; level--) {
        pte_t *pte = &pgtbl[VA_TO_VPN(va, level)];
        if (!(*pte & PTE_V))
            return NULL;
        if (PTE_LEAF(*pte)) {
            vm_check_leaf(*pte, level);
            if (level_out != NULL)
                *level_out = level;
            return pte;
        }
        assert(level > 0, "vm_lookup: bottom non-leaf");
        pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
    }
    return NULL;
}

static void vm_map_range(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm, int max_level) {
    assert(pgtbl != NULL, "vm_map: null page table");
    uint64 remain = vm_round_len(va, len);
    assert((pa & (PGSIZE - 1)) == 0, "vm_map: unaligned pa");
    assert(pa < (1ul << 56) && remain <= (1ul << 56) - pa, "vm_map: pa out of range");
    assert((perm & ~0x3FF) == 0, "vm_map: bad flags");
    assert(!(perm & PTE_W) || (perm & PTE_R), "vm_map: W requires R");
    assert((perm & (PTE_R | PTE_X)) != 0, "vm_map: not a leaf");
    uint64 flags = (uint64)perm | PTE_V | PTE_A;
    if (perm & PTE_W)
        flags |= PTE_D;
    while (remain != 0) {
        int level = max_level;
        pte_t *pte = NULL;
        uint64 size;

        for (;;) {
            size = VM_LEVEL_SIZE(level);
            if (size <= remain && ((va | pa) & (size - 1)) == 0) {
                pte = vm_walk_level(pgtbl, va, level, true);
                if (!(*pte & PTE_V) || PTE_LEAF(*pte))
                    break;
                assert(level > 0, "vm_map: bottom non-leaf");
            }
            assert(level > 0, "vm_map: cannot select page size");
            level--;
        }
        *pte = PA_TO_PTE(pa) | flags;
        va += size;
        pa += size;
        remain -= size;
    }
    vm_sync();
}

void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm) {
    vm_map_range(pgtbl, va, pa, len, perm, 0);
}

void vm_mappages_huge(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm) {
    vm_map_range(pgtbl, va, pa, len, perm, 2);
}

void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit) {
    uint64 end = va + vm_round_len(va, len);
    while (va < end) {
        int level;
        pte_t *pte = vm_lookup(pgtbl, va, &level);
        assert(pte != NULL, "vm_unmap: not mapped");
        uint64 size = VM_LEVEL_SIZE(level);
        assert(!freeit || level == 0, "vm_unmap: huge backing must be freed by its owner");
        if ((va & (size - 1)) != 0 || size > end - va) {
            vm_split(pte, level);
            continue;
        }
        uint64 pa = PTE_TO_PA(*pte);
        *pte = 0;
        vm_sync();
        if (freeit)
            pmem_free(pa, false);
        va += size;
    }
}

void kvm_init(void) {
    uint64 data = (uint64)KERNEL_DATA;
    uint64 begin = (uint64)ALLOC_BEGIN;
    uint64 end = (uint64)ALLOC_END;
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    vm_mappages(kernel_pgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);
    vm_mappages(kernel_pgtbl, PLIC_BASE, PLIC_BASE, PLIC_SIZE, PTE_R | PTE_W);
    vm_mappages_huge(kernel_pgtbl, KERNEL_BASE, KERNEL_BASE, data - KERNEL_BASE, PTE_R | PTE_X);
    vm_mappages_huge(kernel_pgtbl, data, data, begin - data, PTE_R | PTE_W);
    vm_mappages_huge(kernel_pgtbl, begin, begin, end - begin, PTE_R | PTE_W);
}

void kvm_inithart(void) {
    vm_sync();
    w_satp(MAKE_SATP(kernel_pgtbl));
    vm_sync();
}

static void vm_print_level(pgtbl_t pgtbl, int level, uint64 base)
{
    for (int i = 0; i < 512; i++) {
        pte_t pte = pgtbl[i];

        if (!(pte & PTE_V))
            continue;

        uint64 va = base + (uint64)i * VM_LEVEL_SIZE(level);

        if (PTE_LEAF(pte)) {
            vm_check_leaf(pte, level);

            printf("L%d leaf: va=%p pa=%p size=%x flags=%x\n",
                   level, (void *)va, (void *)PTE_TO_PA(pte),
                   VM_LEVEL_SIZE(level), (uint64)PTE_FLAGS(pte));
        } else {
            assert(level > 0, "vm_print: bottom non-leaf");

            printf("L%d table: va=%p child=%p\n", level,
                   (void *)va, (void *)PTE_TO_PA(pte));

            vm_print_level((pgtbl_t)PTE_TO_PA(pte), level - 1, va);
        }
    }
}

void vm_print(pgtbl_t pgtbl)
{
    assert(pgtbl != NULL, "vm_print: null page table");
    printf("root=%p\n", (void *)pgtbl);
    vm_print_level(pgtbl, 2, 0);
}

static uint32 vm_count_level(pgtbl_t pgtbl, int level)
{
    uint32 count = 1; // 当前页表页本身。

    for (int i = 0; i < 512; i++) {
        pte_t pte = pgtbl[i];

        if (!(pte & PTE_V) || PTE_LEAF(pte))
            continue;

        assert(level > 0, "vm_count: bottom non-leaf");

        count += vm_count_level((pgtbl_t)PTE_TO_PA(pte), level - 1);
    }

    return count;
}

uint32 vm_count_pgtbl(pgtbl_t pgtbl)
{
    assert(pgtbl != NULL, "vm_count: null page table");
    return vm_count_level(pgtbl, 2);
}

static void vm_free_level(pgtbl_t pgtbl, int level)
{
    for (int i = 0; i < 512; i++) {
        pte_t pte = pgtbl[i];

        if (!(pte & PTE_V))
            continue;

        assert(!PTE_LEAF(pte), "vm_freewalk: leaf remains");
        assert(level > 0, "vm_freewalk: bottom non-leaf");

        vm_free_level((pgtbl_t)PTE_TO_PA(pte), level - 1);
        pgtbl[i] = 0;
    }

    pmem_free((uint64)pgtbl, true);
}

// 只能销毁已停用、没有有效叶子且未与其他根共享子表的页表。
void vm_freewalk(pgtbl_t pgtbl)
{
    assert(pgtbl != NULL, "vm_freewalk: null page table");
    vm_free_level(pgtbl, 2);
}
