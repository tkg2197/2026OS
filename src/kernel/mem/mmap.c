#include "mod.h"

// mmap_region_node_t 仓库(单向链表) + 链表头节点(不可分配) + 保护仓库的自旋锁
static mmap_region_node_t node_list[N_MMAP];
static mmap_region_node_t list_head;
static spinlock_t list_lk;
static bool in_use[N_MMAP];

// 初始化上述三个数据结构
void mmap_init(void)
{
    spinlock_init(&list_lk, "mmap nodes");
    list_head.next = &node_list[0];
    for (uint32 i = 0; i + 1 < N_MMAP; i++)
        node_list[i].next = &node_list[i + 1];
}

// 从仓库申请一个 mmap_region_t
// 若仓库空了则返回 NULL
mmap_region_t *mmap_region_alloc(void)
{
    spinlock_acquire(&list_lk);
    mmap_region_node_t *node = list_head.next;
    if (node)
    {
        list_head.next = node->next;
        in_use[node - node_list] = true;
        node->mmap = (mmap_region_t){0};
    }
    spinlock_release(&list_lk);
    return node ? &node->mmap : NULL;
}

// 向仓库归还一个 mmap_region_t
void mmap_region_free(mmap_region_t *mmap)
{
    uint64 offset = (uint64)mmap - (uint64)node_list;
    assert(offset < sizeof(node_list) && offset % sizeof(node_list[0]) == 0,
           "mmap_region_free: invalid node");

    uint32 index = offset / sizeof(node_list[0]);
    spinlock_acquire(&list_lk);
    assert(in_use[index], "mmap_region_free: double free");
    in_use[index] = false;
    node_list[index].next = list_head.next;
    list_head.next = &node_list[index];
    spinlock_release(&list_lk);
}

// 输出可用的 mmap_region_node_t 链
// for debug
void mmap_show_nodelist()
{
    spinlock_acquire(&list_lk);

    mmap_region_node_t *tmp = list_head.next;
    int node = 0, index = 0;
    while (tmp)
    {
        index = tmp - &(node_list[0]);
        printf("node %d index = %d\n", node++, index);
        tmp = tmp->next;
    }

    spinlock_release(&list_lk);
}
