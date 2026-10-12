#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fact_inventory.py —— 关键事实清点与「去重收敛」前后比对（P1-1）

背景
    P1-1 的目标是把「同一事实 N 处副本」降到 N=1：技术事实只在 Tech_Design.md
    定义一次，其余文档改为引用。但**删副本的前提是不能丢事实**。本脚本就是
    "不丢事实"的机器证据来源，共三件事：

    1. 清点（snapshot）—— 统计每份文档里各类技术事实的出现次数与去重后的
       token 集合，作为收敛决策与 before/after 的依据；
    2. 比对（compare）—— 断言**关键事实全集在收敛前后完全相等**：
       合并后全集不变 ⇒ 没有任何事实因为"删副本"而从语料里消失；
    3. 审计（audit）—— 对**每一条被删除的行**逐条核对：它包含的事实 token 是否
       都能在 Tech_Design.md 里找到等价表述，输出可贴进报告的核对清单。

用法
    py tools/fact_inventory.py snapshot --out before.json
    py tools/fact_inventory.py compare before.json after.json      # 任一类别全集不等 => 退出码 1
    py tools/fact_inventory.py audit --before <旧文档目录> [--json audit.json]
    py tools/fact_inventory.py report                              # 人读的计数表
    py tools/fact_inventory.py orphans                             # 只在报告/规划书里、Tech_Design 没有的事实

设计原则
    · 只用「抽取 = 事实」的正则，不依赖 Markdown 结构，避免与排版耦合；
    · token 一律用**ASCII 词边界**（`\\b` 在 Python 里把中文也算 word char，
      会把「引脚PF8」整体当成一个词而漏抽，故显式写 (?<![0-9A-Za-z_]) ）；
    · 只比集合、不比次数：次数下降是收敛的目标，集合不变才是零丢失的证明。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE_DOC = "Tech_Design.md"
DOCS = ["Tech_Design.md", "设计报告.md", "项目规划书.md", "PRD.md", "README.md", "AGENTS.md"]
# 「本轮允许改动」的文档：其余文档视为只读参照
EDITABLE = ["Tech_Design.md", "设计报告.md", "项目规划书.md"]

# 生成区/标记语法自身产生的 token，不是技术事实，比对时忽略
NOISE = {"FACT", "BEGIN", "END", "GENERATED", "SOURCE"}

# 已登记的事实「更名」（等号右边的名字在收敛后的语料里必须存在）。
# 这些不是丢失，而是文档里的**旧标识符**被纠正为固件里的真实名字，
# 依据：firmware/Core/Src/main.c、firmware/Core/Inc/esp8266.h、firmware/Core/Src/control.c。
RENAMES: dict[str, str] = {
    "LED0_TOGGLE": "Led0_Toggle",        # main.c：心跳用 Led0_Toggle()
    "led_init": "Led0_Init",             # main.c：初始化用 Led0_Init()
    "beep_init": "Buzzer_Init",          # control.c：蜂鸣器初始化在 Control_Init 内
    "ESP8266_Init": "Esp8266_Init",      # esp8266.h：void Esp8266_Init(...)
}

# ---- token 抽取 ----------------------------------------------------------

_MODULE_IDENTS = (
    r"NodeServer|NodeModel|ChartWidget|ControlPanel|TtsService|AsrService|LlmClient|"
    r"CommandEngine|CommandAckGuard|protocol_schema|protocol_actions|config\.cpp|"
    r"Actuator_Init|Actuator_Set|Control_ApplyOutput|Control_Apply|Control_Init|"
    r"Control_Loop|Control_AutoTarget|Control_OnCommand|Light_ReadRaw|LightSensor_Init|"
    r"Light_IsValid|EEPROM_Load|EEPROM_Save|Esp8266_Init|ESP8266_Init|Esp8266_Poll|"
    r"Esp8266_IsConnected|Led0_Init|Led0_Toggle|Led0_BlinkMark|Buzzer_Init|Buzzer_Set|"
    r"Iwdg_Feed|LcdShow_Init|Protocol_SendReport|Protocol_SendAck|"
    r"Protocol_SendEvent|Protocol_PopCommand|Protocol_ParseAndExecute|LcdShow_Update|"
    r"lcd_init|led_init|beep_init|HAL_GetTick|HAL_Delay|QNetworkAccessManager|"
    r"QTextToSpeech|QThread|QTcpServer|QSettings|QChart|QProcess|QEventLoop|"
    r"availableVoices|setTransferTimeout|chat_template_kwargs|enable_thinking|"
    r"models-max|models-dir|api-key|adc3_get_result|adc3_get_result_average|"
    r"lsens_get_val|lsens_init|adc3_init|Ring_Push|Ring_Pop|DictationGrammar|"
    r"SpeechRecognizer|AutoMoc|uvprojx|uvoptx"
)

