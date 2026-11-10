# OpenSBI在S-mode进入该入口：a0=hartid，a1=DTB地址。
# 本实验不使用DTB，因此忽略a1。
#include "../arch/type.h"

.section .text.entry
.global _entry
_entry:
#ifdef VISIONFIVE2
        .option push
        .option norvc
        j .Lprimary
        nop
        .option pop
        .dword 0x200000
        .dword KERNEL_IMAGE_SIZE
        .dword 0
        .word 2
        .word 0
        .dword 0
        .dword 0x5643534952
        .word 0x05435352
        .word 0
#endif
.Lprimary:
        csrci sstatus, 2
        csrw sie, zero
        li t0, FIRST_HART
        bltu a0, t0, .Lpark
        li t0, NCPU
        bgeu a0, t0, .Lpark

        la t0, BSS_BEGIN
        la t1, BSS_END
.Lclear:
        bgeu t0, t1, .Lcleared
        sd zero, 0(t0)
        addi t0, t0, 8
        j .Lclear
.Lcleared:
        la t0, boot_hartid
        sd a0, 0(t0)
        j .Lstack

.global _entry_secondary
_entry_secondary:
        csrci sstatus, 2
        csrw sie, zero
        li t0, FIRST_HART
        bltu a0, t0, .Lpark
        li t0, NCPU
        bgeu a0, t0, .Lpark

.Lstack:
        # sp = CPU_stack + (hartid + 1) * 4096
        la t0, CPU_stack
        li t1, 4096
        addi t2, a0, 1
        mul t1, t1, t2
        add sp, t0, t1
        call start
.Lpark:
        wfi
        j .Lpark
