#pragma once

// proc.c: 进程管理相关

void proc_init(void);          // 进程模块初始化
proc_t *proc_alloc(void);      // 进程申请
void proc_free(proc_t *p); // 进程释放

pgtbl_t proc_pgtbl_init(uint64 trapframe);          // 页表初始化
void proc_make_first(void);                             // 创建第一个用户进程
int proc_fork(void);                                    // 复制子进程
int proc_wait(uint64 addr);                         // 等待子进程退出
void proc_exit(int exit_state) __attribute__((noreturn));                     // 进程退出
void proc_yield(void);                                  // 进程放弃CPU
void proc_sleep(void *sleep_space, spinlock_t *lk); // 进程睡眠
void proc_wakeup(void *sleep_space);                // 进程唤醒
void proc_sched(void);                                  // 进程切换到调度器
void proc_scheduler(void) __attribute__((noreturn));
void proc_return(void) __attribute__((noreturn));                              // 调度器选择合适的进程执行