_FILES = (
    r"md|py|ps1|c|h|cpp|json|hex|map|uvprojx|uvoptx|gguf|cmd|txt|docx"
)

# 类别 -> 正则。每类抽出的 token 的**并集**就是"关键事实全集"。
CATEGORIES: dict[str, str] = {
    # 引脚：PF8 / PA1 / PB8 / PD11 / PE5 ...
    "pin": r"P[A-G](?:1[0-5]|[0-9])",
    # 引脚宏：GPIO_PIN_5 / GPIO_PIN_11
    "gpio_macro": r"GPIO_PIN_[0-9]+",
    # 端口：9092 / 8092
    "port": r"(?:8092|9092)",
    # 协议动作名（契约词汇）
    "action": r"relay_on|relay_off|auto|set_th|get_state|alarm_test|query",
    # 帧类型
    "msg_type": r"report|ack|evt|cmd",
    # 主动事件
    "event": r"alarm|recover",
    # 全大写符号/缩写：TH_DEFAULT / LIGHT_FROM_ADC / ADC3_IN6 / IWDG / FSMC / MD5 ...
    "symbol": r"[A-Z][A-Z0-9_]{2,}",
    # 代码标识符与类名
    "identifier": _MODULE_IDENTS,
    # 模型名
    "model": r"glm-4\.7-flash|qwen3\.8-27b-turbo|nomic-embed",
    # 中文语音名
    "voice": r"Microsoft (?:Huihui Desktop|Huihui|Kangkang|Yaoyao|Xiaoxiao)",
    # 文件名（含中文名）
    "file": r"[0-9A-Za-z_\u4e00-\u9fff./\\-]+\.(?:" + _FILES + r")",
    # 数值事实（阈值、周期、尺寸……）
    "num": r"[0-9]+(?:\.[0-9]+)?",
}


def _compile() -> dict[str, re.Pattern[str]]:
    pats = {}
    for cat, body in CATEGORIES.items():
        pats[cat] = re.compile(r"(?<![0-9A-Za-z_])(" + body + r")(?![0-9A-Za-z_])")
    return pats


PATS = _compile()


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def _strip_heading_numbers(text: str) -> str:
    """抽取前把「### 5.7 标题」里的**章节号**去掉。

    章节号不是技术事实（"5.7" 只是排版编号），但它长得像数值事实，会在
    before/after 比对里制造假阳性。去掉后 num 类别只反映真正的数值。
    """
    return re.sub(r"(?m)^(#{1,6}[ \t]+)[0-9]+(?:\.[0-9]+)*\.?[ \t]+", r"\1", text)


def extract(text: str) -> dict[str, Counter[str]]:
    """返回 {类别: Counter{token: 出现次数}}"""
    text = _strip_heading_numbers(text)
    out: dict[str, Counter[str]] = {}
    for cat, pat in PATS.items():
        c: Counter[str] = Counter(m.group(1) for m in pat.finditer(text))
        for k in [k for k in c if cat == "symbol" and k in NOISE]:
            del c[k]
        out[cat] = c
    return out


def fact_tokens(text: str) -> set[str]:
    """一行/一段文本里的全部事实 token（跨类别合并，带类别前缀以便追溯）"""
    text = _strip_heading_numbers(text)
    toks: set[str] = set()
    for cat, pat in PATS.items():
        for m in pat.finditer(text):
            if cat == "symbol" and m.group(1) in NOISE:
                continue
            toks.add("%s:%s" % (cat, m.group(1)))
    return toks


