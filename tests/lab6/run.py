"""Run the four README programs and extra Lab 6 cases in an isolated copy."""
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests/lab6"
ROOT_FAULT = r'''
#include "sys.h"
int main(void)
{
    int pid = syscall(SYS_fork);
    if (!pid) {
        syscall(SYS_sleep, 3);
        syscall(SYS_print_str, "CHILD SURVIVED ROOT FAULT\n");
        syscall(SYS_exit, 0);
    }
    volatile unsigned long bad = 0x900000;
    *(volatile char *)bad = 1;
    for (;;) ;
}
'''


def run(source, harts, program, seconds, expected):
    (source / "src/user/initcode.c").write_text(program)
    make = ["make", f"CPUNUM={harts}"]
    build = subprocess.run(make + ["-j4"], cwd=source, capture_output=True, text=True)
    if build.returncode:
        raise AssertionError(build.stdout + build.stderr)

    target = "qemu-1" if harts == 1 else "qemu"
    qemu = ["qemu-system-riscv64", "-machine", "virt", "-bios", "default",
            "-kernel", f"target/{target}/kernel/kernel-qemu.elf", "-m", "130M",
            "-smp", str(harts), "-nographic"]
    try:
        process = subprocess.run(qemu, cwd=source, stdin=subprocess.DEVNULL,
                                 capture_output=True, timeout=seconds)
        raise AssertionError(process.stdout.decode(errors="replace") +
                             process.stderr.decode(errors="replace"))
    except subprocess.TimeoutExpired as result:
        log = (result.stdout or b"").decode(errors="replace").replace("\r", "")

    if ("panic:" in log or "LAB6 FAIL" in log or
            any(log.count(marker) != count for marker, count in expected.items())):
        raise AssertionError(log)
    for hart in range(harts):
        assert log.count(f"cpu {hart} is booting with OpenSBI!") == 1, log


def main():
    readme = (ROOT / "README.md").read_text().split("## 测试用例", 1)[1]
    official = re.findall(r"\x60{3}c\n(.*?)\x60{3}", readme, re.S)
    assert len(official) == 4
    cases = [
        ("getpid", official[0], 2, {"proczero: hello world!": 1}),
        ("fork levels", official[1], 2, {"level-1!": 1, "level-2!": 2, "level-3!": 4}),
        ("copy / wait", official[2], 2,
         {"MMAP_REGION": 1, "HEAP_REGION": 1, "STACK_REGION": 1, "good boy!": 1}),
        ("sleep", official[3], 5, {"Ready to sleep!": 1, "Ready to exit!": 1, "Child exit!": 1}),
        ("stress", (TESTS / "stress.c").read_text(), 8, {"LAB6 PASS": 1}),
        ("root fault", ROOT_FAULT, 2, {"user fault pid=1": 1, "CHILD SURVIVED ROOT FAULT": 1}),
    ]

    with tempfile.TemporaryDirectory(prefix="os2026-lab6-") as temp:
        source = Path(temp)
        shutil.copytree(ROOT / "src", source / "src")
        for name in ("Makefile", "common.mk", "kernel.ld", "user.ld"):
            shutil.copy(ROOT / name, source)
        shutil.copy(TESTS / "probe.c", source / "src/kernel/lock/lab6_test.c")
        main_c = source / "src/kernel/main.c"
        main_c.write_text(main_c.read_text().replace(
            "        proc_init();",
            "        proc_init();\n        extern void lab6_test_init(void);\n        lab6_test_init();"))
        dispatcher = source / "src/kernel/syscall/syscall.c"
        dispatcher.write_text(dispatcher.read_text().replace(
            "static uint64 (*const", "extern uint64 lab6_test_probe(void);\n\nstatic uint64 (*const"
        ).replace("    [SYS_sleep] = sys_sleep,", "    [SYS_sleep] = sys_sleep,\n    [11] = lab6_test_probe,"))

        for harts in (1, 2):
            for name, program, seconds, expected in cases:
                run(source, harts, program, seconds, expected)
                print(f"PASS Lab6 {harts} hart(s) {name}", flush=True)


if __name__ == "__main__":
    main()
