#pragma once

#ifndef __ASSEMBLER__
/* 基本类型定义 */

typedef char int8;
typedef short int16;
typedef int int32;
typedef long long int64;
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long long uint64;
typedef unsigned long long reg;

typedef struct sbi_ret
{
    int64 error;
    int64 value;
} sbi_ret_t;

typedef enum
{
    false = 0,
    true = 1
} bool;

#ifndef NULL
#define NULL ((void *)0)
#endif

extern uint64 boot_hartid;
#endif
/* OS 全局变量 */

#ifdef VISIONFIVE2
#define NCPU 5
#define FIRST_HART 1
#else
#define NCPU 2
#define FIRST_HART 0
#endif

/* SBI Hart State Management extension */
#define SBI_SUCCESS 0
#define SBI_EXT_HSM 0x48534D
#define SBI_HSM_HART_START 0
#define SBI_HSM_HART_STOP 1

/* SBI Timer extension */
#define SBI_EXT_TIME 0x54494D45
#define SBI_TIME_SET_TIMER 0

/* SBI RFENCE extension */
#define SBI_EXT_RFENCE 0x52464E43
#define SBI_RFENCE_SFENCE_VMA 1

/* SBI System Reset extension */
#define SBI_EXT_SRST 0x53525354
#define SBI_SRST_RESET 0


/* RISC-V 架构常量与宏定义 */

/* Supervisor Status Register (sstatus) */
#define SSTATUS_SPP (1L << 8)
#define SSTATUS_SPIE (1L << 5)
#define SSTATUS_UPIE (1L << 4)
#define SSTATUS_SIE (1L << 1)
#define SSTATUS_UIE (1L << 0)

/* Supervisor Interrupt Enable (sie) */
#define SIE_SEIE (1L << 9) /* S-mode 外设中断 */
#define SIE_STIE (1L << 5) /* S-mode 时钟中断 */
#define SIE_SSIE (1L << 1) /* S-mode 软件中断 */
