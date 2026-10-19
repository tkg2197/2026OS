# OS2026 · Lab3：内核陷阱、时钟和外设中断

本实验在已完成的 `lab2` 基础上加入内核 trap、时钟和外设中断。前序实验已完成的基础代码、测试和截图继续保留。

当前分支保存 Lab3 的现有规格与代码框架，核心空函数尚未完成，仍需按验收规格实现。

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
