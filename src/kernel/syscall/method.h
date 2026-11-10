#pragma once

void syscall(void);

uint64 arg_raw(int n);
void arg_uint32(int n, uint32 *ip);
void arg_uint64(int n, uint64 *ip);
int arg_str(int n, char *buf, int maxlen);

uint64 sys_copyin();
uint64 sys_copyout();
uint64 sys_copyinstr();
uint64 sys_brk();
uint64 sys_mmap();
uint64 sys_munmap();

uint64 sys_helloworld(void);
uint64 sys_printf(void);
