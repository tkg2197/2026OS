# OS2026 · Lab4：首个用户进程和用户态陷阱

本实验在 `lab3` 基础上加入用户进程和用户态切换框架。前序实验已完成的基础代码、测试和截图继续保留。

本分支已实现 Lab4 的首个用户进程、用户页表与用户态陷阱往返，函数契约见对应规格。

## Lab4 内容

| 文件 | 用途与当前状态 |
| --- | --- |
| `spec/patches/lab4.yaml` | 首进程、ecall 往返和异常处理的验收目标 |
| `src/kernel/proc/` | 进程结构、上下文切换、页表初始化与首进程创建 |
| `src/kernel/lib/cpu.c`、`type.h` | 当前 CPU 的进程指针和内核上下文 |
| `src/kernel/trap/trampoline.S` | 用户态与内核态之间的汇编切换框架 |
| `src/kernel/trap/trap_user.c` | 用户 trap 处理和返回用户态 |
| `src/user/`、`user.ld` | 用户程序、系统调用封装和用户链接脚本 |
| `Makefile`、`kernel.ld` | 用户程序嵌入内核以及 trampoline 段的构建支持 |

用户与内核地址空间参考图保存在本实验自己的目录中：

![Lab4 用户与内核地址空间](pictures/lab4/address-space.png)

## 构建与验收

```bash
git switch lab4
make clean
make build
make run
```

在 Lab3 所需工具之外，用户代码嵌入还需要 `xxd`。`src/user/initcode.h` 由构建生成，不提交到仓库。

验收应覆盖两次系统调用、用户执行期间的时钟/UART 中断，以及未知调用与用户异常，详见 [Lab4 验收规格](spec/patches/lab4.yaml)。
