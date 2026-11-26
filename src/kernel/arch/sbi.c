#include "mod.h"
// 通过opensbi与底层硬件交互
sbi_ret_t sbi_ecall(uint64 eid, uint64 fid,
                    uint64 arg0, uint64 arg1, uint64 arg2,
                    uint64 arg3, uint64 arg4, uint64 arg5)
{
  register uint64 a0 asm("a0") = arg0;
  register uint64 a1 asm("a1") = arg1;
  register uint64 a2 asm("a2") = arg2;
  register uint64 a3 asm("a3") = arg3;
  register uint64 a4 asm("a4") = arg4;
  register uint64 a5 asm("a5") = arg5;
  register uint64 a6 asm("a6") = fid;
  register uint64 a7 asm("a7") = eid;

  asm volatile("ecall"
               : "+r"(a0), "+r"(a1)
               : "r"(a2), "r"(a3), "r"(a4), "r"(a5),
                 "r"(a6), "r"(a7)
               : "memory");

  return (sbi_ret_t){(int64)a0, (int64)a1};
}

sbi_ret_t sbi_hart_start(uint64 hartid, uint64 start_addr, uint64 opaque)
{
  return sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_START,
                   hartid, start_addr, opaque, 0, 0, 0);
}

sbi_ret_t sbi_hart_stop(void)
{
  return sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_STOP,
                   0, 0, 0, 0, 0, 0);
}

// 设置当前hart的下一次时钟事件
// 注意stime_value是mtime的目标值(绝对时间), 而不是时间间隔
sbi_ret_t sbi_set_timer(uint64 stime_value)
{
  return sbi_ecall(SBI_EXT_TIME, SBI_TIME_SET_TIMER,
                   stime_value, 0, 0, 0, 0, 0);
}

// 请求其他hart执行sfence.vma; start_addr和size均为0表示整个地址空间
sbi_ret_t sbi_remote_sfence_vma(uint64 hart_mask, uint64 hart_mask_base, uint64 start_addr, uint64 size)
{
  return sbi_ecall(SBI_EXT_RFENCE, SBI_RFENCE_SFENCE_VMA,
                   hart_mask, hart_mask_base, start_addr, size, 0, 0);
}

// 重启或关闭整个系统
sbi_ret_t sbi_system_reset(uint64 reset_type, uint64 reset_reason)
{
  return sbi_ecall(SBI_EXT_SRST, SBI_SRST_RESET,
                   reset_type, reset_reason, 0, 0, 0, 0);
}
