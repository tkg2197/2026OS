.section .text
.global lab1_mtrap
lab1_mtrap:
    addi sp, sp, -32
    sd t0, 0(sp)
    sd t1, 8(sp)
    sd t2, 16(sp)
    csrr t0, mcause
    la t1, observed_cause
    slli t2, tp, 3
    add t1, t1, t2
    sd t0, 0(t1)
    csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    ld t0, 0(sp)
    ld t1, 8(sp)
    ld t2, 16(sp)
    addi sp, sp, 32
    mret
