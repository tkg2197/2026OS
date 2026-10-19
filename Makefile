# 引入通用配置文件
include common.mk

.DEFAULT_GOAL := build

# 自动探测本机可用的RISC-V工具链前缀
# common.mk中默认是riscv64-linux-gnu-, 但有些环境(例如macOS上的brew)装的是riscv64-unknown-elf-
# 探测不到任何工具链时保留common.mk中的默认值, 命令行传入的TOOLPREFIX优先级最高:
#   make TOOLPREFIX=riscv64-linux-gnu-
DETECTED_TOOLPREFIX := $(shell \
	for p in riscv64-linux-gnu- riscv64-unknown-elf- riscv64-elf- riscv64-none-elf-; do \
		if command -v $${p}gcc >/dev/null 2>&1; then echo $$p; break; fi; \
	done)
ifneq ($(DETECTED_TOOLPREFIX),)
TOOLPREFIX = $(DETECTED_TOOLPREFIX)
endif

# 配置CPU核心数量
CPUNUM = 2
# 定义目标文件输出目录
TARGET = target
# 定义各模块路径
KernelPath = src/kernel
UserPath = src/user
# 内核链接脚本
KERNEL_LD  = kernel.ld
# 用户程序链接脚本 (用户程序从USER_BASE = 0x1000开始执行)
USER_LD    = user.ld
# 定义内核目标文件路径
ELFKernel = $(TARGET)/kernel/kernel-qemu.elf
NakedKernel = $(TARGET)/kernel/kernel-qemu.bin
# 定义用户程序的目标文件路径
ELFUser = $(TARGET)/user/initcode.elf
NakedUser = $(TARGET)/user/initcode.bin
# initcode数组的头文件 (由proc.c直接#include)
InitcodeHeader = $(UserPath)/initcode.h

# 收集内核源代码文件（.c和.S汇编文件）
KernelSourceFile = $(wildcard $(KernelPath)/*.c) $(wildcard $(KernelPath)/*.S)
KernelSourceFile += $(wildcard $(KernelPath)/*/*.c) $(wildcard $(KernelPath)/*/*.S)
# 收集用户态源代码文件
UserSourceFile = $(wildcard $(UserPath)/*.c)

# 生成目标文件（.o）路径列表
KernelOBJ = $(patsubst $(KernelPath)/%.S, $(TARGET)/kernel/%.o, $(filter %.S, $(KernelSourceFile)))
KernelOBJ += $(patsubst $(KernelPath)/%.c, $(TARGET)/kernel/%.o, $(filter %.c, $(KernelSourceFile)))
UserOBJ = $(patsubst $(UserPath)/%.c, $(TARGET)/user/%.o, $(filter %.c, $(UserSourceFile)))

# QEMU模拟器配置
QEMU     = qemu-system-riscv64  # 指定QEMU程序
QEMUOPTS = -machine virt -bios default -kernel $(ELFKernel)  # 使用OpenSBI启动S-mode内核
QEMUOPTS += -m 130M -smp $(CPUNUM) -nographic  # 物理内存从0x80000000到0x88200000

# 调试相关配置
GDBPORT = $(shell expr `id -u` % 5000 + 25000)  # 动态计算GDB端口号
# 根据QEMU版本选择合适的GDB调试参数
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)

# 引入编译器生成的依赖文件(.d)
# CFLAGS中的-MD会为每个.o生成同名的.d文件, 里面记录了该目标文件依赖的所有头文件
# 不引入它们的话, 修改头文件(例如timer/type.h中的INTERVAL)不会触发重新编译
-include $(KernelOBJ:.o=.d) $(UserOBJ:.o=.d)

# 生成GDB初始化文件
.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

# 运行目标：先构建再启动QEMU
run: build
	$(QEMU) $(QEMUOPTS)

# 调试目标：启动带GDB调试的QEMU
debug: $(ELFKernel) .gdbinit
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

# 构建目标：创建输出目录并编译内核
build: $(TARGET) $(ELFKernel) 

# 创建输出目录结构（如果不存在）
.PHONY: $(TARGET)
$(TARGET):
	@mkdir -p $(TARGET)/kernel
	@mkdir -p $(TARGET)/kernel/arch
	@mkdir -p $(TARGET)/kernel/boot
	@mkdir -p $(TARGET)/kernel/lock
	@mkdir -p $(TARGET)/kernel/lib
	@mkdir -p $(TARGET)/kernel/mem
	@mkdir -p $(TARGET)/kernel/trap
	@mkdir -p $(TARGET)/kernel/proc
	@mkdir -p $(TARGET)/user

# 编译规则：将汇编文件(.S)编译为目标文件(.o)
$(TARGET)/kernel/%.o: $(KernelPath)/%.S
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# 编译规则：将C文件(.c)编译为目标文件(.o)
$(TARGET)/kernel/%.o: $(KernelPath)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# 编译用户态程序
$(TARGET)/user/%.o: $(UserPath)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# 链接生成用户程序ELF (main位于USER_BASE, 由user.ld保证)
$(ELFUser): $(UserOBJ) $(USER_LD)
	$(LD) $(LDFLAGS) -T $(USER_LD) $(UserOBJ) -o $@

# 转换成平坦二进制映像: 第一个字节就是USER_BASE处的指令
$(NakedUser): $(ELFUser)
	$(OBJCOPY) -O binary $< $@

# 转换成内核可以直接使用的数组
$(InitcodeHeader): $(NakedUser)
	xxd -i -n target_user_initcode $< > $@

# proc.c会#include initcode.h, 因此它必须等待initcode生成完毕
$(TARGET)/kernel/proc/proc.o: $(InitcodeHeader)

# 链接生成内核ELF文件
$(ELFKernel): $(KernelOBJ) $(KERNEL_LD)
	$(LD) $(LDFLAGS) -T $(KERNEL_LD) $(KernelOBJ) -o $@

# 清理目标：删除输出目录和生成的头文件
.PHONY: clean
clean:
	rm -rf target
	rm -f $(InitcodeHeader)

.PHONY: all build run qemu debug
all: build
qemu: run
