#include "mod.h"

static pgtbl_t kernel_pgtbl;

// 确保上面所有命令都执行完，然后清空TLB
static void vm_sync(void) {
    __sync_synchronize();
    asm volatile("sfence.vma zero, zero" ::: "memory");
}

// 把len向上取整对齐
static uint64 vm_round_len(uint64 va, uint64 len) {
    assert((va & (PGSIZE - 1)) == 0, "vm: unaligned va");
    assert(va < VA_MAX && len > 0 && len <= VA_MAX - va, "vm: bad range");
    return (len + PGSIZE - 1) & ~((uint64)PGSIZE - 1);
}

// 是否是一个合法的叶子页
static void vm_check_leaf(pte_t pte, int level)
{
    assert(PTE_LEAF(pte), "vm: not a leaf");
    assert(!(pte & PTE_W) || (pte & PTE_R), "vm: W requires R");
    assert((PTE_TO_PA(pte) & (VM_LEVEL_SIZE(level) - 1)) == 0,
           "vm: misaligned leaf pa");
}

// 把原来的大页拆成512个小的叶子页，并申请一个新的页填入原来的pte来映射这些新的页
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

// 根据va查询到对应level页表对应的pte指针
static pte_t *vm_walk_level(pgtbl_t pgtbl, uint64 va, int target, bool alloc) {
    assert(pgtbl != NULL, "vm_walk: null page table");
    assert(va < VA_MAX, "vm_walk: va out of range");
    assert(target >= 0 && target <= 2, "vm_walk: bad level");
    for (int level = 2; level > target; level--) {
        pte_t *pte = &pgtbl[VA_TO_VPN(va, level)];
        if (*pte & PTE_V) {
            if (PTE_LEAF(*pte)) {
                if (!alloc) // 不允许分配新的页
                    return NULL;
                vm_split(pte, level);
            }
        }
        else { // 还没建立映射
            if (!alloc)
                return NULL;
            pgtbl_t child = (pgtbl_t)pmem_alloc(true);
            __sync_synchronize();
            *pte = PA_TO_PTE(child) | PTE_V; // 在这个pte中填入新申请的页（非叶子页）
        }
        pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
    }
    return &pgtbl[VA_TO_VPN(va, target)];
}

pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc) {
    return vm_walk_level(pgtbl, va, 0, alloc);
}

// 从va向下查，直到遇到叶子项，返回叶子项的level和pte指针
pte_t *vm_lookup(pgtbl_t pgtbl, uint64 va, int *level_out) {
    assert(pgtbl != NULL, "vm_lookup: null page table");
    assert(va < VA_MAX, "vm_lookup: va out of range");
    if (level_out != NULL)
        *level_out = -1;
    for (int level = 2; level >= 0; level--) {
        pte_t *pte = &pgtbl[VA_TO_VPN(va, level)];
        if (!(*pte & PTE_V)) // 还没完成建立映射
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

// 将va开始的len长度的连续地址映射到pa开始的连续地址，在满足大小和对齐条件下使用大页
// 这个函数是分配的底层函数，所以要求物理地址连续，上层函数当然不要求连续
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

// 解除映射。特别注意，这里解除的映射可能是一个大页的一部分，只要len是4kb对齐的就是合法的
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit) {
    uint64 end = va + vm_round_len(va, len);
    while (va < end) {
        int level;
        pte_t *pte = vm_lookup(pgtbl, va, &level);
        assert(pte != NULL, "vm_unmap: not mapped");
        uint64 size = VM_LEVEL_SIZE(level);
        assert(!freeit || level == 0, "vm_unmap: huge backing must be freed by its owner"); // 只允许释放4kb的小页，大页必须由申请者调用pmem_free_order来释放
        if ((va & (size - 1)) != 0 || size > end - va) { // va从一个大页的中间开始，或者想解除映射的长度没有到大页的结尾
            vm_split(pte, level); // 把大页拆分，以更细的粒度来解除映射
            continue;
        }
        uint64 pa = PTE_TO_PA(*pte);
        *pte = 0; // 清空pte
        vm_sync();
        if (freeit)
            pmem_free(pa, false);
        va += size;
    }
}

// 这里建立映射的物理内存都已经先在region_init中分配好了
void kvm_init(void) {
    uint64 data = (uint64)KERNEL_DATA;
    uint64 begin = (uint64)ALLOC_BEGIN;
    uint64 end = (uint64)ALLOC_END;
    extern char trampoline[];
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true); // 根页表
    vm_mappages(kernel_pgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);
    vm_mappages(kernel_pgtbl, PLIC_BASE, PLIC_BASE, PLIC_SIZE, PTE_R | PTE_W);
    vm_mappages_huge(kernel_pgtbl, KERNEL_BASE, KERNEL_BASE, data - KERNEL_BASE, PTE_R | PTE_X);
    vm_mappages_huge(kernel_pgtbl, data, data, begin - data, PTE_R | PTE_W);
    vm_mappages_huge(kernel_pgtbl, begin, begin, end - begin, PTE_R | PTE_W); // 可分配内存，分为内核空间和用户空间
    vm_mappages(kernel_pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    uint64 stack = (uint64)pmem_alloc(true); // 从内核空间中申请内核栈
    vm_mappages(kernel_pgtbl, KSTACK(0), stack, PGSIZE, PTE_R | PTE_W); // 内核栈,暂时只分配第0进程的
}

void kvm_inithart(void) {
    vm_sync();
    w_satp(MAKE_SATP(kernel_pgtbl)); // 启用页表
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

// 要求先调用vm_unmappages清空pte，然后这个函数清空页表
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

// 用户页表在发布前由构造者独占；不拆分大页，也不覆盖已有映射。
bool vm_try_mappage(pgtbl_t pgtbl, uint64 va, uint64 pa, int perm)
{
    if (pgtbl == NULL || va >= VA_MAX || ((va | pa) & (PGSIZE - 1)) ||
        pa >= (1ull << 56) || (perm & ~(PTE_R | PTE_W | PTE_X | PTE_U)) ||
        !(perm & (PTE_R | PTE_X)) || ((perm & PTE_W) && !(perm & PTE_R)))
        return false;
    // 修改了父表中哪些槽位
    pte_t *created[2];
    // 申请了哪些新物理页
    pgtbl_t pages[2];
    int count = 0;
    for (int level = 2; level > 0; level--)
    {
        pte_t *slot = &pgtbl[VA_TO_VPN(va, level)];
        if (*slot & PTE_V)
        {
            if (PTE_LEAF(*slot))
                goto fail;
        }
        else
        {
            pgtbl_t child = pmem_try_alloc(true);
            if (child == NULL)
                goto fail;
            created[count] = slot;
            pages[count++] = child;
            *slot = PA_TO_PTE(child) | PTE_V;
        }
        pgtbl = (pgtbl_t)PTE_TO_PA(*slot);
    }
    pte_t *leaf = &pgtbl[VA_TO_VPN(va, 0)];
    if (*leaf & PTE_V)
        goto fail;
    *leaf = PA_TO_PTE(pa) | perm | PTE_V | PTE_A |
            ((perm & PTE_W) ? PTE_D : 0);
    __sync_synchronize();
    return true;
fail:
    while (count > 0)
    {
        count--;
        *created[count] = 0;
        pmem_free((uint64)pages[count], true);
    }
    return false;
}
