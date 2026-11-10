#pragma once

void syscall(void);

uint64 arg_raw(int n);
void arg_uint32(int n, uint32 *ip);
void arg_uint64(int n, uint64 *ip);
int arg_str(int n, char *buf, int maxlen);

uint64 sys_brk(void);
uint64 sys_mmap(void);
uint64 sys_munmap(void);
uint64 sys_print_str(void);
uint64 sys_print_int(void);
uint64 sys_getpid(void);
uint64 sys_fork(void);
uint64 sys_wait(void);
uint64 sys_exit(void);
uint64 sys_sleep(void);
