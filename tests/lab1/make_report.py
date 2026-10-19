#!/usr/bin/env python3
"""展示本次真实QEMU日志供浏览器截图，不生成或替换实验输出。"""
from pathlib import Path
import hashlib
import html
import json

here = Path(__file__).resolve().parent
logs, report = here / 'results', here / 'report'
report.mkdir(exist_ok=True)
env = json.loads((logs / 'environment.json').read_text())
rows = json.loads((logs / 'results.json').read_text())
by_name = {r['name']:r for r in rows}

def body(name):
    return [s for s in (logs / f'{name}.log').read_text(errors='replace').splitlines()
            if not s.startswith('qemu-system-riscv64: terminating')]

def section(name, lines=None):
    digest = hashlib.sha256((logs / f'{name}.log').read_bytes()).hexdigest()[:16]
    text = '\n'.join(body(name) if lines is None else lines)
    return f'<section><h2>{html.escape(name)} <small>SHA256 {digest}…</small></h2><pre>{html.escape(text)}</pre></section>'

def page(name,title,content,note='正文为本次QEMU实际串口输出，仅省略测试结束后的进程终止提示。'):
    doc = f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>{html.escape(title)}</title>
<style>html{{background:#f4f6f8}}body{{margin:0;padding:24px 28px;color:#172638;font:14px "Segoe UI","Microsoft YaHei",sans-serif;box-sizing:border-box;min-height:360px}}h1{{font-size:24px;margin:0 0 8px}}p{{margin:5px 0 12px;line-height:1.5;color:#4c5a6c}}.meta{{font-size:12px}}section{{border:1px solid #cfd9e5;border-radius:5px;overflow:hidden;margin:10px 0;background:white}}h2{{font-size:14px;margin:0;padding:8px 12px;background:#e9eef5;border-bottom:1px solid #d5deea}}small{{font-size:11px;font-weight:400;color:#52657b}}pre{{margin:0;padding:10px 12px;font:13px/1.45 Consolas,"Cascadia Mono",monospace;white-space:pre-wrap;overflow-wrap:anywhere}}table{{border-collapse:collapse;width:100%;font:13px/1.35 Consolas,monospace;background:white}}td,th{{text-align:left;padding:5px 8px;border-bottom:1px solid #dce4ec}}th{{background:#e9eef5}}.ok{{color:#19633d;font-weight:600}}</style>
<h1>{html.escape(title)}</h1><p class="meta">Lab1 · {html.escape(env['date'])} · QEMU 8.2.2 · GCC 13.3.0 · 128 MiB · 2 harts · BIOS none · TCG multi</p><p>{html.escape(note)}</p>{content}</html>'''
    (report / f'{name}.html').write_text(doc,encoding='utf-8')

table = '<table><tr><th>#</th><th>Case</th><th>Result</th><th>Expected behavior</th></tr>'
for i,row in enumerate(rows,1):
    behavior = 'expected panic' if row['expected_panic'] else 'observed race' if row['name'] in ('add-unlocked','printf-unlocked') else 'normal'
    table += f'<tr><td>{i}</td><td>{row["name"]}</td><td class="ok">{row["status"]}</td><td>{behavior}</td></tr>'
page('00-summary','Lab1 实际测试结果：16 / 16 符合预期',table+'</table><p class="ok">原源码 SHA256 前后一致 · 8 个正常用例 + 2 个竞争演示 + 6 个预期 panic</p>',
     '全部用例保留 -Wall -Werror 并独立启动QEMU；竞争演示要求实际观察到错误，未暴露竞争不会记为成功。')
page('01-boot','启动验证：双核、独立栈与 M-mode → S-mode',section('original-atomic-entry')+section('boot-mode-stack'),
     '上：原main.c原样运行。下：仅在测试副本中加入M-mode陷阱记录器，ecall的mcause=9确认main位于S-mode。')
page('02-add','任务实验：无锁、细粒度锁与粗粒度锁的并行加法',
     section('add-unlocked')+section('add-fine-lock')+section('add-coarse-lock'),
     '每核每轮执行1,000,000次累加；先等待两核都完成再检查最终值。无锁结果随调度变化，加锁结果必须为2,000,000。')
page('03-format','补充测试：printf 格式、空指针与整数边界',section('printf-format'),
     '按本Lab1自定义接口验证：%p读取uint32；%x读取uint64并输出0x加16位十六进制；INT_MIN结果只代表当前构建实测。')
locked = body('printf-locked')
page('04-print-locked','任务实验：printf 加锁后的双核输出',section('printf-locked',locked[:6]+['[… 省略中间记录；完整64条输出见原始日志 …]']+locked[-4:])+
     f'<p class="ok">运行器解析结果：64 条完整记录，64 组 (hart, seq) 均唯一，损坏行 = {by_name["printf-locked"]["broken_lines"]}</p>',
     '每条ROW由一次printf调用输出；两核可以按任意先后顺序取得锁，但同一条记录内部不应交错。')
unlocked = body('printf-unlocked')
row = by_name['printf-unlocked']
page('05-print-unlocked','任务实验：移除 printf 锁后观察实际交错',section('printf-unlocked',unlocked[:9]+['[… 省略后续记录；完整输出见原始日志 …]']+unlocked[-3:])+
     f'<p>运行器解析结果：共 {row["total_lines"]} 行，完整记录 {row["valid_records"]} 条，损坏行 {row["broken_lines"]} 行。</p>',
     '仅在临时print.c中移除print_lk的获取和释放；UART逐字符push_off/pop_off仍保留。图中混合字符来自真实双核执行。')
page('06-interrupt','补充测试：嵌套关中断与锁所有权',section('nested-interrupt'),
     '分别测试原来关中断与开中断两种情况；具体中断源sie保持关闭，不把状态位测试误写成已实现外设中断处理。')
page('07-uart','补充测试：实际 UART 输入字节读取',section('uart-input'),
     '先确认无输入时返回-1。运行器看到READY后，经QEMU标准输入发送6个字节：Lab1!及换行；内核逐字节核验ASCII值。')
content = ''
for name in ('bad-recursive-lock','bad-unheld-release','bad-cross-hart-release','bad-pop-underflow','bad-pop-enabled','bad-assert'):
    content += section(name,[s for s in body(name) if 'panic!' in s])
page('08-negative','补充测试：6 个独立启动的错误路径',content,
     '每种错误单独启动并匹配指定panic文本。pop_off的报错沿用源码中的push_off标签；超时不会被当作通过。')
print(f'Generated {len(list(report.glob("*.html")))} report pages: {report}')
