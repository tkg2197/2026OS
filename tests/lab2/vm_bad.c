#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#ifndef VM_BAD_CASE
#define VM_BAD_CASE 1
#endif
void main(void)
{
    if (r_tp() == 0) {
        print_init();
        pmem_init();
        pgtbl_t root = (pgtbl_t)pmem_alloc(true);
        printf("VM BAD CASE %d: next operation must panic\n", VM_BAD_CASE);
        if (VM_BAD_CASE == 1)
            vm_mappages(root, 0, 0x80000000ul, PGSIZE, PTE_W);
        else if (VM_BAD_CASE == 2)
            vm_mappages(root, 1, 0x80000000ul, PGSIZE, PTE_R);
        else if (VM_BAD_CASE == 3)
            vm_mappages(root, VA_MAX, 0x80000000ul, PGSIZE, PTE_R);
        else if (VM_BAD_CASE == 4)
            vm_unmappages(root, 0, PGSIZE, false);
        else if (VM_BAD_CASE == 5)
            vm_getpte(NULL, 0, false);
        else
            vm_mappages(root, 0, 0x80000000ul, 0, PTE_R);
        panic("VM BAD: invalid operation accepted");
    }
    while (1) asm volatile("wfi");
}
