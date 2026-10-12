#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bump_version.py —— 文档集版本号与修订记录的单一入口

为什么需要它
    本仓库的版本号散落在 4 份文档里（PRD / Tech_Design / 项目规划书 / AGENTS），
    手工同步极易漏掉其中一份，造成"文档集版本不一致"——而那正是 V1.0 阶段花力气
    消除的问题。本脚本保证：一次调用，4 份文档的版本标记与修订记录同步更新。

行为
    1) 改写各文档的版本标记（每份的写法不同，用各自的正则匹配）
    2) 在"修订记录"表格的表头分隔行之后插入一行新记录（最新在最上）
    3) 幂等：若该版本已存在于修订记录，则跳过，不重复插入

用法
    py tools/bump_version.py 1.1 "新增实测记录与一键构建脚本"
    py tools/bump_version.py --show          # 只显示当前版本标记
"""

from __future__ import annotations

import argparse
import datetime
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 每份文档的版本标记写法不同，各配一条正则（只替换版本数字）
TARGETS = {
    "PRD.md":            r"(> 版本：\*{0,2}[Vv])(\d+\.\d+)",
    "Tech_Design.md":    r"(> 版本：\*{0,2}[Vv])(\d+\.\d+)",
    "项目规划书.md":      r"(\| 文档版本 \| \*{0,2}[Vv])(\d+\.\d+)",
    "AGENTS.md":         r"(> 文档集版本：\*{0,2}[Vv])(\d+\.\d+)",
    "设计报告.md":        r"(\| 报告版本 \| \*{0,2}[Vv])(\d+\.\d+)",
}

TABLE_SEP = re.compile(r"^\|\s*-{3,}.*\|\s*$")


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def show() -> None:
    for name, pat in TARGETS.items():
        m = re.search(pat, _read(ROOT / name))
        print("  %-18s %s" % (name, m.group(0) if m else "未找到版本标记"))


def bump(version: str, message: str) -> int:
    today = datetime.date.today().isoformat()
    changed = 0

    for name, pat in TARGETS.items():
        path = ROOT / name
        text = _read(path)
        lines = text.splitlines()

        # 1) 版本标记
        new_text, n = re.subn(pat, lambda m: m.group(1) + version, text)
        if n == 0:
            print("  ! %-18s 未匹配到版本标记，跳过" % name)
            continue
        lines = new_text.splitlines()

        # 2) 修订记录：定位"修订记录"标题后的第一个表格分隔行，在其后插入
        idx_rec = next((i for i, ln in enumerate(lines)
                        if ln.strip().startswith("##") and "修订记录" in ln), None)
        if idx_rec is None:
            print("  ! %-18s 未找到修订记录章节，只改了版本号" % name)
        else:
            sep = next((i for i in range(idx_rec, len(lines))
                        if TABLE_SEP.match(lines[i])), None)
            if sep is None:
                print("  ! %-18s 修订记录表格结构异常，只改了版本号" % name)
            else:
                row = "| **V%s** | %s | %s |" % (version, today, message)
                if any(("V%s" % version) in ln for ln in lines[idx_rec:sep + 30]):
                    print("  = %-18s 修订记录已含 V%s，跳过插入" % (name, version))
                else:
                    lines.insert(sep + 1, row)

        (ROOT / name).write_text("\n".join(lines) + "\n", encoding="utf-8")
        print("  OK %-18s -> V%s" % (name, version))
        changed += 1

    return changed


def main() -> None:
    ap = argparse.ArgumentParser(description="文档集版本号与修订记录同步")
    ap.add_argument("version", nargs="?", help="新版本号，如 1.1")
    ap.add_argument("message", nargs="?", default="", help="修订记录说明")
    ap.add_argument("--show", action="store_true", help="只显示当前版本标记")
    args = ap.parse_args()

    if args.show or not args.version:
        show()
        return
    if not re.fullmatch(r"\d+\.\d+", args.version):
        sys.exit("版本号格式应为 主.次，例如 1.1")
    if not args.message:
        sys.exit("请给出修订记录说明（第二个参数）")

    n = bump(args.version, args.message)
    print("已更新 %d 份文档。记得重跑：py tools/md2docx.py" % n)


if __name__ == "__main__":
    main()
