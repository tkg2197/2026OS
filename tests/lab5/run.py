"""Exercise Lab 5 under QEMU and cross-build the same tests for VisionFive2."""
import argparse
import os
import re
import select
import shutil
import struct
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QEMU = ["qemu-system-riscv64", "-machine", "virt", "-bios", "default",
        "-kernel", "target/qemu/kernel/kernel-qemu.elf", "-m", "130M", "-smp", "2", "-nographic"]


def make(source, *args, expected=0):
    p = subprocess.run(["make", *args], cwd=source, capture_output=True, text=True)
    assert (p.returncode == 0) == (expected == 0), p.stdout + p.stderr
    return p.stdout + p.stderr


def run(source, markers, seconds=2):
    try:
        process = subprocess.run(QEMU, cwd=source, stdin=subprocess.DEVNULL,
                                 capture_output=True, timeout=seconds)
        raise AssertionError(process.stdout.decode(errors="replace") +
                             process.stderr.decode(errors="replace"))
    except subprocess.TimeoutExpired as result:
        log = (result.stdout or b"").decode(errors="replace").replace("\r", "")
    assert "panic:" not in log and "LAB5 FAIL" not in log, log
    for marker in markers:
        assert marker in log, log
    for hart in (0, 1):
        assert log.count(f"cpu {hart} is booting with OpenSBI!") == 1, log
    return log


def check_uart(source):
    with subprocess.Popen(QEMU, cwd=source, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as guest:
        log = bytearray()
        sent = False
        expected = b"A\x08 \x08\r\n\r\n"
        deadline = time.monotonic() + 5
        try:
            while time.monotonic() < deadline:
                if not select.select([guest.stdout], [], [], 0.2)[0]:
                    continue
                data = os.read(guest.stdout.fileno(), 4096)
                if not data:
                    break
                log.extend(data)
                if not sent and b"LAB5 PASS\n" in log:
                    guest.stdin.write(b"A\x7f\r\n")
                    guest.stdin.flush()
                    sent = True
                if expected in log:
                    return
            raise AssertionError(log.decode(errors="replace"))
        finally:
            guest.terminate()


def check_image(source):
    path = source / "target/vf2/kernel/kernel-vf2.bin"
    code0, code1, offset, size, flags, version, reserved1, reserved2, magic, magic2, reserved3 = (
        struct.unpack("<IIQQQIIQQII", path.read_bytes()[:64])
    )
    assert (code0, code1, offset, flags, version) == (0x0400006f, 0x13, 0x200000, 0, 2)
    assert (reserved1, reserved2, magic, magic2, reserved3) == (0, 0, 0x5643534952, 0x05435352, 0)
    assert 0 < path.stat().st_size <= size < (128 << 20) and size % 4096 == 0
    for platform, base in (("qemu", 0x80200000), ("vf2", 0x40200000)):
        elf = source / f"target/{platform}/kernel/kernel-{platform}.elf"
        text = subprocess.check_output(["riscv64-linux-gnu-nm", str(elf)], text=True)
        symbols = {name: int(address, 16) for address, name in
                   re.findall(r"(?m)^([0-9a-f]+) [a-zA-Z] (\w+)$", text)}
        assert symbols["_entry"] == base and symbols["ALLOC_END"] == base + (128 << 20)
        assert symbols["BSS_BEGIN"] % 8 == symbols["BSS_END"] % 8 == 0
        if platform == "vf2":
            assert symbols["KERNEL_IMAGE_SIZE"] == symbols["ALLOC_BEGIN"] - base == size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vf2-output", type=Path, help="save VF2 test images in this directory")
    args = parser.parse_args()

    def save_vf2(source, name):
        if args.vf2_output:
            args.vf2_output.mkdir(parents=True, exist_ok=True)
            image = source / "target/vf2/kernel/kernel-vf2.bin"
            shutil.copyfile(image, args.vf2_output / f"lab5-{name}.bin")

    user = ROOT / "src/user/initcode.c"
    readme = (ROOT / "README.md").read_text()
    extract = lambda part: re.findall(r"\x60{3}c\n(.*?)\x60{3}", part, re.S)
    copy = extract(readme.split("## 测试1：", 1)[1].split("## 任务2：", 1)[0])[0]
    brk, stack = extract(readme.split("## 测试2：", 1)[1].split("## 任务3:", 1)[0])
    mmap = extract(readme.split("## 测试4:", 1)[1].split("## 任务5:", 1)[0])[0]

    with tempfile.TemporaryDirectory(prefix="os2026-lab5-") as temp:
        source = Path(temp)
        shutil.copytree(ROOT / "src", source / "src")
        for name in ("Makefile", "common.mk", "kernel.ld", "user.ld"):
            shutil.copy(ROOT / name, source)

        initcode = source / "src/user/initcode.c"
        cases = [
            ("integrated", user.read_text(), ["LAB5 PASS", "hello, world", "stack:"]),
            ("copy", copy, ["1 2 3 4 5", "hello, world"]),
            ("brk", brk, ["0x000000000000b000", "0x0000000000006000"]),
            ("stack", stack, ["hello", "world", "stack: 1 ->"]),
            ("mmap", mmap, ["mmap:", "pgtbl:"]),
        ]
        for name, program, markers in cases:
            initcode.write_text(program)
            make(source, "-j4", "CPPFLAGS=-DLAB5_TRACE")
            run(source, markers)
            if name == "integrated":
                check_uart(source)
            make(source, "-j4", "PLATFORM=vf2", "CPPFLAGS=-DLAB5_TRACE")
            save_vf2(source, name)
            print(f"PASS Lab5 {name} (QEMU run, VF2 build)", flush=True)

        # Compile and link the same kernel probes for QEMU and the physical board.
        initcode.write_text(user.read_text())
        shutil.copy(ROOT / "tests/lab5/probe.c", source / "src/kernel/mem/lab5_test.c")
        main_c = source / "src/kernel/main.c"
        main_c.write_text(main_c.read_text().replace(
            "    timer_init();",
            "    timer_init();\n    extern void lab5_pool_test(uint64);\n    lab5_pool_test(hartid);"))
        proc = source / "src/kernel/proc/proc.c"
        proc.write_text(proc.read_text().replace(
            "    cpu->proc = p;",
            "    cpu->proc = p;\n    extern void lab5_vm_test(void);\n    lab5_vm_test();"))
        make(source, "-j4")
        log = run(source, ["POOL PASS", "VM PASS", "LAB5 PASS",
                           "printf probe: -2147483648 2a 0x000000000000002a -1.250000 %"], seconds=5)
        before, after = log.split("POOL INITIAL")
        after = after.split("POOL PASS")[0]
        assert re.findall(r"node (\d+) index = (\d+)", before) == [
            (str(i), str(i)) for i in range(256)
        ], log
        assert sorted(int(i) for i in re.findall(r"node \d+ index = (\d+)", after)) == list(range(256)), log
        make(source, "-j4", "PLATFORM=vf2")
        check_image(source)
        save_vf2(source, "kernel-probes")
        make(source, "-j4")
        assert "use U-Boot booti" in make(source, "run", "PLATFORM=vf2", expected=1)
        assert "use U-Boot booti" in make(source, "debug", "PLATFORM=vf2", expected=1)
        make(source, "PLATFORM=bad", expected=1)
        make(source, "CPUNUM=1", expected=1)
        print("PASS Lab5 node pool, VM transactions, copy ownership and platform builds")


if __name__ == "__main__":
    main()
