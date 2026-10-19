# OS2026 · Lab3：内核陷阱、时钟和外设中断

本分支在已完成的 `lab2` 上整理 Lab3 的现有规格与代码框架。Lab1、Lab2 的基础代码、测试和实验记录继续保留；Lab3 的核心空函数尚未完成，本次整理不代表实验验收通过。

## 已完成内容与分支关系

- [Lab1 报告](README.lab1.md)、[测试与记录](tests/lab1/)、[截图](pictures/lab1/)保留自 `lab1`。
- [Lab2 报告](README.lab2.md)、[测试与记录](tests/lab2/)、[截图](pictures/lab2/)保留自 `lab2`。
- 启动、UART、打印、自旋锁，以及物理页分配、buddy、内核堆和 Sv39 页表，使用 Lab2 已完成的实现。
- `lab1`、`lab2` 保留各自的独立实验版本；`lab3` 继承前序基础，`lab4` 再从 `lab3` 增加用户进程与用户态陷阱。
- 前序测试及其日志保留对应阶段的验证证据，运行到后续阶段时须结合接口与启动方式检查兼容性。

## Lab3 内容

| 文件 | 用途与当前状态 |
| --- | --- |
| `spec/patches/lab3.yaml` | 本实验目标和验收场景 |
| `spec/modules/trap.yaml` | 内核陷阱、时钟、PLIC 的目标契约 |
| `src/kernel/trap/trap.S` | 内核 trap 汇编入口 |
| `src/kernel/trap/trap_kernel.c` | 内核 trap、设备和时钟分派框架，仍有待补部分 |
| `src/kernel/trap/timer.c` | SBI 首次定时已有代码；共享时钟创建、更新和读取仍为空 |
| `src/kernel/trap/plic.c` | PLIC 初始化、claim 和 complete |
| `src/kernel/arch/sbi.c` | SBI TIME 与 HSM 调用 |

Lab4 的进程目录、用户程序、用户陷阱和 trampoline 源码仅在 `lab4` 分支中加入。

## 构建与验收

```bash
git switch lab3
make clean
make build
# 完成待补实现并成功构建后：
make run
```

需要 RISC-V GCC/binutils、GNU make 和 `qemu-system-riscv64`。2026-10-19 整理时已执行 `make build`：编译停在原有 `timer_get_ticks()` 缺少返回值、`system_timer` 未使用的错误。当前保留教学框架的空函数和 `-Wall -Werror`，须完成实现后再进行运行验收。

完成目标见 [Lab3 验收规格](spec/patches/lab3.yaml)：时钟 ticks 持续推进，修改 `INTERVAL` 后频率相应变化，UART 正常回显并处理退格与换行。
