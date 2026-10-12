#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
打包交作业 / 发群用的源码包 —— 结构性排除密钥，而不是靠"记得别放进去"。

背景（P0-1 的第二半）：
  .gitignore 只保护"git 仓库"，保护不了"把目录打 zip 交作业"。
  只要有人右键压缩整个目录，firmware/Core/Inc/wifi_config.h 里的
  WiFi SSID/密码就随包带走 —— 而 .gitignore 对此毫无作用。

解法：
  用 `git archive HEAD` 打包。它只导出**受版本控制的文件**，
  而 wifi_config.h 因被 .gitignore 排除、从未进入任何提交，
  因此**从原理上不可能出现在包里**。这比"人工核对"可靠得多。

脚本还会在打包后**自己校验一次**（防御性）：
  1. 列出包内文件，确认没有 wifi_config.h / .env / *.uvoptx / Output/ 等；
  2. 把 wifi_config.h 里真实的 SSID 与密码取出来，在包内全文搜索，
     确认一个字都不出现（这一步能抓住"不小心复制粘贴进别的文件"的情况）。

用法：
  python tools/package_submission.py                    # 生成源码包
  python tools/package_submission.py --docs-only        # 只打包文档（更小）
  python tools/package_submission.py -o D:\\提交.zip
"""

import argparse
import os
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 包内绝对不允许出现的路径特征
FORBIDDEN_PATTERNS = [
    r"(^|/)wifi_config\.h$",       # WiFi 凭据本体
    r"(^|/)\.env$",
    r"\.uvoptx$",                  # Keil 用户态文件（含 ST-Link 序列号）
    r"\.uvguix\.",
    r"^firmware/Output/",          # 构建产物
    r"^qtapp/build/",
    r"^_attic/",
    r"__pycache__/",
    r"\.pyc$",
    r"\.log$",
    r"\.bak",
    r"(^|/)\.git/",
]

DOC_SUFFIXES = {".md", ".docx", ".pdf", ".txt"}


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True,
                          encoding="utf-8", errors="replace", **kw)


def git_ok() -> bool:
    r = run(["git", "rev-parse", "--is-inside-work-tree"])
    return r.returncode == 0 and r.stdout.strip() == "true"


def secrets_from_local_config():
    """从本地 wifi_config.h 取出真实凭据（仅用于校验，绝不写进包）。"""
    p = ROOT / "firmware" / "Core" / "Inc" / "wifi_config.h"
    if not p.is_file():
        return []
    txt = p.read_text(encoding="utf-8", errors="replace")
    out = []
    for key in ("WIFI_SSID", "WIFI_PASSWORD", "TCP_SERVER_IP"):
        m = re.search(r'#define\s+%s\s+"([^"]*)"' % key, txt)
        if m and len(m.group(1)) >= 4:          # 太短的（如 "1"）搜了没意义
            out.append((key, m.group(1)))
    return out


def make_archive(out_path: Path, docs_only: bool) -> bool:
    cmd = ["git", "archive", "--format=zip", "-o", str(out_path), "HEAD"]
    if docs_only:
        # 只导出文档：用 pathspec 限定
        cmd = ["git", "archive", "--format=zip", "-o", str(out_path), "HEAD",
               "*.md", "docs"]
    r = run(cmd)
    if r.returncode != 0:
        print("  [FAIL] git archive 失败：")
        print("         " + (r.stderr or r.stdout).strip())
        return False
    return True


def verify(out_path: Path) -> bool:
    problems = []
    secrets = secrets_from_local_config()
    leaked = {k: 0 for k, _ in secrets}

    with zipfile.ZipFile(out_path) as z:
        names = z.namelist()

        # ① 路径级检查
        for n in names:
            for pat in FORBIDDEN_PATTERNS:
                if re.search(pat, n):
                    problems.append("包内出现禁止项: %s  (匹配 %s)" % (n, pat))
                    break

        # ② 内容级检查：真实凭据不得出现在包内任何文件中
        for n in names:
            if n.endswith("/"):
                continue
            try:
                raw = z.read(n)
            except Exception:
                continue
            if b"\x00" in raw[:4096]:       # 二进制跳过
                continue
            txt = raw.decode("utf-8", errors="replace")
            for k, v in secrets:
                if v in txt:
                    leaked[k] += 1
                    problems.append("凭据泄漏: %s 的值出现在 %s" % (k, n))

    print("  包内文件数: %d" % len(names))
    if secrets:
        print("  已用 %d 项本地真实凭据做过全文搜索: %s"
              % (len(secrets), ", ".join(k for k, _ in secrets)))
    else:
        print("  [WARN] 未找到本地 wifi_config.h，无法做凭据全文搜索（仅做了路径检查）")

    if problems:
        print("  [FAIL] 校验未通过：")
        for p in problems:
            print("         - " + p)
        return False
    print("  [OK  ] 校验通过：无禁止项、无凭据泄漏")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", default=None)
    ap.add_argument("--docs-only", action="store_true")
    args = ap.parse_args()

    if not git_ok():
        print("!! 当前不是 git 仓库。请先 git init 并提交，"
              "本脚本依赖 git archive 来保证密钥不随包带走。")
        return 2

    name = args.output or (str(ROOT / "submission_docs.zip") if args.docs_only
                           else str(ROOT / "submission_src.zip"))
    out = Path(name)
    if not out.is_absolute():
        out = ROOT / out

    head = run(["git", "log", "--oneline", "-1"]).stdout.strip()
    print("从提交 %s 打包" % head)
    print("目标: %s" % out)

    if not make_archive(out, args.docs_only):
        return 1

    size = out.stat().st_size / 1024.0
    print("  生成成功: %.1f KB" % size)
    ok = verify(out)
    if not ok:
        print("\n!! 已删除不安全的包：%s" % out)
        out.unlink(missing_ok=True)
        return 1
    print("\n可以交付: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
