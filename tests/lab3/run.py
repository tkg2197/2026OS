#!/usr/bin/env python3
"""Lab3 QEMU acceptance. Run inside WSL: python3 tests/lab3/run.py."""
from pathlib import Path
import hashlib
import json
import os
import re
import selectors
import shutil
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'tests/lab3/results'


def build(work, name):
    result = subprocess.run(['make', '-j4', 'build'], cwd=work,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / (name + '-build.log')).write_bytes(result.stdout)
    assert result.returncode == 0, result.stdout.decode(errors='replace')


def run(work, name, seconds=2.6, send=False):
    command = ['qemu-system-riscv64', '-machine', 'virt', '-bios', 'default',
               '-kernel', 'target/kernel/kernel-qemu.elf', '-m', '130M', '-smp', '2', '-nographic']
    p = subprocess.Popen(command, cwd=work, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    data = bytearray()
    sel = selectors.DefaultSelector()
    sel.register(p.stdout, selectors.EVENT_READ)
    sent = False
    start = time.monotonic()
    payload = b'abc\x08D\x7fE\rF\n' + bytes(range(65, 91)) * 3 + b'\r'
    try:
        while time.monotonic() - start < seconds:
            for key, _ in sel.select(.03):
                chunk = os.read(key.fileobj.fileno(), 65536)
                if chunk:
                    data.extend(chunk)
            if send and not sent and b'buddy two-core test passed' in data:
                # 等待两核安装stvec并启用UART接收，再逐字节发送防止虚拟FIFO溢出。
                time.sleep(.15)
                for c in payload:
                    p.stdin.write(bytes([c]))
                    p.stdin.flush()
                    time.sleep(.003)
                sent = True
    finally:
        p.terminate()
        rest, _ = p.communicate(timeout=5)
        data.extend(rest)
        sel.close()
    (OUT / (name + '-qemu.log')).write_bytes(data)
    return bytes(data)


def change(work, name, old, new):
    p = work / name
    s = p.read_text()
    assert old in s, (name, old)
    p.write_text(s.replace(old, new))


def copy_source(parent, name):
    work = parent / name
    shutil.copytree(ROOT, work, ignore=shutil.ignore_patterns('.git', 'target', 'tests', 'docs', 'pictures'))
    return work


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    before = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (ROOT / 'src').rglob('*') if p.is_file()}
    results = {}
    with tempfile.TemporaryDirectory(prefix='os-lab3-tests-') as temp:
        parent = Path(temp)
        work = copy_source(parent, 'baseline')
        change(work, 'src/kernel/main.c', '        uart_enable_rx_interrupt();',
               '    { uart_enable_rx_interrupt(); assert(ReadReg(IER) == 1, "UART RX IER"); }')
        build(work, 'baseline')
        data = run(work, 'baseline', send=True)
        assert b'panic!' not in data
        for hart in (0, 1):
            assert data.count(f'cpu {hart} is booting with OpenSBI!'.encode()) == 1
        assert b'abc\x08 \x08D\x08 \x08E\r\nF\r\n' in data
        assert bytes(range(65, 91)) * 3 + b'\r\n' in data
        results['baseline_echo_dual_hart'] = 'PASS'
        periods = {}
        for interval in (500000, 1000000, 2000000):
            name = f'timer-{interval}'
            work = copy_source(parent, name)
            change(work, 'src/kernel/trap/type.h', '#define INTERVAL 1000000', f'#define INTERVAL {interval}')
            change(work, 'src/kernel/trap/trap_kernel.c', '        timer_update();',
                   '        timer_update();\n    printf("[timer] hart=%d tick=%x time=%x\\n", mycpuid(), timer_get_ticks(), r_time());')
            build(work, name)
            data = run(work, name, seconds=2.2)
            assert b'panic!' not in data
            samples = re.findall(rb'\[timer\] hart=(\d) tick=0x([0-9a-f]+) time=0x([0-9a-f]+)', data)
            h0 = [(int(t, 16), int(tm, 16)) for h, t, tm in samples if h == b'0']
            h1 = [(int(t, 16), int(tm, 16)) for h, t, tm in samples if h == b'1']
            assert len(h0) >= 7 and len(h1) >= 7, samples
            assert all(b[0] == a[0] + 1 for a, b in zip(h0, h0[1:])), h0
            period = statistics.median(b[1] - a[1] for a, b in zip(h0, h0[1:]))
            assert interval <= period < interval * 1.6, period
            periods[interval] = period
            results[name] = {'status': 'PASS', 'hart0_events': len(h0), 'hart1_events': len(h1), 'median_time_counts': period}
        assert periods[500000] < periods[1000000] < periods[2000000]

        # 用受控PLIC替身覆盖无IRQ、未知IRQ和一次完成规则；真实UART由baseline覆盖。
        work = copy_source(parent, 'irq-contract')
        (work / 'src/kernel/trap/plic.c').write_text('''#include "mod.h"
static int step, completed;
void plic_init(void) {}
void plic_inithart(void) {}
int plic_claim(void) { return step++ == 0 ? 0 : 7; }
void plic_complete(int irq) { assert(irq == 7 && completed++ == 0, "bad complete"); }
''')
        change(work, 'src/kernel/main.c', '        trap_kernel_init();',
               '        trap_kernel_init();\n        external_interrupt_handler();\n        external_interrupt_handler();\n        printf("IRQ contract passed\\n");')
        build(work, 'irq-contract')
        data = run(work, 'irq-contract', 1)
        assert b'IRQ contract passed' in data and b'unexpected external irq: 7' in data and b'panic!' not in data
        results['irq_contract'] = 'PASS'

        work = copy_source(parent, 'illegal-instruction')
        change(work, 'src/kernel/main.c', '    if (hartid == 0)\n        uart_enable_rx_interrupt();',
               '    if (hartid == 0) { uart_enable_rx_interrupt(); asm volatile(".word 0"); }')
        build(work, 'illegal-instruction')
        data = run(work, 'illegal-instruction', 2)
        assert b'Illegal instruction' in data and b'panic! trap_kernel_handler' in data
        assert re.search(rb'scause=0x0000000000000002 sepc=0x[0-9a-f]{16} stval=', data)
        results['kernel_fault_diagnostic'] = 'PASS'
    after = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in (ROOT / 'src').rglob('*') if p.is_file()}
    assert before == after, 'tests modified production source'
    results['production_source_unchanged'] = True
    results['source_sha256'] = before
    (OUT / 'summary.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({k: v for k, v in results.items() if k != 'source_sha256'}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