# ---- snapshot ------------------------------------------------------------


def heading_numbers(text: str) -> set[str]:
    """Markdown 标题里的章节号（"### 5.7 xxx" -> "5.7"）。

    章节号是排版编号而非技术事实；它同时会被正文交叉引用（"见 5.7"），
    因此比对 num 类别时要把两侧的章节号一并剔除，避免假阳性。
    """
    return set(re.findall(r"(?m)^#{1,6}[ \t]+([0-9]+(?:\.[0-9]+)*)", text))


def snapshot(root: Path, docs: list[str]) -> dict:
    data: dict = {"root": str(root), "docs": {}, "total_bytes": 0, "heading_numbers": []}
    for name in docs:
        p = root / name
        if not p.is_file():
            continue
        text = read(p)
        ext = extract(text)
        data["docs"][name] = {
            "bytes": p.stat().st_size,
            "lines": text.count("\n") + 1,
            "counts": {cat: sum(c.values()) for cat, c in ext.items()},
            "distinct": {cat: len(c) for cat, c in ext.items()},
            "tokens": {cat: sorted(c) for cat, c in ext.items()},
            "occurrences": {cat: dict(sorted(c.items())) for cat, c in ext.items()},
        }
        data["total_bytes"] += p.stat().st_size
        data["heading_numbers"] = sorted(set(data["heading_numbers"]) | heading_numbers(text))
    return data


def union_tokens(snap: dict, cat: str, docs: list[str] | None = None) -> set[str]:
    docs = docs or list(snap["docs"])
    u: set[str] = set()
    for d in docs:
        if d in snap["docs"]:
            u |= set(snap["docs"][d]["tokens"].get(cat, []))
    return u


