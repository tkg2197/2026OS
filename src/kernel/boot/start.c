#include "../arch/mod.h"

// 每个CPU在运行操作系统时需要一个初始的函数栈
__attribute__((aligned(16))) uint8 CPU_stack[4096 * NCPU];

extern void main();

void start(uint64 hartid)
{
    // OpenSBI已在S-mode进入内核。HSM启动hart时satp也为0。
    w_satp(0);
    sfence_vma();

    // OpenSBI通过a0传入hartid,也就是当前函数的输入参数hartid，我们可以直接使用他,内核继续用tp保存它。

    // 进入main函数
}
