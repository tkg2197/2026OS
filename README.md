# OS2026 · Lab3：内核陷阱、时钟与UART中断

本分支已在Lab2基础上完成Lab3全部任务。运行环境为QEMU virt、默认OpenSBI、130MiB内存、2个hart；保留`-Wall -Werror`，支持干净的并行构建。完成日期：2026-10-19（Asia/Shanghai）。

- [详细教程：逐文件、逐函数与完整代码](docs/lab3/tutorial.md)（112个函数，含静态辅助函数和内联CSR访问器）
- [课程原版实验要求](docs/lab3/requirements.md)
- [本实验验收规格](spec/patches/lab3.yaml)
- [自动验收脚本](tests/lab3/run.py)与[结果和源码哈希](tests/lab3/results/summary.json)

## 实现与要求对应

|要求|实现|
|---|---|
|监督者trap入口和现场恢复|trap.S保存内核通用寄存器；trap_kernel_handler检查来源、保存并恢复sepc/sstatus|
|时钟中断|每hart通过SBI TIME首次预约并在每次中断后重新预约；只由hart0更新加锁共享ticks|
|PLIC/UART外部中断|设置SEIE，按当前hart claim，分派UART，非零IRQ恰好complete一次；未知IRQ诊断后继续|
|回显、退格、换行|普通字符原样；BS/DEL输出08 20 08；CR和LF分别输出0d 0a；循环排空接收FIFO|
|初始化顺序|共享资源先初始化，本地stvec与PLIC就绪后开中断，UART接收IER最后开启|
|补充测试和错误检查|覆盖双核、3档间隔、连续输入、无IRQ/未知IRQ/complete规则、非法指令诊断|

额外修复了实际运行发现的两个必要问题：并行make时输出目录的创建竞争；OpenSBI随机先交接hart1时原入口会永久等待。现在start区分固件非零a1和HSM的0，必要时先启动hart0，仍由hart0执行共享初始化。panic改为关闭本hart中断后直接写UART，避免持printf锁时诊断死锁。继承的%p是完整64位指针，已纠正规格中不一致的描述。

## 构建与运行

```bash
git switch lab3
make clean
make -j4 build
make run
# 自动验收（只在临时副本注入观测代码）
python3 tests/lab3/run.py
```

依赖：GNU make、RISC-V GCC/binutils、qemu-system-riscv64、Python3。QEMU退出方式：Ctrl-a后按x。默认运行持续等待中断，不自行退出；测试脚本在观察窗口后主动结束QEMU。

## 实测结果

双核启动及继承的buddy单核/双核测试通过。普通字符、BS、DEL、CR、LF与连续78字符按预期字节回显；测试无panic。时钟观测如下，timebase由固件日志确认是10MHz：

|INTERVAL|hart0/hart1观测事件数|hart0间隔中位数（time计数）|
|---:|---:|---:|
|500000|40/40|502966|
|1000000|20/20|1001748|
|2000000|10/10|2003348|

每个hart0事件的ticks恰好增加1；interval越大频率越慢。PLIC受控替身验证无IRQ不complete、未知非零IRQ完成一次；真实UART路径由双核QEMU覆盖。故意执行非法指令时，输出Illegal instruction、完整scause/sepc/stval并进入panic停驻，属于该负向测试的预期结果。

原始日志见[tests/lab3/results](tests/lab3/results/)。正式源码在测试前后SHA-256一致。Lab1、Lab2测试和图片保留；进程、用户trap与trampoline属于lab4分支。