def cmd_snapshot(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve() if args.root else ROOT
    snap = snapshot(root, args.docs or DOCS)
    out = Path(args.out)
    out.write_text(json.dumps(snap, ensure_ascii=False, indent=1), encoding="utf-8")
    print("已写入快照：%s" % out)
    print("  文档 %d 份，合计 %d B（%.1f KB）"
          % (len(snap["docs"]), snap["total_bytes"], snap["total_bytes"] / 1024))
    for name, d in snap["docs"].items():
        print("  %-16s %7d B  %5d 行  %5d 个事实 token"
              % (name, d["bytes"], d["lines"],
                 len(set().union(*[set(v) for v in d["tokens"].values()]) if d["tokens"] else set())))
    return 0


# ---- compare -------------------------------------------------------------


def cmd_compare(args: argparse.Namespace) -> int:
    a = json.loads(Path(args.before).read_text(encoding="utf-8"))
    b = json.loads(Path(args.after).read_text(encoding="utf-8"))
    docs = [d for d in a["docs"] if d in b["docs"]]
    rc = 0

    print("=" * 78)
    print("一、关键事实全集（各类别 并集）before vs after —— 必须完全相等")
    print("=" * 78)
    print("%-12s %8s %8s  %s" % ("类别", "before", "after", "结论"))
    # 章节号不是事实：两侧的标题编号都从 num 类别里剔除
    skip = set(a.get("heading_numbers", [])) | set(b.get("heading_numbers", []))
    after_all = {t for c in CATEGORIES for t in union_tokens(b, c, docs)}
    renamed: list[str] = []
    for cat in CATEGORIES:
        ua, ub = union_tokens(a, cat, docs), union_tokens(b, cat, docs)
        if cat == "num":
            ua, ub = ua - skip, ub - skip
        added = ub - ua
        lost = ua - ub
        # 已登记的更名：旧名消失但新名存在 -> 记为"更名"而不是"丢失"
        for tok in sorted(lost):
            new = RENAMES.get(tok)
            if new and new in after_all:
                lost.discard(tok)
                renamed.append("%s -> %s" % (tok, new))
        if not lost:
            note = ""
            if renamed:
                note = "  （含已登记更名：%s）" % "；".join(renamed)
                renamed = []          # 只提示一次
            if added:
                note += "  （新增 %d 个事实：%s）" % (len(added), ", ".join(sorted(added)))
            print("%-12s %8d %8d  OK 无丢失%s" % (cat, len(ua), len(ub), note))
        else:
            rc = 1
            print("%-12s %8d %8d  FAIL 丢失 %d / 新增 %d"
                  % (cat, len(ua), len(ub), len(lost), len(added)))
            for t in sorted(lost):
                print("      - 丢失 %s" % t)
            for t in sorted(added):
                print("      + 新增 %s" % t)

    ua_all = set().union(*[union_tokens(a, c, docs) for c in CATEGORIES]) - skip
    ub_all = set().union(*[union_tokens(b, c, docs) for c in CATEGORIES]) - skip
    lost_all = {t for t in (ua_all - ub_all) if t.split(":", 1)[-1] not in RENAMES}
    print()
    print("全部类别合并：before %d 个事实 token / after %d 个 -> %s"
          % (len(ua_all), len(ub_all),
             "OK 全集不变（零丢失）" if not lost_all else "FAIL 全集发生变化"))
    if renamed:
        print("已登记更名（旧标识符纠正为固件实现名，非丢失）：%s" % "；".join(renamed))

    print()
    print("=" * 78)
    print("二、事实出现总次数（收敛效果：应显著下降，全集不变）")
    print("=" * 78)
    print("%-12s %10s %10s %8s" % ("类别", "before", "after", "减少"))
    for cat in CATEGORIES:
        ca = sum(a["docs"][d]["counts"].get(cat, 0) for d in docs)
        cb = sum(b["docs"][d]["counts"].get(cat, 0) for d in docs)
        print("%-12s %10d %10d %8d" % (cat, ca, cb, ca - cb))
    ta = sum(sum(a["docs"][d]["counts"].values()) for d in docs)
    tb = sum(sum(b["docs"][d]["counts"].values()) for d in docs)
    print("%-12s %10d %10d %8d" % ("合计", ta, tb, ta - tb))

    print()
    print("=" * 78)
    print("三、文档体积 before vs after")
    print("=" * 78)
    print("%-16s %10s %10s %9s" % ("文档", "before", "after", "变化"))
    for d in docs:
        ba, bb = a["docs"][d]["bytes"], b["docs"][d]["bytes"]
        print("%-16s %8d B %8d B %+9d" % (d, ba, bb, bb - ba))
    print("%-16s %8d B %8d B %+9d"
          % ("合计", a["total_bytes"], b["total_bytes"], b["total_bytes"] - a["total_bytes"]))
    print("（%.1f KB -> %.1f KB）" % (a["total_bytes"] / 1024, b["total_bytes"] / 1024))

    print()
    print("=" * 78)
    print("四、重点事实的**字面**出现次数 before vs after（按类别取最大值 = 字面次数）")
    print("=" * 78)
    for tok in ["9092", "PF8"]:
        def lit(snap: dict, doc: str) -> int:
            return max([snap["docs"][doc]["occurrences"].get(c, {}).get(tok, 0)
                        for c in CATEGORIES] or [0])

        ra = sum(lit(a, d) for d in docs)
        rb = sum(lit(b, d) for d in docs)
        print("%-8s 合计 %3d -> %3d 次" % (tok, ra, rb))
        for d in docs:
            xa, xb = lit(a, d), lit(b, d)
            if xa or xb:
                print("      %-16s %3d -> %3d" % (d, xa, xb))

    print()
    print("结论：%s" % ("关键事实全集不变，零丢失（PASS）" if rc == 0 else "存在事实丢失（FAIL）"))
    return rc


# ---- audit ---------------------------------------------------------------


def _norm(line: str) -> str:
    return re.sub(r"\s+", " ", line.strip())


def cmd_audit(args: argparse.Namespace) -> int:
    before_root = Path(args.before).resolve()
    after_root = Path(args.after).resolve() if args.after else ROOT
    src_after = read(after_root / SOURCE_DOC)
    src_tokens = fact_tokens(src_after)
    # 其余文档（收敛后）也作为"事实仍然存在"的落点：按分工，实测数据归报告、计划论证归规划书
    other: dict[str, set[str]] = {}
    for name in DOCS:
        if name == SOURCE_DOC:
            continue
        p = after_root / name
        if p.is_file():
            other[name] = fact_tokens(read(p))
    # 章节号不是事实：两侧标题编号都视为结构性编号
    structural = set()
    for root_ in (before_root, after_root):
        for name in DOCS:
            p = root_ / name
            if p.is_file():
                structural |= {"num:%s" % n for n in heading_numbers(read(p))}

    rows: list[dict] = []
    for name in (args.docs or DOCS):
        old_p, new_p = before_root / name, after_root / name
        if not old_p.is_file() or not new_p.is_file():
            continue
        old_lines = read(old_p).splitlines()
        new_set = {_norm(l) for l in read(new_p).splitlines() if _norm(l)}
        for i, raw in enumerate(old_lines, 1):
            n = _norm(raw)
            if not n or n in new_set:
                continue
            toks = fact_tokens(raw)
            if not toks:
                rows.append({"doc": name, "line": i, "text": n, "tokens": [],
                             "missing": [], "kind": "非事实行", "where": {}})
                continue
            missing, where = [], {}
            for t in sorted(toks):
                if t in src_tokens:
                    continue
                hit = [d for d, s in other.items() if t in s]
                if hit:
                    where[t] = hit
                elif t in structural:
                    where[t] = ["(章节编号)"]
                elif t.split(":", 1)[-1] in RENAMES:
                    where[t] = ["(已登记更名 -> %s)" % RENAMES[t.split(":", 1)[-1]]]
                else:
                    missing.append(t)
            rows.append({"doc": name, "line": i, "text": n,
                         "tokens": sorted(toks), "missing": missing, "where": where,
                         "kind": "Tech_Design 已有" if not where and not missing
                                 else ("其他文档承接" if not missing else "需人工核对")})

    print("=" * 78)
    print("删除行审计：被删除的每一行，它的事实去哪了")
    print("=" * 78)
    by_doc: dict[str, Counter] = {}
    for r in rows:
        by_doc.setdefault(r["doc"], Counter())[r["kind"]] += 1
    for d, c in by_doc.items():
        print("%-16s 共删除 %3d 行：非事实行 %d，%s 已在内 %d，其他文档承接 %d，需人工核对 %d"
              % (d, sum(c.values()), c["非事实行"], SOURCE_DOC,
                 c["Tech_Design 已有"], c["其他文档承接"], c["需人工核对"]))
    print()

    carry = [r for r in rows if r["kind"] == "其他文档承接"]
    if carry:
        print("-" * 78)
        print("事实由其他文档承接（同一事实仅存的落点，非丢失）")
        print("-" * 78)
        for r in carry:
            print("%s:%d  %s" % (r["doc"], r["line"], r["text"][:90]))
            for t, hit in r["where"].items():
                print("        %-22s -> %s" % (t, ", ".join(hit)))

    need = [r for r in rows if r["kind"] == "需人工核对"]
    print()
    if need:
        print("-" * 78)
        print("需人工核对的删除行（事实在任何文档里都找不到）")
        print("-" * 78)
        for r in need:
            print("%s:%d  %s" % (r["doc"], r["line"], r["text"][:110]))
            print("        未找到: %s" % ", ".join(r["missing"]))
    else:
        print("需人工核对：0 行 —— 所有被删除行的事实，要么在 %s 里已有等价表述，"
              "要么由其他文档按分工承接（或属于章节编号 / 已登记更名）。" % SOURCE_DOC)

    if args.json:
        Path(args.json).write_text(json.dumps(rows, ensure_ascii=False, indent=1),
                                   encoding="utf-8")
        print("\n明细已写入：%s" % args.json)
    return 0 if not need else 1


# ---- orphans -------------------------------------------------------------


def cmd_orphans(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve() if args.root else ROOT
    snap = snapshot(root, DOCS)
    src = snap["docs"].get(SOURCE_DOC)
    if not src:
        print("找不到 %s" % SOURCE_DOC)
        return 2
    src_all: set[str] = set()
    for cat in CATEGORIES:
        src_all |= {"%s:%s" % (cat, t) for t in src["tokens"].get(cat, [])}

    print("只在其他文档出现、%s 里没有的事实 token（孤本事实）：" % SOURCE_DOC)
    total = 0
    for d in DOCS:
        if d == SOURCE_DOC or d not in snap["docs"]:
            continue
        own: set[str] = set()
        for cat in CATEGORIES:
            own |= {"%s:%s" % (cat, t) for t in snap["docs"][d]["tokens"].get(cat, [])}
        only = own - src_all
        total += len(only)
        print("\n  [%s] %d 个" % (d, len(only)))
        for t in sorted(only):
            print("    " + t)
    if total == 0:
        print("\n（无孤本事实：这三份文档里的技术事实在 %s 中均有定义）" % SOURCE_DOC)
    return 0


# ---- report --------------------------------------------------------------


def cmd_report(args: argparse.Namespace) -> int:
    root = Path(args.root).resolve() if args.root else ROOT
    snap = snapshot(root, DOCS)
    print("=" * 78)
    print("文档事实清点（root=%s）" % root)
    print("=" * 78)
    print("%-16s %9s %6s %s" % ("文档", "字节", "行数", "各类事实出现次数"))
    for name, d in snap["docs"].items():
        print("%-16s %9d %6d  %s" % (name, d["bytes"], d["lines"],
                                     " ".join("%s=%d" % (c, d["counts"][c]) for c in CATEGORIES)))
    print("%-16s %9d" % ("合计", snap["total_bytes"]))

    print()
    print("=" * 78)
    print("重复最严重的事实（跨这三份文档的副本次数）")
    print("=" * 78)
    hits: list[tuple[int, int, str]] = []
    for cat in CATEGORIES:
        keys: set[str] = set()
        for d in EDITABLE:
            keys |= set(snap["docs"][d]["tokens"].get(cat, []))
        for k in keys:
            total = 0
            ndocs = 0
            for d in EDITABLE:
                c = snap["docs"][d]["occurrences"][cat].get(k, 0)
                total += c
                ndocs += 1 if c else 0
            if ndocs >= 2:
                hits.append((total, ndocs, "%s:%s" % (cat, k)))
    hits.sort(reverse=True)
    print("%-28s %8s %8s" % ("事实", "总次数", "出现文档数"))
    for total, ndocs, key in hits[:30]:
        print("%-28s %8d %8d" % (key, total, ndocs))
    print("\n跨 2 份以上文档出现的事实 token 共 %d 个" % len(hits))
    return 0


# ---- main ----------------------------------------------------------------


class _Tee:
    """把 stdout 同时写进 UTF-8 文件 —— 便于把证据原样贴进报告。

    不用 PowerShell 管道转存：控制台代码页 936 会把 Python 的 UTF-8 输出
    按 GBK 解码，中文全部变成替换字符（显示问题，但会让证据不可读）。
    """

    def __init__(self, path: str) -> None:
        self.f = open(path, "w", encoding="utf-8", newline="\n")

    def write(self, s: str) -> int:
        sys.__stdout__.write(s)
        self.f.write(s)
        return len(s)

    def flush(self) -> None:
        sys.__stdout__.flush()
        self.f.flush()


def main() -> int:
    ap = argparse.ArgumentParser(description="关键事实清点 / 收敛前后比对")
    ap.add_argument("--tee", default=None,
                    help="把输出（UTF-8）同时写进该文件，便于留存证据")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("snapshot")
    p.add_argument("--root", default=None)
    p.add_argument("--out", required=True)
    p.add_argument("--docs", nargs="*", default=None)
    p.set_defaults(func=cmd_snapshot)

    p = sub.add_parser("compare")
    p.add_argument("before")
    p.add_argument("after")
    p.set_defaults(func=cmd_compare)

    p = sub.add_parser("audit")
    p.add_argument("--before", required=True)
    p.add_argument("--after", default=None)
    p.add_argument("--docs", nargs="*", default=None)
    p.add_argument("--json", default=None)
    p.set_defaults(func=cmd_audit)

    p = sub.add_parser("orphans")
    p.add_argument("--root", default=None)
    p.set_defaults(func=cmd_orphans)

    p = sub.add_parser("report")
    p.add_argument("--root", default=None)
    p.set_defaults(func=cmd_report)

    args = ap.parse_args()
    if args.tee:
        sys.stdout = _Tee(args.tee)  # type: ignore[assignment]
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
