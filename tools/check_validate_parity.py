# tools/check_validate_parity.py
# -*- coding: utf-8 -*-
"""跨语言一致性守卫：把 tools/validate_cases.json 的同一张用例表喂给
protocol_schema.validate_cmd，产出"Python 侧真实判定"快照，供 Qt 侧比对。

背景：commandengine.h 顶部承诺"校验规则与 tools/protocol_schema.py::validate_cmd
完全一致"，但此前没有任何机制在强制它。qtapp/tests/test_commandengine.cpp 会用
<b>同一张用例表</b>跑 C++ 的 CommandEngine::validate，再与本脚本产出的快照逐条比对
（含 why 文本）。任一侧改规则、本脚本未重跑，Qt 测试会显式报出快照过期。

两种用法：

1) 生成快照（改了 validate_cases.json 或 protocol_schema.py 后必须重跑）：

       python tools/check_validate_parity.py --emit-json
       python tools/check_validate_parity.py --write      # 直接写回 --snapshot 指定的文件

2) 比对模式（给人看的 diff，不改任何东西，始终退出码 0）：

       python tools/check_validate_parity.py tools/validate_cases.json

退出码约定：
    --emit-json / --write  -> 0 成功，1 用例表或 schema 加载失败
    比对模式               -> 恒为 0（纯报告；不一致只打印清单）

注意：本脚本 <b>不修改</b> protocol_schema.py，只读它。发现不一致时如实报告。
"""

import argparse
import io
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import protocol_schema as schema  # noqa: E402  (需要上面的 sys.path 注入)

DEFAULT_CASES = os.path.join(_HERE, "validate_cases.json")
DEFAULT_SNAPSHOT = os.path.join(_HERE, "validate_parity_python.json")

SNAPSHOT_FORMAT = 1


