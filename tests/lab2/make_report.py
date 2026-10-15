#!/usr/bin/env python3
"""把本次真实日志展示为本地HTML，供浏览器截图；不生成或改写测试结果。"""
from pathlib import Path
import hashlib
import html
import json

here = Path(__file__).resolve().parent
logs = here / 'results'
report = here / 'report'
report.mkdir(exist_ok=True)
env = json.loads((logs / 'environment.json').read_text())
results = json.loads((logs / 'results.json').read_text())

def body(name):
    lines = (logs / f'{name}.log').read_text(errors='replace').splitlines()
    start = next(i for i, s in enumerate(lines) if s.startswith('Boot HART MEDELEG')) + 1
    return [s for s in lines[start:] if not s.startswith('qemu-system-riscv64: terminating')]

def section(name, lines=None):
    raw = logs / f'{name}.log'
    sha = hashlib.sha256(raw.read_bytes()).hexdigest()[:16]
    text = '\n'.join(body(name) if lines is None else lines)
    return f'<section><h2>{html.escape(name)} <small>SHA256 {sha}…</small></h2><pre>{html.escape(text)}</pre></section>'

def page(name, title, contents, note='日志正文为 QEMU 实际串口输出；已省略 OpenSBI 启动横幅和测试结束后的进程终止提示。'):
    doc = f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>{html.escape(title)}</title>
<style>html{{background:#f3f5f7}}body{{margin:0;padding:24px 28px;color:#182231;font:14px "Segoe UI","Microsoft YaHei",sans-serif;box-sizing:border-box}}
h1{{font-size:23px;margin:0 0 8px}}p{{margin:5px 0 10px;line-height:1.5;color:#435166}}h2{{font-size:14px;margin:0;padding:8px 12px;background:#e8edf4;border-bottom:1px solid #d2dce8}}small{{font-weight:400;font-size:11px;color:#52657b}}
section{{border:1px solid #d2dce8;border-radius:5px;overflow:hidden;margin:10px 0;background:white}}pre{{margin:0;padding:10px 12px;font:12px/1.45 Consolas,"Cascadia Mono",monospace;white-space:pre-wrap;overflow-wrap:anywhere;color:#152a40}}
.meta{{font-size:12px}}.ok{{color:#17663d;font-weight:600}}table{{width:100%;border-collapse:collapse;font:12px/1.25 Consolas,monospace;background:white}}td,th{{text-align:left;padding:4px 8px;border-bottom:1px solid #e1e7ee}}th{{background:#e8edf4}}</style>
<h1>{html.escape(title)}</h1><p class="meta">Lab2 · {html.escape(env['date'])} · QEMU 8.2.2 · RISC-V GCC 13.3.0 · 130 MiB · 2 harts · TCG multi</p>
<p>{html.escape(note)}</p>{contents}</html>'''
    (report / f'{name}.html').write_text(doc, encoding='utf-8')

rows = ''.join(f'<tr><td>{i}</td><td>{r["name"]}</td><td class="ok">{r["status"]}</td><td>{"expected panic" if r["expected_panic"] else "normal"}</td></tr>' for i, r in enumerate(results, 1))
page('00-summary', 'Lab2 实际测试结果：30 / 30 通过',
     '<table><tr><th>#</th><th>Case</th><th>Result</th><th>Expected behavior</th></tr>' + rows + '</table>' +
     f'<p class="ok">source_unchanged = {str(env["source_unchanged"]).lower()} · 8 正常结束 + 22 预期 panic</p>',
     '这些结果针对 Lab2 单独构建。原工程完整构建 exit=2，原因是后续实验 proc.c 的未完成函数；详情单独记录。')
parallel = body('official-pmem-parallel')
page('01-official-parallel', '任务要求：双核申请并释放全部 1024 页',
     section('official-pmem-parallel', parallel[:9] + [f'[… 省略中间 {len(parallel)-19} 行；完整输出见原始 .log …]'] + parallel[-10:]),
     '截图展示原始日志的首尾片段。程序及运行器同时核验：每核 512 页、1024 个地址无重复、内容正确、容量恢复。')
page('02-official-exhaust', '任务要求：内核区与用户区内存耗尽',
     section('official-exhaust-kernel') + section('official-exhaust-user'),
     '这两个独立启动用例以指定 panic 为成功条件；运行器核验 panic 文本，超时或意外错误不会记为通过。')
page('03-official-user', '任务要求：用户页申请、回收、再分配清零', section('official-pmem-lifecycle'))
page('04-official-vm', '任务要求：页表修改前后的实际输出', section('official-vm-print'),
     '5 个有效叶子变为 3 个；VA=0 的 flags 从 0x43 变为 0xc7；两个 hart 均已启用分页。')
page('05-official-vm-assert', '任务要求：映射与解映射断言', section('official-vm-assert') + section('scoped-entry-smoke'))
page('06-extra-buddy', '补充测试：Buddy 拆分、合并、碎片、双核并发', section('extra-buddy'))
page('07-extra-heap', '补充测试：内核堆边界、对齐、耗尽、跨核释放', section('extra-heap'))
page('08-extra-huge', '补充测试：大页、局部拆分与真实 MMU 访问', section('extra-huge'),
     '2 MiB 测试包含真实 satp 切换和读写；1 GiB 测试只验证页表结构，不访问超出物理 RAM 的映射。')
for group, title, count in [('buddy', 'Buddy 的 9 个错误路径', 9), ('heap', '堆和大页的错误路径', 3), ('vm', '页表参数的 6 个错误路径', 6)]:
    content = ''
    for i in range(1, count + 1):
        name = f'bad-{group}-{i}'
        content += section(name, [x for x in body(name) if 'panic!' in x or 'must panic' in x])
    if group == 'heap':
        for i in (1, 2):
            name = f'bad-huge-{i}'
            content += section(name, [x for x in body(name) if 'panic!' in x])
    number = {'buddy': '09', 'heap': '10', 'vm': '11'}[group]
    page(f'{number}-bad-{group}', '补充测试：' + title, content, '每个框来自一次独立 QEMU 启动；展示触发的实际 panic 行。指定错误被拦截即为该用例通过。')
full_lines = (logs / 'full-project.build.log').read_text().splitlines()
error_start = next(i for i, s in enumerate(full_lines) if 'In function' in s)
page('12-build-scope', '构建范围及当前工程的实际限制',
     '<section><h2>完整工程 make -B build：exit=2</h2><pre>' + html.escape('\n'.join(full_lines[error_start:])) + '</pre></section>' +
     '<section><h2>本次 Lab2 构建范围</h2><pre>' + html.escape('\n'.join(env['lab2_objects'])) + '</pre></section>',
     '未改动原工程源码。测试副本排除后续实验 proc/timer/trap C 文件；保留当前链接脚本需要的 trampoline 汇编。')
print(f'Generated {len(list(report.glob("*.html")))} local report pages: {report}')
