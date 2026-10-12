#pragma once

/* pmem.c: 物理内存管理逻辑 */

void pmem_init(void);
void *pmem_alloc(bool in_kernel);
void pmem_free(uint64 page, bool in_kernel);

/* kvm.c: 内核态虚拟内存管理 + 页表通用函数 */

pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc);
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm);
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit);
void vm_print(pgtbl_t pgtbl);
void kvm_init();
void kvm_inithart();

/* buddy system */

void *pmem_alloc_order(unsigned order, bool in_kernel);
void pmem_free_order(uint64 pa, unsigned order, bool in_kernel);
uint32 pmem_freepages(bool in_kernel);

/* heap */

void kheap_init(void);
void *kmalloc(uint64 size);
void *kmalloc_aligned(uint64 size, uint64 align);
void kfree(void *ptr);
uint32 kheap_pages(void);
void kheap_print(void);
void *pmem_try_alloc(bool in_kernel);

/* huge page */
pte_t *vm_lookup(pgtbl_t pgtbl, uint64 va, int *level_out);

void vm_mappages_huge(pgtbl_t pgtbl, uint64 va, uint64 pa,
                      uint64 len, int perm);

uint32 vm_count_pgtbl(pgtbl_t pgtbl);

void vm_freewalk(pgtbl_t pgtbl);
