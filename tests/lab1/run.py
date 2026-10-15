#!/usr/bin/env python3
"""复制Lab1到临时目录，运行真实双核QEMU测试；不替换原工程测试入口。"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import selectors
import shutil
import subprocess
import tempfile
import time

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=here.parents[1])
parser.add_argument('--output', type=Path, default=here / 'results')
parser.add_argument('--only', help='用例名子串；建议同时指定独立--output目录')
args = parser.parse_args()
source, output = args.source.resolve(), args.output.resolve()
output.mkdir(parents=True, exist_ok=True)

def hashes():
    paths = sorted(p for p in (source / 'src').rglob('*') if p.suffix in ('.c', '.h', '.S'))
    paths += [source / name for name in ('Makefile', 'common.mk', 'kernel.ld')]
    return {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}

before = hashes()
workspace = Path(tempfile.mkdtemp(prefix='os2026-lab1-test-'))
shutil.copytree(source / 'src', workspace / 'src')
for name in ('Makefile', 'common.mk', 'kernel.ld'):
    shutil.copyfile(source / name, workspace / name)
main = workspace / 'src/kernel/main.c'
start = workspace / 'src/kernel/boot/start.c'
printer = workspace / 'src/kernel/lib/print.c'
original_main, original_start, original_print = main.read_text(), start.read_text(), printer.read_text()
env = {
    'date': time.strftime('%Y-%m-%d %H:%M:%S %z'), 'source':str(source), 'workspace':str(workspace),
    'head': subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip(),
    'qemu':subprocess.check_output(['qemu-system-riscv64','--version'],text=True).splitlines()[0],
    'gcc':subprocess.check_output(['riscv64-linux-gnu-gcc','--version'],text=True).splitlines()[0],
    'qemu_options':'-machine virt -bios none -m 128M -smp 2 -accel tcg,thread=multi -nographic',
    'source_sha256':before,
    'fixture_changes':'Only temporary main; boot diagnostics install temporary M-mode ecall handler; unlocked printf removes only print_lk acquire/release.',
}
cases = [
    ('original-atomic-entry',0,'cpu 1 report: sum = 2000000',False),
    ('boot-mode-stack',1,'BOOT MODE/STACK PASS',False),
    ('add-unlocked',2,'UNLOCKED ADD DEMO COMPLETE',False),
    ('add-fine-lock',3,'LOCKED ADD PASS: mode=fine',False),
    ('add-coarse-lock',4,'LOCKED ADD PASS: mode=coarse',False),
    ('printf-format',5,'PRINTF FORMAT PASS',False),
    ('printf-locked',6,'PRINTF RECORDS DONE',False),
    ('printf-unlocked',7,'PRINTF RECORDS DONE',False),
    ('nested-interrupt',8,'NESTED INTERRUPT/LOCK OWNERSHIP PASS',False),
    ('uart-input',9,'UART INPUT PASS',False),
    ('bad-recursive-lock',10,'spinlock_acquire: already holding',True),
    ('bad-unheld-release',11,'spinlock_release: not holding',True),
    ('bad-cross-hart-release',12,'spinlock_release: not holding',True),
    ('bad-pop-underflow',13,'push_off: 2',True),
    ('bad-pop-enabled',14,'push_off: 1',True),
    ('bad-assert',15,'lab1 expected assertion',True),
]

def boot(name, marker, panic_expected):
    command = ['qemu-system-riscv64','-machine','virt','-bios','none','-kernel',
               str(workspace / 'target/kernel/kernel-qemu.elf'),'-m','128M','-smp','2',
               '-accel','tcg,thread=multi','-nographic']
    proc = subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout,selectors.EVENT_READ)
    data = bytearray()
    deadline = time.monotonic() + 60
    sent_input = False
    try:
        while time.monotonic() < deadline:
            for key,_ in selector.select(0.05):
                chunk = os.read(key.fileobj.fileno(),65536)
                if chunk: data.extend(chunk)
            if name == 'uart-input' and b'UART INPUT READY\n' in data and not sent_input:
                proc.stdin.write(b'Lab1!\n')
                proc.stdin.flush()
                sent_input = True
            if name == 'original-atomic-entry':
                done = all(f'cpu {h} report: sum = 2000000\n'.encode() in data for h in (0,1))
            else:
                done = marker.encode() in data and b'\n' in data.split(marker.encode(),1)[1]
            panic_done = b'panic!' in data and b'\n' in data.split(b'panic!',1)[1]
            if done or panic_done or proc.poll() is not None: break
    finally:
        if proc.poll() is None: proc.terminate()
        try: data.extend(proc.communicate(timeout=3)[0])
        except subprocess.TimeoutExpired:
            proc.kill(); data.extend(proc.communicate()[0])
        selector.close()
    raw = bytes(data)
    (output / f'{name}.log').write_bytes(raw)
    assert marker.encode() in raw, f'{name}: missing completion marker; inspect raw log'
    assert (b'panic!' in raw) == panic_expected, f'{name}: wrong panic state'
    details = {}
    if name == 'original-atomic-entry':
        assert all(f'cpu {h} is booting!'.encode() in raw for h in (0,1))
        assert all(f'cpu {h} report: sum = 2000000'.encode() in raw for h in (0,1))
    if name == 'add-unlocked':
        values = [int(x) for x in re.findall(rb'final=(\d+) expected=2000000',raw)]
        assert len(values) == 5 and any(x != 2000000 for x in values)
        details['final_counts'] = values
    if name == 'printf-format':
        for expected in [b'FMT signed: 0 1 -1 2147483647 -2147483648',
                         b'FMT p32: 0 80000000 ffffffff',
                         b'FMT x64: 0x123456789abcdef0 0xffffffffffffffff',
                         b'FMT text: Z hello (null) %',b'FMT unknown: %q',b'FMT trailing: %\n']:
            assert expected in raw, f'{name}: expected output missing: {expected!r}'
    if name.startswith('printf-') and name != 'printf-format':
        lines = [x for x in raw.splitlines() if x and b'PRINTF RECORDS DONE' not in x and not x.startswith(b'qemu-system')]
        valid = []
        for line in lines:
            m = re.fullmatch(rb'ROW hart=([01]) seq=(\d+) payload=([AB]{64}) END',line)
            if m and m[3] == (b'A' if m[1] == b'0' else b'B') * 64:
                valid.append((int(m[1]),int(m[2])))
        broken = [line.decode(errors='replace') for line in lines
                  if not re.fullmatch(rb'ROW hart=[01] seq=\d+ payload=[AB]{64} END',line)]
        if name == 'printf-locked':
            assert len(lines) == 64 and len(set(valid)) == 64
            assert set(valid) == {(h,i) for h in (0,1) for i in range(32)}
        else:
            assert len(valid) < 64 or broken, 'unlocked printf: scheduling did not expose interleaving; rerun'
        details.update(total_lines=len(lines),valid_records=len(valid),broken_lines=len(broken))
    return details

rows = []
try:
    for name,case,marker,panic_expected in cases:
        if args.only and args.only not in name: continue
        started = time.monotonic()
        start.write_text(original_start)
        printer.write_text(original_print)
        if case == 0:
            main.write_text(original_main)
        else:
            main.write_text(f'#define LAB1_CASE {case}\n' + (here / 'cases.c').read_text())
            shutil.copyfile(here / 'mtrap.S',workspace / 'src/kernel/boot/lab1_test_mtrap.S')
            if case == 1:
                start.write_text('extern void lab1_prepare_trap(void);\n' + original_start.replace(
                    'asm volatile("mret");','lab1_prepare_trap();\n    asm volatile("mret");'))
            if case == 7:
                for call in ('spinlock_acquire(&print_lk);','spinlock_release(&print_lk);'):
                    assert original_print.count(call) == 1
                printer.write_text(original_print.replace('spinlock_acquire(&print_lk);','/* test only: print lock omitted */')
                                   .replace('spinlock_release(&print_lk);','/* test only: print unlock omitted */'))
        build = subprocess.run(['make','-B','build'],cwd=workspace,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        (output / f'{name}.build.log').write_bytes(build.stdout)
        assert build.returncode == 0 and b'-Wall -Werror' in build.stdout, f'{name}: build failed'
        details = boot(name,marker,panic_expected)
        row = dict(name=name,status='PASS',expected=marker,expected_panic=panic_expected,
                   seconds=round(time.monotonic()-started,3),**details)
        rows.append(row)
        print(f'PASS {name} ({row["seconds"]}s) {details}',flush=True)
finally:
    env['source_unchanged'] = before == hashes()
    (output / 'environment.json').write_text(json.dumps(env,ensure_ascii=False,indent=2))
    (output / 'results.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2))
    assert env['source_unchanged'], 'original source changed during test'
print(f'ALL {len(rows)} CASES PASSED; original source unchanged',flush=True)