def _load_cases(path):
    """读用例表。返回 (cases: list, meta: dict)。"""
    with io.open(path, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    if not isinstance(doc, dict) or "cases" not in doc:
        raise ValueError("%s 不是合法的用例表（缺少 cases 数组）" % path)
    cases = doc["cases"]
    if not isinstance(cases, list) or not cases:
        raise ValueError("%s 的 cases 为空" % path)
    for c in cases:
        for key in ("id", "cmd", "hys", "expect"):
            if key not in c:
                raise ValueError("用例缺少字段 %s: %r" % (key, c.get("id")))
    ids = [c["id"] for c in cases]
    if len(ids) != len(set(ids)):
        raise ValueError("用例 id 有重复：%s" % sorted({i for i in ids if ids.count(i) > 1}))
    return cases, {"source": os.path.basename(path), "count": len(cases)}


def run_cases(cases):
    """对每条用例调用协议单一事实源的 validate_cmd，返回结果列表。"""
    results = []
    for c in cases:
        ok, why = schema.validate_cmd(c["cmd"], int(c["hys"]))
        results.append({
            "id": c["id"],
            "expect": bool(c["expect"]),
            "actual": bool(ok),
            "why": why,
            "note": c.get("note", ""),
            "rule": c.get("rule", ""),
        })
    return results


def build_snapshot(cases_path):
    cases, meta = _load_cases(cases_path)
    results = run_cases(cases)
    return {
        "_comment": [
            "由 tools/check_validate_parity.py --write 生成，请勿手改。",
            "record 是 protocol_schema.validate_cmd 对 tools/validate_cases.json 的真实判定，",
            "qtapp/tests/test_commandengine.cpp 会读它并与 C++ CommandEngine::validate 逐条比对。",
        ],
        "format": SNAPSHOT_FORMAT,
        "cases_source": meta["source"],
        "case_count": meta["count"],
        "hys_default": schema.HYS_DEFAULT,
        "adc_max": schema.ADC_MAX,
        "records": results,
    }


def _report(cases_path):
    """比对模式：打印 P1（判定不一致）与 P2（判定相同但 why 文本不同）清单。"""
    snap = build_snapshot(cases_path)
    records = snap["records"]

    verdict_mismatch = [r for r in records if r["actual"] != r["expect"]]
    python_vs_table = [r for r in records if r["actual"] != r["expect"]]

    # 本文件只能看到 Python 一侧；"两侧判定差异"由 Qt 测试负责报出。
    # 这里能报的是：Python 自己的判定 vs 用例表登记的 expect（= C++ 现行语义）。
    print("用例数: %d   (source: %s, schema hys_default=%d, adc_max=%d)"
          % (snap["case_count"], snap["cases_source"], snap["hys_default"], snap["adc_max"]))

    if python_vs_table:
        print("")
        print("!! Python validate_cmd 判定 与 用例表 expect（= C++ CommandEngine::validate 现行语义）"
              "不一致：%d 条" % len(python_vs_table))
        for r in python_vs_table:
            print("   - %-42s expect=%s  python=%s   why=%r"
                  % (r["id"], r["expect"], r["actual"], r["why"]))
            print("     note: %s   rule: %s" % (r["note"], r["rule"]))
    else:
        print("Python validate_cmd 判定与用例表 expect 逐条一致（%d/%d）。"
              % (snap["case_count"], snap["case_count"]))

    print("")
    print("提示：两侧的 why 文本差异由 qtapp/tests/test_commandengine.cpp 的 "
          "parity_* 用例负责报出（Qt 侧会调本脚本的 --emit-json）。")
    print("      重跑快照： python tools/check_validate_parity.py --write")
    return 0


def main(argv=None):
    # Windows 默认 stdout 编码是本地代码页（cp936），会把中文 why 写成非法 UTF-8 字节，
    # 导致 Qt 侧 fromJson 报 "invalid UTF8 string"。这里强制 UTF-8。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):  # 老解释器 / 被重定向成非 TextIOWrapper
            pass

    ap = argparse.ArgumentParser(
        description="CommandEngine::validate 与 protocol_schema.validate_cmd 的一致性守卫")
    ap.add_argument("cases", nargs="?", default=DEFAULT_CASES,
                    help="用例表 JSON（默认 tools/validate_cases.json）")
    ap.add_argument("--emit-json", action="store_true",
                    help="把 Python 侧真实判定快照打到 stdout（供 Qt 测试读取）")
    ap.add_argument("--write", action="store_true",
                    help="把快照写回 --snapshot 指定的文件（默认 tools/validate_parity_python.json）")
    ap.add_argument("--snapshot", default=DEFAULT_SNAPSHOT,
                    help="快照文件路径")
    ap.add_argument("--self-check", action="store_true",
                    help="额外跑一遍 protocol_schema 自带不变式")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.cases):
        sys.stderr.write("找不到用例表: %s\n" % args.cases)
        return 1

    if args.self_check:
        assert schema.is_valid_action(schema.ACTION_GET_STATE)
        assert not schema.is_valid_action("query")
        assert schema.validate_cmd({"action": "relay_on"})[0]
        assert not schema.validate_cmd({"action": "set_th", "value": 100})[0]
        assert not schema.validate_cmd({"action": "set_th", "value": 5000})[0]
        assert schema.validate_cmd({"action": "set_th", "value": 2500})[0]

    try:
        snap = build_snapshot(args.cases)
    except Exception as exc:  # noqa: BLE001 - 命令行工具，报清楚就够
        sys.stderr.write("用例表/schema 加载失败: %s\n" % exc)
        return 1

    if args.emit_json:
        # 必须 UTF-8 且不转义非 ASCII：Qt 侧按 UTF-8 解码
        text = json.dumps(snap, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        sys.stdout.write(text)
        sys.stdout.flush()
        return 0

    if args.write:
        text = json.dumps(snap, ensure_ascii=False, indent=2, sort_keys=True)
        with io.open(args.snapshot, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text + "\n")
        print("已写入快照: %s  (%d 条用例)" % (args.snapshot, snap["case_count"]))
        return 0

    if args.snapshot != DEFAULT_SNAPSHOT:
        # 指定了 --snapshot 但没给动作，视为"只读报告"
        return _report(args.cases)

    return _report(args.cases)


if __name__ == "__main__":
    sys.exit(main())
