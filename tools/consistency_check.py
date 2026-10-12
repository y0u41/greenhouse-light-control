#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
consistency_check.py —— 跨层一致性守护（固件 ↔ Qt ↔ 契约 ↔ 文档）

与 contract_test.py 的分工
    contract_test.py 只管"动作词汇"这一件事（get_state / query 类漂移）。
    本脚本管**同一个事实在多处出现时是否一致**：阈值默认值、端口、引脚、
    执行器阶段配置、亮度方向、看门狗超时、文档集版本……这类漂移不会报编译错，
    只会让文档与代码悄悄对不上，正是本项目 V1.0 阶段花力气消除的问题。

用法
    py tools/consistency_check.py          # 全部检查，任一失败则退出码 1
    py tools/consistency_check.py -v       # 打印每条检查的取值
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware" / "Core"
QT = ROOT / "qtapp" / "src"
DOCS = ["PRD.md", "Tech_Design.md", "项目规划书.md", "AGENTS.md", "设计报告.md"]

# IWDG 数值守卫：LSI 标称频率，以及"规范性声明"看门狗超时的文档清单。
LSI_HZ = 40000
# AGENTS.md 与 docs/lessons.md 刻意保留了踩坑当时的旧值（≈2.0 s、RLR=1250）作为
# 历史证据，**不能**参与比对，因此不列入。
# 已知例外：firmware/CUBEMX_SETUP.md 里还有一处过时的 "IWDG ≈2 s"，它位于
# firmware/ 目录、不在本守卫范围（改动 firmware/ 会与"最终提交不得含固件改动"冲突），
# 需要单独跟进。
IWDG_DOCS = ["Tech_Design.md", "设计报告.md", "项目规划书.md", "PRD.md"]
# 文档里"声明看门狗超时"的写法（详见 iwdg_claims 的说明）
IWDG_PERIOD_LINE = re.compile(r"≈\s*(\d+(?:\.\d+)?)\s*s\b")
IWDG_ADJACENT = re.compile(r"IWDG\s*\**\s*≈")
IWDG_RELOAD = re.compile(r"重装值\s*\**\s*(\d+)")

VERBOSE = False
FAILED: list[str] = []


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="ignore")


def check(name: str, ok: bool, detail: str = "") -> None:
    tag = "OK  " if ok else "FAIL"
    print("  [%s] %s%s" % (tag, name, ("  -> " + detail) if (detail and (VERBOSE or not ok)) else ""))
    if not ok:
        FAILED.append(name)


def macro(text: str, name: str, default: str = "") -> str:
    m = re.search(r"#define\s+%s\s+([0-9]+)U?" % re.escape(name), text)
    return m.group(1) if m else default


