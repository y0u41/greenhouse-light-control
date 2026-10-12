#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sync_doc_tables.py —— 把 Tech_Design.md 的技术事实**生成**进 设计报告.md 的生成区

为什么需要它（P1-1）
    《设计报告》要保持独立自洽（评审老师只看这一份也能读懂），因此它必须保留
    引脚表 / 协议表 / 阈值表 / 端口表。但"同一事实两份手工副本"必然漂移。
    解法：报告的这些表格**不做手工副本**，而是从唯一事实源 Tech_Design.md
    生成；手工只在 Tech_Design.md 改。本脚本就是生成器 + 漂移守护。

标记格式
    源（唯一事实源，只手工改这里）—— Tech_Design.md
        <!-- FACT:<id> BEGIN -->
        ...任意 Markdown（表格 / 代码块 / 清单）...
        <!-- FACT:<id> END -->

    目标（生成区，**禁止手工修改**）—— 设计报告.md
        <!-- GENERATED:BEGIN <id>  (source: Tech_Design.md#<anchor>) -->
        ...由本脚本填充，手工改动会在下一次运行/校验时被覆盖或报错...
        <!-- GENERATED:END <id> -->

用法
    py tools/sync_doc_tables.py            # 就地同步（幂等：内容相同则不写盘）
    py tools/sync_doc_tables.py --check    # 只校验：与磁盘不一致 -> 打印 diff，退出码 1
    py tools/sync_doc_tables.py -v         # 打印每个生成区的同步结果
    py tools/sync_doc_tables.py --list     # 只列源块 <-> 生成区对应关系

退出码
    0 = 一致（或已成功同步）；1 = 存在漂移 / 结构错误；2 = 文件缺失
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "Tech_Design.md"
# 目标文档 -> 允许出现的生成区 id 集合（None = 不限）
TARGETS: dict[str, set[str] | None] = {
    "设计报告.md": None,
}

FACT_RE = re.compile(r"^[ \t]*<!--\s*FACT:([A-Za-z0-9_.-]+)\s+BEGIN\s*-->\s*$")
FACT_END_RE = re.compile(r"^[ \t]*<!--\s*FACT:([A-Za-z0-9_.-]+)\s+END\s*-->\s*$")
GEN_RE = re.compile(r"^[ \t]*<!--\s*GENERATED:BEGIN\s+([A-Za-z0-9_.-]+).*-->\s*$")
GEN_END_RE = re.compile(r"^[ \t]*<!--\s*GENERATED:END\s+([A-Za-z0-9_.-]+)\s*-->\s*$")


class SyncError(Exception):
    pass


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def split_lines(text: str) -> list[str]:
    """保留行尾换行的行列表（最后一行可能无换行）。"""
    return text.splitlines(keepends=True)


def parse_source(text: str) -> dict[str, list[str]]:
    """返回 {id: [内容行...]}（不含 BEGIN/END 标记行本身）。"""
    blocks: dict[str, list[str]] = {}
    cur: str | None = None
    buf: list[str] = []
    for n, line in enumerate(split_lines(text), 1):
        m = FACT_RE.match(line)
        if m:
            if cur is not None:
                raise SyncError("第 %d 行：%s 的 BEGIN 尚未 END" % (n, cur))
            cur = m.group(1)
            if cur in blocks:
                raise SyncError("第 %d 行：源块 id 重复：%s" % (n, cur))
            buf = []
            continue
        m = FACT_END_RE.match(line)
        if m:
            if cur is None:
                raise SyncError("第 %d 行：出现游离的 FACT END（%s）" % (n, m.group(1)))
            if m.group(1) != cur:
                raise SyncError("第 %d 行：END(%s) 与 BEGIN(%s) 不匹配"
                                % (n, m.group(1), cur))
            blocks[cur] = buf
            cur = None
            continue
        if cur is not None:
            buf.append(line)
    if cur is not None:
        raise SyncError("源块 %s 缺少 END 标记" % cur)
    return blocks


def canon(lines: list[str]) -> str:
    """规范化块内容：去掉首尾空行，逐行去行尾空白，统一以换行结束。"""
    body = [l.rstrip() for l in lines]
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    return "".join(l + "\n" for l in body)


def parse_regions(text: str) -> list[dict]:
    """返回 [{id, begin_i, end_i, body}]，索引为行下标（含标记行）。"""
    out: list[dict] = []
    pending: dict | None = None
    for i, line in enumerate(split_lines(text)):
        m = GEN_RE.match(line)
        if m:
            if pending is not None:
                raise SyncError("生成区 %s 的 BEGIN 尚未 END" % pending["id"])
            pending = {"id": m.group(1), "begin_i": i, "body": []}
            continue
        m = GEN_END_RE.match(line)
        if m:
            if pending is None:
                raise SyncError("出现游离的 GENERATED:END（%s）" % m.group(1))
            if m.group(1) != pending["id"]:
                raise SyncError("GENERATED END(%s) 与 BEGIN(%s) 不匹配"
                                % (m.group(1), pending["id"]))
            pending["end_i"] = i
            out.append(pending)
            pending = None
            continue
        if pending is not None:
            pending["body"].append(line)
    if pending is not None:
        raise SyncError("生成区 %s 缺少 END 标记" % pending["id"])
    return out


def rebuild(text: str, blocks: dict[str, list[str]], verbose: bool) -> tuple[str, list[str]]:
    """返回 (新文本, 变化的 id 列表)。未涉及的字节一律原样保留。"""
    lines = split_lines(text)
    regions = parse_regions(text)
    seen: dict[str, int] = {}
    for r in regions:
        seen[r["id"]] = seen.get(r["id"], 0) + 1
    dup = [k for k, v in seen.items() if v > 1]
    if dup:
        raise SyncError("生成区 id 重复：%s" % ", ".join(dup))

    changed: list[str] = []
    for r in reversed(regions):          # 从后往前替换，索引不失效
        rid = r["id"]
        if rid not in blocks:
            raise SyncError("生成区 %s 在 %s 里找不到对应的 FACT 源块" % (rid, SOURCE.name))
        want = canon(blocks[rid])
        have = canon(r["body"])
        if want != have:
            changed.append(rid)
        new_body = [l + "\n" for l in want.splitlines()]
        lines[r["begin_i"] + 1:r["end_i"]] = new_body
    return "".join(lines), list(reversed(changed))


class _Tee:
    """把 stdout 同时写进 UTF-8 文件（PowerShell 管道会按 936 代码页误读中文）。"""

    def __init__(self, path: str) -> None:
        self.f = open(path, "w", encoding="utf-8", newline="\n")

    def write(self, s: str) -> int:
        sys.__stdout__.write(s)
        self.f.write(s)
        return len(s)

    def writelines(self, lines) -> None:
        for s in lines:
            self.write(s)

    def flush(self) -> None:
        sys.__stdout__.flush()
        self.f.flush()


def main() -> int:
    ap = argparse.ArgumentParser(description="Tech_Design -> 设计报告 表格生成器")
    ap.add_argument("--check", action="store_true", help="只校验，不写盘；不一致则退出码 1")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--list", action="store_true", help="列出源块与生成区对应关系")
    ap.add_argument("--tee", default=None, help="把输出（UTF-8）同时写进该文件")
    args = ap.parse_args()
    if args.tee:
        sys.stdout = _Tee(args.tee)  # type: ignore[assignment]

    if not SOURCE.is_file():
        print("!! 找不到唯一事实源：%s" % SOURCE)
        return 2
    try:
        blocks = parse_source(read(SOURCE))
    except SyncError as e:
        print("!! 源文件标记结构错误：%s" % e)
        return 1

    if args.list:
        print("唯一事实源 %s：共 %d 个 FACT 源块" % (SOURCE.name, len(blocks)))
        for name in TARGETS:
            p = ROOT / name
            if not p.is_file():
                continue
            used = {r["id"] for r in parse_regions(read(p))}
            print("\n目标 %s：%d 个生成区" % (name, len(used)))
            for rid in sorted(used):
                mark = "OK " if rid in blocks else "!! "
                print("  %s%-22s <- FACT:%s" % (mark, rid, rid))
            unused = sorted(set(blocks) - used)
            plain = sorted(set(used) - set(blocks))
            if plain:
                print("  !! 目标里有源中不存在的生成区：%s" % ", ".join(plain))
            if unused:
                print("  (源块未被镜像：%s)" % ", ".join(unused))
        return 0

    rc = 0
    for name in TARGETS:
        p = ROOT / name
        if not p.is_file():
            print("!! 找不到目标文档：%s" % p)
            return 2
        old = read(p)
        try:
            new, changed = rebuild(old, blocks, args.verbose)
        except SyncError as e:
            print("!! %s：%s" % (name, e))
            return 1

        if old == new:
            print("[OK  ] %s 已与 %s 一致（%d 个生成区，0 处漂移）"
                  % (name, SOURCE.name, len(parse_regions(old))))
            if args.verbose:
                for r in parse_regions(old):
                    print("         = %s" % r["id"])
            continue

        print("[DRIFT] %s 与 %s 不一致，涉及 %d 个生成区：%s"
              % (name, SOURCE.name, len(changed), ", ".join(changed)))
        if args.check:
            diff = difflib.unified_diff(
                split_lines(old), split_lines(new),
                fromfile=name + "（磁盘现状）", tofile=name + "（按 Tech_Design 重新生成）",
                n=2)
            sys.stdout.writelines(diff)
            rc = 1
        else:
            p.write_text(new, encoding="utf-8", newline="")
            print("        已重新生成并写盘。请勿手工编辑 GENERATED 区。")
    if rc:
        print("\n!! 检测到文档漂移：改了 Tech_Design.md 却忘了同步设计报告。"
              "\n   修复：py tools/sync_doc_tables.py")
    return rc


if __name__ == "__main__":
    sys.exit(main())
