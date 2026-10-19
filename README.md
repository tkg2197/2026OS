# OS2026 · Lab4：首个用户进程与用户态陷阱

本分支合入已完成的Lab3，实现首个用户进程、用户页表、trampoline和ecall往返，并填完课程要求的process/usertrap函数契约。完成日期：2026-10-19（Asia/Shanghai）。

- [详细教程：逐文件、逐函数、完整代码与调用图](docs/lab4/tutorial.md)（126个源函数，含继承基础和内联函数）
- [课程原版实验要求](docs/lab4/requirements.md)
- [进程函数契约](spec/modules/process.yaml)、[用户trap函数契约](spec/modules/usertrap.yaml)
- [验收脚本](tests/lab4/run.py)、[结果和源码哈希](tests/lab4/results/summary.json)
- [Lab3教程](docs/lab3/tutorial.md)；Lab3历史验收脚本请在lab3分支运行

## 实验要求对应

|要求|实现位置与行为|
|---|---|
|补全实验设计|process.yaml中的proc_pgtbl_init/proc_make_first，以及usertrap.yaml中的trap_user_handler/trap_user_return均填写pre/effects/post|
|首进程资源与地址空间|proc.c创建内核池tf/页表和用户池代码/栈；零页不映射，代码RXU、栈RWU，高地址辅助页无U|
|内核新增映射|kvm.c映射共享TRAMPOLINE RX、KSTACK(0) RW；内核中TRAPFRAME位置保留为保护页|
|同级上下文切换|填写proc.ctx.ra/sp，经swtch从CPU早期执行流进入trap_user_return|
|跨特权级切换|trampoline.S保存/恢复整数现场并切satp；C设置高地址stvec、sepc和SPP/SPIE|
|系统调用|a7选服务，tf.a0写结果；hello返回0，未知号返回-1；ecall后epc加4|
|用户中断与错误|时钟/UART复用Lab3 handler且不推进用户PC；用户错误输出诊断并停止故障执行，内核继续响应设备|
|构造失败回滚|pmem_try_alloc与vm_try_mappage可失败；撤销映射并回收全部新页，成功前不发布cpu.proc|
|构建与入口|用户main显式.text.entry；user.ld断言0x1000入口及一页上限；bin经xxd嵌入内核|

继承的Lab2已经使用buddy和混合大页，因此保留它的接口行为，另加可恢复的单页映射API供用户进程构造。memory.yaml已按实际接口说明这一区别，避免原简单链表模板和现有实现冲突。C与汇编的context/trapframe关键偏移由_Static_assert检查。当前trap仅保存整数上下文，统一使用RV64IMAC+Zicsr+Zifencei/LP64编译。

## 构建与运行

```bash
git switch lab4
make clean
make -j4 build
make run
# 全部验收，场景代码仅写入临时源码副本
python3 tests/lab4/run.py
```

依赖：RISC-V GCC/binutils、GNU make、qemu-system-riscv64、xxd、Python3。默认QEMU virt、130MiB、2个hart与OpenSBI。`make qemu`是`make run`的别名。退出QEMU按Ctrl-a，再按x。

默认输出中恰好出现两次：

```text
proczero: hello world!
proczero: hello world!
```

随后保持用户循环，可继续输入字符、退格、换行；不重复执行ecall。

## 验收结果：21组全部通过

|场景|结果|
|---|---|
|默认两次调用、双核启动、UART与布局|通过，验证RXU/RWU、零页、受保护别名和内核栈保护页|
|用户源码及INTERVAL头增量依赖|通过，用户bin/header/proc.o/内核及对应timer目标重新生成|
|未知调用与已知调用返回值|未知号-1，连续两次hello均0|
|全部可写整数寄存器及用户栈恢复|通过；观测到36次用户态时钟事件，UART同步输入正常|
|八种用户错误|零页读/写、写代码页、读trapframe/trampoline/内核、栈执行、非法指令均出现预期cause；各只报告一次，之后UART仍可回显|
|八个分配失败点|每处都不发布进程，内核池和用户池容量均精确恢复|
|镜像超过一页|链接器按实验约束拒绝|

原始编译和QEMU输出见[tests/lab4/results](tests/lab4/results/)。所有测试前后正式源码哈希一致；规格YAML均可解析，四个待补函数契约没有null占位。最终干净并行构建通过，用户ELF入口为0x1000，无RWX载入段。

本阶段只有一个用户进程，没有scheduler/exit；用户故障后停止执行但保留其资源，供后续实验扩展生命周期。创建失败则已经完整回滚。当前仅支持整数用户现场，不支持浮点/向量状态保存。