def parse_iwdg(text: str):
    """从 main.c 解析 IWDG 配置 -> (分频系数, 重装值, 超时秒数)；解析失败返回 None。

    分频系数不取 `IWDG_PRESCALER_<N>` 的字面 N，而是按 RM0008 的合法档位
    `N = 4 × 2^n` 反解 n 再算回 `4 × 2^n`，这样 `IWDG_PRESCALER_50` 之类
    非法档位会被判为解析失败，而不是被当成 50 继续算。
    """
    m = re.search(r"\.Prescaler\s*=\s*IWDG_PRESCALER_(\d+)", text)
    if not m:
        return None
    n_val = int(m.group(1))
    if n_val < 4 or n_val % 4:                 # 必须是 4 的倍数
        return None
    n = (n_val // 4).bit_length() - 1          # n_val/4 必须是 2 的幂
    if n > 6 or (4 << n) != n_val:
        return None
    divider = 4 << n

    m = re.search(r"\.Reload\s*=\s*(\d+)\s*U?", text)
    if not m:
        return None
    reload_val = int(m.group(1))
    if not 0 < reload_val <= 4095:             # IWDG_RLR 只有 12 位
        return None
    return divider, reload_val, reload_val * divider / LSI_HZ


def iwdg_claims(text: str) -> tuple[list[float], list[int]]:
    """逐行收集文档声明的 (溢出秒数列表, 重装值列表)。

    这是本守卫唯一的"读文档"入口，**绝不裸搜数字**。文档里"≈ x s"的写法太常见
    （开机闪灯 `Led0_BlinkMark(2)` ≈ 1.1 s 就写在看门狗那段紧邻的一行里），
    所以周期必须锚在"看门狗语境"上，只认下面两种声明式写法：

      ① 同一行出现 `溢出` 且带 `≈ x s` —— 如 `IWDG 溢出周期 **≈ 6.4 s**`、
         `| IWDG 溢出 | **≈ 6.4 s** |`；
      ② `IWDG` 后紧跟 `≈ x s` —— 如 `IWDG ≈6.4 s`、`IWDG **≈ 6.4 s**`。

    `重装值` 是本项目只用于 IWDG 的专有词，命中即可，不再要求同行语境。
    """
    periods: list[float] = []
    reloads: list[int] = []
    for line in text.splitlines():
        if "溢出" in line or IWDG_ADJACENT.search(line):
            periods += [float(m.group(1))
                        for m in IWDG_PERIOD_LINE.finditer(line)]
        reloads += [int(m.group(1)) for m in IWDG_RELOAD.finditer(line)]
    return periods, reloads


def main() -> None:
    global VERBOSE
    ap = argparse.ArgumentParser(description="跨层一致性守护")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    VERBOSE = args.verbose

    schema = read(ROOT / "tools" / "protocol_schema.py")
    mainh = read(FW / "Inc" / "main.h")
    control_c = read(FW / "Src" / "control.c")
    adc_h = read(FW / "Inc" / "adc_light.h")
    adc_c = read(FW / "Src" / "adc_light.c")
    actions_h = read(QT / "protocol_actions.h")
    config_c = read(QT / "config.cpp")
    esp_c = read(FW / "Src" / "esp8266.c")
    docs = {n: read(ROOT / n) for n in DOCS}
    all_docs = "\n".join(docs.values())

    print("== 1. 动作词汇：契约 / 固件 / Qt 三处必须一致 ==")
    schema_actions = set(re.findall(r'ACTION_[A-Z_]+\s*=\s*"([a-z_]+)"', schema))
    fw_actions = set(re.findall(r'strcmp\(action,\s*"([a-z_]+)"\)', control_c))
    qt_actions = set(re.findall(r'ACTION_[A-Z_]+\s*\[\]\s*=\s*"([a-z_]+)"', actions_h))
    check("protocol_schema 动作集合非空", len(schema_actions) >= 6, str(sorted(schema_actions)))
    check("固件 Control_OnCommand 分支 == 契约", fw_actions == schema_actions,
          "固件多/少: %s" % sorted(fw_actions ^ schema_actions))
    check("Qt protocol_actions.h == 契约", qt_actions == schema_actions,
          "Qt 多/少: %s" % sorted(qt_actions ^ schema_actions))
    check("'query' 不在任何合法集合中", "query" not in schema_actions | fw_actions | qt_actions)
    check("alarm_test 三处都有（PRD 报警测试按钮）", "alarm_test" in schema_actions | fw_actions | qt_actions)

    print("\n== 2. 阈值 / 回差默认值：固件 / 契约 / Qt 一致 ==")
    fw_th, fw_hys = macro(mainh, "TH_DEFAULT"), macro(mainh, "HYS_DEFAULT")
    sc_th = re.search(r"TH_DEFAULT\s*=\s*(\d+)", schema)
    sc_hys = re.search(r"HYS_DEFAULT\s*=\s*(\d+)", schema)
    qt_th = re.search(r"TH_DEFAULT\s*=\s*(\d+)", actions_h)
    qt_hys = re.search(r"HYS_DEFAULT\s*=\s*(\d+)", actions_h)
    check("固件 main.h 有 TH/HYS 默认值", bool(fw_th and fw_hys), "%s / %s" % (fw_th, fw_hys))
    check("契约 TH/HYS == 固件",
          bool(sc_th and sc_th.group(1) == fw_th) and bool(sc_hys and sc_hys.group(1) == fw_hys),
          "契约 %s / %s" % (sc_th.group(1) if sc_th else "?", sc_hys.group(1) if sc_hys else "?"))
    check("Qt TH/HYS == 固件",
          bool(qt_th and qt_th.group(1) == fw_th) and bool(qt_hys and qt_hys.group(1) == fw_hys),
          "Qt %s / %s" % (qt_th.group(1) if qt_th else "?", qt_hys.group(1) if qt_hys else "?"))
    check("代码里无硬编码 150 充当回差（应引用常量）",
          not re.search(r"int\s+hys\s*=\s*150\s*;", read(QT / "commandengine.cpp")))

    print("\n== 3. 端口与监听地址 ==")
    check("Qt tcp/port 默认 9092",
          ('"tcp/port"' in config_c) and ("9092" in config_c))
    check("Qt llm/url 指向 8092", "8092" in config_c)
    check("Qt 默认绑局域网网卡（不可绑回环）", '"0.0.0.0"' in config_c)
    check("文档写明 llama-server 绑回环、Qt 监听局域网",
          ("局域网网卡" in all_docs or "监听局域网" in all_docs))
    check("文档写明端口不可混用", ("端口隔离" in all_docs) or ("不可混用" in all_docs))
    check("固件默认目标端口 9092", "9092" in read(FW / "Inc" / "wifi_config.h.example"))

    print("\n== 4. 执行器（两阶段 + 抽象，业务代码不得直写引脚）==")
    check("control.c 有 ACTUATOR_* 宏", "ACTUATOR_GPIO_PORT" in control_c and "ACTUATOR_ACTIVE_LEVEL" in control_c)
    check("阶段一配置 = LED1/PE5（GPIOE + PIN_5）",
          "GPIOE" in control_c and "GPIO_PIN_5" in control_c)
    check("阶段二配置已备好 = PD11（GPIOD + PIN_11，注释内）",
          "GPIOD" in control_c and "GPIO_PIN_11" in control_c)
    check("有效电平为低（LED1 与低电平触发模块一致）",
          re.search(r"#define\s+ACTUATOR_ACTIVE_LEVEL\s+0", control_c) is not None)
    check("协议层/主流程无 Relay_Set 硬编码残留",
          not re.search(r"\bRelay_(Set|Init)\s*\(", control_c))

    print("\n== 5. 光照采集（引脚 / 通道 / 方向）==")
    check("adc_light.c 用 ADC3 通道 6", "ADC_CHANNEL_6" in adc_c)
    check("adc_light.c 用 GPIOF + PIN_8（PF8）", "GPIOF" in adc_c and "GPIO_PIN_8" in adc_c)
    check("光照方向：light = 4095 - adc", "LIGHT_ADC_MAX - (a)" in adc_h)
    check("文档一致写 PF8 / ADC3_IN6", "PF8" in all_docs and "ADC3_IN6" in all_docs)

    print("\n== 6. 手动优先与链路细节 ==")
    check("固件有 MODE_MANUAL 且 relay_on/off 切手动",
          "MODE_MANUAL" in control_c and control_c.count("g_mode = MODE_MANUAL") >= 2)
    check("固件有 auto 分支", 'strcmp(action, "auto")' in control_c)
    check("ESP8266 透传期只嗅探短前缀（防负载含 CLOSED 误重连）", "AT_LINE_SNIFF_MAX" in esp_c)
    check("ESP8266 不再依赖 wifi_config.h（配置由 Init 参数传入）",
          "wifi_config.h" not in esp_c)

    print("\n== 7. 文档集版本统一 ==")
    vers = {}
    for n, t in docs.items():
        m = re.search(r"[Vv](\d+\.\d+)", t)
        vers[n] = m.group(1) if m else "?"
    check("文档集 + 设计报告 版本号一致", len(set(vers.values())) == 1, str(vers))

    print("\n== 8. 看门狗（IWDG）超时：main.c 解析值 == 文档声明 ==")
    # 这个数字（超时秒数 / 重装值）曾长期潜伏在文档里没人发现，原因就是
    # 没有任何机器看着它。此处**不硬编码**任何数值：一律从 main.c 现场解析，
    # 文档只与解析结果比对；解析不出来就报 FAIL，绝不静默跳过。
    main_c = read(FW / "Src" / "main.c")
    parsed = parse_iwdg(main_c)
    if parsed is None:
        check("IWDG 超时与重装值：main.c 解析值 == 文档声明", False,
              "无法从 main.c 解析 IWDG 配置。期望 MX_IWDG_Init() 里有 "
              "`...Init.Prescaler = IWDG_PRESCALER_<4×2^n>`（合法档位 4~256）与 "
              "`...Init.Reload = <1~4095>U`（RLR 仅 12 位）。"
              "代码结构可能已变，请人工核对后同步本守卫")
    else:
        divider, reload_val, timeout = parsed
        problems: list[str] = []
        n_period = n_reload = 0
        for name in IWDG_DOCS:
            periods, reloads = iwdg_claims(docs[name])
            n_period += len(periods)
            n_reload += len(reloads)
            if not periods:
                problems.append("%s 未声明溢出周期（≈x s）" % name)
            for p in periods:
                if abs(p - timeout) > 0.05:
                    problems.append("%s 声明 ≈%gs，main.c 算出 %gs"
                                    % (name, p, timeout))
            if not reloads:
                problems.append("%s 未声明重装值" % name)
            for r in reloads:
                if r != reload_val:
                    problems.append("%s 声明重装值 %d，main.c 为 %d"
                                    % (name, r, reload_val))
        # 同一处声明在文档里常出现多次（Tech_Design 就有 4 处），去重后
        # 报错信息才不会退化成一长串重复
        problems = list(dict.fromkeys(problems))
        # 通过时也把"到底比对了多少处"打出来：否则无从判断这条守卫是不是
        # "什么都没读到所以永远绿"
        check("IWDG 超时与重装值：main.c 解析值 == 文档声明", not problems,
              "；".join(problems) if problems else
              "main.c: %d 分频 × 重装 %d / LSI %d Hz = %gs；%d 份文档共 %d 处"
              "周期声明 + %d 处重装值声明，全部一致"
              % (divider, reload_val, LSI_HZ, timeout,
                 len(IWDG_DOCS), n_period, n_reload))

    print()
    if FAILED:
        print("一致性检查未通过，共 %d 项漂移：" % len(FAILED))
        for f in FAILED:
            print("  - " + f)
        sys.exit(1)
    print("全部一致性检查通过（固件 / Qt / 契约 / 文档 四处对齐）")


if __name__ == "__main__":
    main()
