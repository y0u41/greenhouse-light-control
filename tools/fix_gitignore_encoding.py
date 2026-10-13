#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fix_gitignore_encoding.py —— 把 .gitignore 里**唯一一行** GBK 中文注释转成 UTF-8。

背景
    仓库里其它文件都是 UTF-8，只有 .gitignore 第 48 行的中文注释
    `# ---- 主机测试产物 ----` 是 GBK 字节（D6 F7 BB FA ...），
    导致整个文件严格 UTF-8 解码失败（GitHub 显示乱码、编辑器报警告）。

做法
    只替换那一行的字节，其余**逐字节原样保留**（含 CRLF / 裸 LF 的混用）。
    脚本自带前后不一致断言，重跑幂等。
"""

from __future__ import annotations

import pathlib
import sys

TARGET_LINE = 48          # 1-based
root = pathlib.Path(__file__).resolve().parent.parent
p = root / ".gitignore"

raw = p.read_bytes()
parts = raw.split(b"\n")          # 保留各自的行尾（\r 留在行内）
idx = TARGET_LINE - 1
old = parts[idx]

try:
    old.decode("utf-8")
    print("[skip] 第 %d 行已是合法 UTF-8，无需改动。" % TARGET_LINE)
    sys.exit(0)
except UnicodeDecodeError:
    pass

new_text = old.decode("gbk")
new = new_text.encode("utf-8")
print("第 %d 行：GBK -> UTF-8" % TARGET_LINE)
print("  before: %s" % " ".join("%02X" % c for c in old))
print("  after : %s" % " ".join("%02X" % c for c in new))
print("  text  : %r  ->  %r" % (old.decode("gbk"), new_text))

parts[idx] = new
out = b"\n".join(parts)

# ---- 自检：除目标行外，其余每一行必须逐字节相同 ----
before_lines = raw.split(b"\n")
after_lines = out.split(b"\n")
assert len(before_lines) == len(after_lines), "行数变了！"
for i, (a, b) in enumerate(zip(before_lines, after_lines)):
    if i != idx:
        assert a == b, "第 %d 行被意外改动！" % (i + 1)
print("自检通过：仅第 %d 行变化，其余 %d 行逐字节一致。"
      % (TARGET_LINE, len(before_lines) - 1))

# ---- 自检：整文件严格 UTF-8 ----
out.decode("utf-8")
print("自检通过：整文件现在可严格 UTF-8 解码。")

p.write_bytes(out)
print("已写盘：%s" % p)
