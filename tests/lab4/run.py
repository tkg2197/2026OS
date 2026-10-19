#!/usr/bin/env python3
"""Lab4 acceptance on isolated source copies; no host packages needed."""
from pathlib import Path
import hashlib
import json
import os
import re
import selectors
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'tests/lab4/results'


def build(work, name, succeeds=True):
    result = subprocess.run(['make', '-j4', 'build'], cwd=work,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / (name + '-build.log')).write_bytes(result.stdout)
    assert (result.returncode == 0) == succeeds, result.stdout.decode(errors='replace')
    return result.stdout


def run(work, name, seconds=1.6, send=False):
    p = subprocess.Popen(['qemu-system-riscv64', '-machine', 'virt', '-bios', 'default',
                          '-kernel', 'target/kernel/kernel-qemu.elf', '-m', '130M', '-smp', '2', '-nographic'],
                         cwd=work, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
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
            if send and not sent and (b'proczero: hello world!' in data or b'user fault:' in data):
                time.sleep(.12)
                for c in payload:
                    p.stdin.write(bytes([c])); p.stdin.flush(); time.sleep(.002)
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


def check_echo(data):
    assert b'abc\x08 \x08D\x08 \x08E\r\nF\r\n' in data
    assert bytes(range(65, 91)) * 3 + b'\r\n' in data


def assert_healthy(data, hello=2):
    assert b'panic!' not in data and b'user fault:' not in data, data[-1800:]
    assert data.count(b'proczero: hello world!') == hello, data[-1800:]
    assert b'buddy two-core test passed' in data


def register_program():
    # 不经过C编译器寄存器分配，直接验证全部可写整数寄存器。
    regs = ['ra', 'gp', 'tp', 't1', 't2', 's0', 's1'] + ['a'+str(i) for i in range(1,7)] + ['s'+str(i) for i in range(2,12)] + ['t3','t4','t5','t6']
    lines = ['addi sp, sp, -32', 'li t0, 0x12345678', 'sd t0, 0(sp)']
    lines += [f'li {r}, {65536+i}' for i,r in enumerate(regs)]
    lines += ['li a0, 31337', 'li a7, 0', 'ecall', 'bnez a0, 9f',
              'li t0, 100000000', '1: addi t0, t0, -1', 'bnez t0, 1b', 'bnez a0, 9f', 'bnez a7, 9f']
    for i,r in enumerate(regs):
        lines += [f'li t0, {65536+i}', f'bne {r}, t0, 9f']
    lines += ['li t0, 0x3fffffdfe0', 'bne sp, t0, 9f', 'ld t1, 0(sp)',
              'li t0, 0x12345678', 'bne t1, t0, 9f', 'li a7, 0', 'ecall', 'bnez a0, 9f',
              '2: j 2b', '9: ebreak', 'j 9b']
    assembly = '\\n'.join(lines) + '\\n'
    return '__attribute__((naked, section(".text.entry"))) void main(void) {\nasm volatile("' + assembly + '");\n}\n'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    before = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (ROOT/'src').rglob('*') if p.is_file() and p.name != 'initcode.h'}
    results = {}
    with tempfile.TemporaryDirectory(prefix='os-lab4-tests-') as temp:
        parent = Path(temp)
        work = copy_source(parent, 'baseline')
        # 在发布之前验证真实页表权限、别名和保护页。
        checks = '''    int level;
    pte_t *entry = vm_lookup(p->pgtbl, USER_BASE, &level);
    assert(entry && level == 0 && (*entry & (PTE_R|PTE_X|PTE_U)) == (PTE_R|PTE_X|PTE_U) && !(*entry & PTE_W), "code permission");
    entry = vm_lookup(p->pgtbl, USTACK, &level);
    assert(entry && (*entry & (PTE_R|PTE_W|PTE_U)) == (PTE_R|PTE_W|PTE_U) && !(*entry & PTE_X), "stack permission");
    entry = vm_lookup(p->pgtbl, TRAPFRAME, &level);
    assert(entry && PTE_TO_PA(*entry) == (uint64)p->tf && !(*entry & PTE_U), "trapframe alias");
    entry = vm_lookup(p->pgtbl, TRAMPOLINE, &level);
    assert(entry && PTE_TO_PA(*entry) == (uint64)trampoline && !(*entry & (PTE_U|PTE_W)), "trampoline permission");
    assert(vm_lookup(p->pgtbl, 0, NULL) == NULL, "zero page mapped");
    pgtbl_t kernel = (pgtbl_t)((r_satp() & ((1ull<<44)-1)) << 12);
    assert(vm_lookup(kernel, TRAPFRAME, NULL) == NULL, "kernel guard mapped");
    assert(vm_lookup(kernel, KSTACK(0), NULL) != NULL, "kernel stack missing");
    assert(!vm_try_mappage(p->pgtbl, USER_BASE, (uint64)code, PTE_R|PTE_X|PTE_U), "duplicate mapping accepted");
    assert(!vm_try_mappage(p->pgtbl, 1, (uint64)code, PTE_R), "unaligned mapping accepted");
    assert(!vm_try_mappage(p->pgtbl, 0, (uint64)code, PTE_W), "write-only mapping accepted");
    printf("[layout] passed\\n");
'''
        change(work,'src/kernel/proc/proc.c','    // 所有资源就绪后才发布。', checks+'    // 所有资源就绪后才发布。')
        build(work, 'baseline')
        data = run(work, 'baseline', seconds=2.3, send=True)
        assert_healthy(data); check_echo(data)
        assert b'[layout] passed' in data
        assert data.count(b'is booting with OpenSBI!') == 2
        results['default_two_ecalls_echo_layout'] = 'PASS'

        # 同一副本不clean，验证用户源码与INTERVAL头文件的依赖重编译。
        old = (work/'target/kernel/kernel-qemu.elf').stat().st_mtime_ns
        with (work/'src/user/initcode.c').open('a') as f: f.write('\n// incremental build probe\n')
        out = build(work, 'incremental-user')
        assert b'target/user/initcode.o' in out and b'xxd' in out and b'target/kernel/proc/proc.o' in out
        assert (work/'target/kernel/kernel-qemu.elf').stat().st_mtime_ns > old
        change(work,'src/kernel/trap/type.h','#define INTERVAL 1000000','#define INTERVAL 900000')
        out = build(work,'incremental-timer-header')
        assert b'target/kernel/trap/timer.o' in out and b'target/kernel/trap/trap_kernel.o' in out
        results['incremental_dependencies'] = 'PASS'

        work = copy_source(parent, 'unknown-syscall')
        (work/'src/user/initcode.c').write_text('''#include "sys.h"
__attribute__((section(".text.entry"))) int main(void) {
    if (syscall(999) != -1) asm volatile("ebreak");
    if (syscall(SYS_helloworld) != 0) asm volatile("ebreak");
    if (syscall(SYS_helloworld) != 0) asm volatile("ebreak");
    while (1) ;
}
''')
        build(work, 'unknown-syscall')
        assert_healthy(run(work, 'unknown-syscall'))
        results['unknown_minus_one_and_known_zero'] = 'PASS'

        work = copy_source(parent, 'register-interrupts')
        (work/'src/user/initcode.c').write_text(register_program())
        change(work, 'src/kernel/trap/trap_user.c', '        timer_interrupt_handler();', '''    {
        timer_interrupt_handler();
        printf("[user timer] tick=%x pc=%x\\n", timer_get_ticks(), p->tf->user_to_kern_epc);
    }''')
        build(work, 'register-interrupts')
        data = run(work, 'register-interrupts', seconds=4, send=True)
        assert_healthy(data)
        ticks = [int(x,16) for x in re.findall(rb'\[user timer\] tick=0x([0-9a-f]+)', data)]
        assert len(ticks)>=5 and all(b>a for a,b in zip(ticks,ticks[1:])), ticks
        # timer诊断可能插在普通输入字节间；先移除整条诊断后核对终端协议。
        clean = re.sub(rb'\[user timer\] tick=0x[0-9a-f]+ pc=0x[0-9a-f]+\n', b'', data)
        check_echo(clean)
        results['all_integer_registers_stack_user_timer_uart'] = {'status':'PASS','user_timer_events':len(ticks)}

        faults = {
            'zero-read': ('(void)*(volatile unsigned long *)0;',13),
            'zero-write': ('*(volatile unsigned long *)0=1;',15),
            'code-write': ('*(volatile unsigned long *)0x1000=1;',15),
            'trapframe-read': ('(void)*(volatile unsigned long *)0x3fffffe000;',13),
            'trampoline-read': ('(void)*(volatile unsigned long *)0x3ffffff000;',13),
            'kernel-read': ('(void)*(volatile unsigned long *)0x80200000;',13),
            'stack-execute': ('((void (*)(void))0x3fffffdfc0)();',12),
            'illegal-instruction': ('asm volatile(".word 0");',2),
        }
        for name,(trigger,cause) in faults.items():
            work = copy_source(parent,name)
            (work/'src/user/initcode.c').write_text('#include "sys.h"\n__attribute__((section(".text.entry"))) int main(void) { syscall(SYS_helloworld); '+trigger+' syscall(SYS_helloworld); while(1); }\n')
            build(work,name)
            data=run(work,name,seconds=1.8,send=True)
            assert b'panic!' not in data and data.count(b'proczero: hello world!')==1, data[-1500:]
            assert f'scause=0x{cause:016x}'.encode() in data and data.count(b'user fault:')==1, data[-1500:]
            check_echo(data)
            results[name]={'status':'PASS','scause':cause,'uart_after_fault':True}

        # 第1..8次分配覆盖tf、根、两层高地址表、用户栈、代码、两层低地址表。
        for nth in range(1,9):
            name=f'allocation-failure-{nth}'
            work=copy_source(parent,name)
            change(work,'src/kernel/mem/pmem.c','void *pmem_try_alloc(bool in_kernel)\n{', '''extern int test_alloc_active, test_alloc_count, test_fail_at;
void *pmem_try_alloc(bool in_kernel)
{
    if (test_alloc_active && ++test_alloc_count == test_fail_at)
        return NULL;''')
            with (work/'src/kernel/proc/proc.c').open('a') as f:
                f.write('\nint test_alloc_active, test_alloc_count, test_fail_at;\n')
            change(work,'src/kernel/main.c','extern void _entry(void);','extern int test_alloc_active, test_alloc_count, test_fail_at;\nextern void _entry(void);')
            change(work,'src/kernel/main.c','        proc_make_first();',f'''        uint32 before_k=pmem_freepages(true), before_u=pmem_freepages(false);
        test_fail_at={nth}; test_alloc_active=1;
        proc_make_first();
        test_alloc_active=0;
        assert(test_alloc_count=={nth}, "failure injection not reached");
        assert(myproc()==NULL, "failed process published");
        assert(pmem_freepages(true)==before_k && pmem_freepages(false)==before_u, "construction leaked pages");
        printf("[oom] passed {nth}\\n");''')
            build(work,name)
            data=run(work,name,seconds=1.1)
            assert f'[oom] passed {nth}'.encode() in data and b'panic!' not in data, data[-1200:]
            results[name]='PASS: no published process, both page pools restored'

        work=copy_source(parent,'oversized-image')
        (work/'src/user/initcode.c').write_text('__attribute__((used)) const char image[5000]={1};\n__attribute__((section(".text.entry"))) int main(void){while(1);}\n')
        out=build(work,'oversized-image',succeeds=False)
        assert b'initcode is larger than one page' in out
        results['oversized_image_link_rejected']='PASS'
    after={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
           for p in (ROOT/'src').rglob('*') if p.is_file() and p.name!='initcode.h'}
    assert before==after
    results['production_source_unchanged']=True
    results['source_sha256']=before
    (OUT/'summary.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({k:v for k,v in results.items() if k!='source_sha256'},ensure_ascii=False,indent=2))


if __name__=='__main__':
    main()
