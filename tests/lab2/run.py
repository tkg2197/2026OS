#!/usr/bin/env python3
"""在临时副本运行Lab2用例；不替换原工程源码。WSL/Linux: python3 tests/lab2/run.py"""
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

HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=HERE.parents[1])
parser.add_argument('--output', type=Path, default=HERE / 'results')
parser.add_argument('--only', help='按用例名子串筛选；不改变默认完整测试集')
args = parser.parse_args()
source, output = args.source.resolve(), args.output.resolve()
output.mkdir(parents=True, exist_ok=True)

def hashes():
    paths = [p for p in (source / 'src').rglob('*') if p.suffix in ('.c', '.h', '.S')]
    paths += [source / x for x in ('Makefile', 'common.mk', 'kernel.ld', 'user.ld') if (source / x).exists()]
    return {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}

before = hashes()
workspace = Path(tempfile.mkdtemp(prefix='os2026-lab2-readme-'))
shutil.copytree(source / 'src', workspace / 'src', ignore=shutil.ignore_patterns('*.swp', '*.swo'))
for name in ('Makefile', 'common.mk', 'kernel.ld', 'user.ld'):
    if (source / name).exists(): shutil.copyfile(source / name, workspace / name)
main = workspace / 'src/kernel/main.c'
original_main = main.read_text()
environment = {
    'date': time.strftime('%Y-%m-%d %H:%M:%S %z'),
    'source': str(source), 'workspace': str(workspace),
    'branch': subprocess.check_output(['git', 'branch', '--show-current'], cwd=source, text=True).strip(),
    'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip(),
    'qemu': subprocess.check_output(['qemu-system-riscv64', '--version'], text=True).splitlines()[0],
    'gcc': subprocess.check_output(['riscv64-linux-gnu-gcc', '--version'], text=True).splitlines()[0],
    'qemu_options': '-machine virt -bios default -m 130M -smp 2 -accel tcg,thread=multi -nographic',
    'source_sha256': before,
}
# 原工程另含尚未实现的后续实验proc/timer；完整构建单独记录，不冒充通过。
full_build = subprocess.run(['make', '-B', 'build'], cwd=workspace,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(output / 'full-project.build.log').write_bytes(full_build.stdout)
environment['full_project_build_exit'] = full_build.returncode
print(f'Full project build exit={full_build.returncode}; see full-project.build.log', flush=True)
lab2_sources = [workspace / 'src/kernel/main.c']
# 纯Lab2仓库没有后续实验的trampoline；仅在当前源码包含它时构建。
trampoline = workspace / 'src/kernel/trap/trampoline.S'
if trampoline.exists():
    lab2_sources.append(trampoline)
for directory in ('arch', 'boot', 'lib', 'lock', 'mem'):
    lab2_sources += sorted(p for p in (workspace / 'src/kernel' / directory).iterdir()
                           if p.suffix in ('.c', '.S'))
lab2_objects = ['target/kernel/' + str(p.relative_to(workspace / 'src/kernel').with_suffix('.o'))
                for p in lab2_sources]
build_args = ['make', 'KernelOBJ=' + ' '.join(lab2_objects), 'build']
environment['lab2_objects'] = lab2_objects
environment['scoped_entry_adjustment'] = 'Only in temp main: omit trap_kernel_init/trap_kernel_inithart/timer_init calls.'
cases = [('scoped-entry-smoke', None, None, 'buddy two-core test passed', False)]
cases += [(name, 'official.c', f'LAB2_CASE {i}', marker, panic) for i, name, marker, panic in [
    (1, 'official-pmem-parallel', 'OFFICIAL PMEM PARALLEL PASS', False),
    (2, 'official-exhaust-kernel', 'buddy alloc: no suitable block', True),
    (3, 'official-exhaust-user', 'buddy alloc: no suitable block', True),
    (4, 'official-pmem-lifecycle', 'OFFICIAL PMEM LIFECYCLE PASS', False),
    (5, 'official-vm-print', 'OFFICIAL VM TWO-HART PAGING PASS', False),
    (6, 'official-vm-assert', 'OFFICIAL VM TWO-HART PAGING PASS', False),
]]
cases += [
    ('extra-buddy', 'buddy.c', 'BUDDY_BAD_CASE 0', '12 two-hart rounds, no overlaps, contents and capacity', False),
    ('extra-heap', 'heap.c', 'HEAP_BAD_CASE 0', 'HEAP ALL TESTS PASSED', False),
    ('extra-huge', 'huge.c', 'TEST_ERROR 0', 'HUGE ALL TESTS PASSED', False),
]
for i, message in enumerate([
    'buddy alloc: bad order', 'buddy free: unaligned page', 'buddy free: wrong region',
    'buddy free: invalid block or order', 'buddy free: invalid block or order',
    'buddy free: invalid block or order', 'buddy alloc: no suitable block',
    'buddy alloc: no suitable block', 'buddy alloc: no suitable block'], 1):
    cases.append((f'bad-buddy-{i}', 'buddy.c', f'BUDDY_BAD_CASE {i}', message, True))
for i, message in enumerate(['heap free: block already free', 'heap free: address is not a block start',
                             'heap free: address outside kernel page pool'], 1):
    cases.append((f'bad-heap-{i}', 'heap.c', f'HEAP_BAD_CASE {i}', message, True))
for i, message in enumerate(['vm_unmap: huge backing must be freed by its owner',
                             'vm_freewalk: leaf remains'], 1):
    cases.append((f'bad-huge-{i}', 'huge.c', f'TEST_ERROR {i}', message, True))
for i, message in enumerate(['vm_map: W requires R', 'vm: unaligned va', 'vm: bad range',
                             'vm_unmap: not mapped', 'vm_walk: null page table', 'vm: bad range'], 1):
    cases.append((f'bad-vm-{i}', 'vm_bad.c', f'VM_BAD_CASE {i}', message, True))

def boot(name, marker, expected_panic, attempt=0):
    command = ['qemu-system-riscv64', '-machine', 'virt', '-bios', 'default',
               '-kernel', str(workspace / 'target/kernel/kernel-qemu.elf'),
               '-m', '130M', '-smp', '2', '-accel', 'tcg,thread=multi', '-nographic']
    proc = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    data = bytearray()
    deadline = time.monotonic() + 60
    try:
        while time.monotonic() < deadline:
            for key, _ in selector.select(0.1):
                chunk = os.read(key.fileobj.fileno(), 65536)
                if chunk: data.extend(chunk)
            complete = marker.encode() in data and b'\n' in data.split(marker.encode(), 1)[1]
            panic_complete = b'panic!' in data and b'\n' in data.split(b'panic!', 1)[1]
            wrong_boot_hart = re.search(rb'Boot HART ID\s*:\s*1\r?\n', data)
            if complete or panic_complete or wrong_boot_hart or proc.poll() is not None: break
    finally:
        if proc.poll() is None: proc.terminate()
        try: data.extend(proc.communicate(timeout=3)[0])
        except subprocess.TimeoutExpired:
            proc.kill()
            data.extend(proc.communicate()[0])
        selector.close()
    raw = bytes(data)
    # 当前工程约定CPU0负责启动。TCG multi下OpenSBI可能先选择CPU1，
    # 这是启动前提不满足，保存日志后重启，绝不把它计为测试通过。
    if re.search(rb'Boot HART ID\s*:\s*1\r?\n', raw):
        (output / f'{name}.boot-hart1-attempt{attempt + 1}.log').write_bytes(raw)
        assert attempt < 11, f'{name}: could not obtain required boot hart 0'
        environment.setdefault('boot_hart_retries', {})[name] = attempt + 1
        return boot(name, marker, expected_panic, attempt + 1)
    (output / f'{name}.log').write_bytes(raw)
    assert marker.encode() in raw, f'{name}: expected marker missing; see raw log'
    assert (b'panic!' in raw) == expected_panic, f'{name}: unexpected panic state'
    if name == 'official-pmem-parallel':
        records = re.findall(rb'cpu ([01]) mem = (0x[0-9a-f]+), data = (\d+)', raw)
        assert len(records) == 1024 and len({r[1] for r in records}) == 1024
        assert all(r[2] == b'16843009' for r in records)
        assert sum(r[0] == b'0' for r in records) == sum(r[0] == b'1' for r in records) == 512
    return len(raw)

results = []
try:
    for name, filename, define, marker, expected_panic in cases:
        if args.only and args.only not in name: continue
        text = original_main if filename is None else (HERE / filename).read_text()
        if filename is None:
            for call in ('trap_kernel_init();', 'trap_kernel_inithart();', 'timer_init();'):
                text = text.replace(call, '/* Lab2 scoped build: later-lab initialization omitted. */')
        if filename:
            macro, value = define.split()
            # buddy参考旧入口直接define，其余通过ifndef：统一替换或前置。
            text = re.sub(rf'^#define {macro} \d+\s*$', f'#define {macro} {value}', text, flags=re.M)
            text = f'#ifndef {macro}\n#define {macro} {value}\n#endif\n' + text
        main.write_text(text)
        started = time.monotonic()
        build = subprocess.run(build_args[:1] + (['-B'] if not results else []) + build_args[1:],
                               cwd=workspace, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / f'{name}.build.log').write_bytes(build.stdout)
        assert build.returncode == 0, f'{name}: build failed; see build log'
        assert b'-Wall -Werror' in build.stdout, f'{name}: compiler flags missing'
        size = boot(name, marker, expected_panic)
        row = {'name': name, 'status': 'PASS', 'expected_panic': expected_panic,
               'expected': marker, 'seconds': round(time.monotonic() - started, 3), 'log_bytes': size}
        results.append(row)
        print(f'PASS {name}: {"expected panic" if expected_panic else "normal completion"} ({row["seconds"]}s)', flush=True)
finally:
    after = hashes()
    environment['source_unchanged'] = before == after
    (output / 'environment.json').write_text(json.dumps(environment, ensure_ascii=False, indent=2))
    (output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2))
    assert before == after, 'source files changed while tests ran'
print(f'ALL {len(results)} CASES PASSED; original source unchanged', flush=True)
print(f'Logs: {output}', flush=True)
