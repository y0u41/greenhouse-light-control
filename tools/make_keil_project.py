#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_keil_project.py —— 由官方模板生成 Keil MDK 工程（greenhouse.uvprojx + uvoptx）

为什么要脚本生成而不是手工维护工程文件
    官方模板（_template_atk_f103.uvprojx）是按"实验20 光敏传感器"组织的，
    源文件清单与本项目不同，且**缺少两个必需的 HAL 模块**：
      · stm32f1xx_hal_i2c.c   —— AT24C02 走 I2C1
      · stm32f1xx_hal_iwdg.c  —— 独立看门狗
    手工在 Keil 里增删文件极容易漏，且无法复现。本脚本把"分组 / 包含路径 /
    宏定义 / 调试器"全部固化，任何一次改动都能一键重放。

同时生成 uvoptx
    **调试器选择只存在于 .uvoptx（用户选项），不在 .uvprojx（工程文件）**。
    只给 uvprojx 时 Keil 会回退到模板默认的 ULINK2（BIN\\UL2CM3.DLL），
    本机是 ST-Link，于是报：
        Internal DLL Error
        Error: Flash Download failed - Target DLL has been cancelled
    所以这里一并生成 greenhouse.uvoptx 并锁定为 ST-Link。

用法
    py tools/make_keil_project.py              # 生成工程（发布态）
    py tools/make_keil_project.py --check      # 只校验被引用文件是否存在
    py tools/make_keil_project.py --debug      # 追加 -DDEBUG_UART=1（上板诊断）

退出码
    0 = 引用齐全；1 = 有被引用文件/包含路径缺失（**不论是否带 --check**）。
    --check 只决定"校验完是否继续生成"，不决定是否报错 —— 详见 main()。

    注：原来的 --safeblink（-DSAFE_BLINK_TEST=1，只闪灯自检）已随 main.c 里那套
    被编译掉的备用 main() 一并移除；分诊"没进 main() / 进 main 后挂掉"请依赖
    main() 开头的裸寄存器三次闪灯，详见 docs/lessons.md#semihosting。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from xml.etree import ElementTree as ET

HERE = Path(__file__).resolve().parent
FW = HERE.parent / "firmware"
MDK = FW / "Projects" / "MDK-ARM"
TEMPLATE = MDK / "_template_atk_f103.uvprojx"
OUTPUT = MDK / "greenhouse.uvprojx"
UVOPTX_TEMPLATE = MDK / "_template_atk_f103.uvoptx"
UVOPTX_OUTPUT = MDK / "greenhouse.uvoptx"

TARGET_NAME = "Greenhouse"
OUTPUT_NAME = "greenhouse"

# 编译宏：STM32F103xE = 高密度 512K（ZET6）；USE_HAL_DRIVER 为 HAL 前提
DEFINES = "USE_HAL_DRIVER,STM32F103xE"
DEBUG_DEFINE = "DEBUG_UART=1"
DEBUG_BUILD = False

# ST-Link 在 Keil 用户选项里的标识
NTESL_STLINK = 6
PMON_STLINK = r"STLink\ST-LINKIII-KEIL_SWO.dll"

# 包含路径。Drivers 根目录必须在列：BSP/SYSTEM 里的头文件用 "./BSP/LCD/lcd.h"
# 这类相对 Drivers 的写法；BSP/LCD、SYSTEM/delay、SYSTEM/sys 单独列出，
# 以支持 "#include \"lcd.h\"" / "#include \"delay.h\"" 这类直接写法。
INCLUDE_PATHS = [
    r"..\..\Core\Inc",
    r"..\..\Middlewares",
    r"..\..\Middlewares\cJSON",
    r"..\..\Drivers",
    r"..\..\Drivers\BSP\LCD",
    r"..\..\Drivers\SYSTEM\delay",
    r"..\..\Drivers\SYSTEM\sys",
    r"..\..\Drivers\STM32F1xx_HAL_Driver\Inc",
    r"..\..\Drivers\CMSIS\Device\ST\STM32F1xx\Include",
    r"..\..\Drivers\CMSIS\Include",
]

GROUPS = [
    ("Startup", [
        ("startup_stm32f103xe.s", r"startup_stm32f103xe.s", 2),
    ]),
    ("Core", [
        ("main.c", r"..\..\Core\Src\main.c", 1),
        ("adc_light.c", r"..\..\Core\Src\adc_light.c", 1),
        ("control.c", r"..\..\Core\Src\control.c", 1),
        ("eeprom.c", r"..\..\Core\Src\eeprom.c", 1),
        ("esp8266.c", r"..\..\Core\Src\esp8266.c", 1),
        ("lcd_show.c", r"..\..\Core\Src\lcd_show.c", 1),
        ("debug_uart.c", r"..\..\Core\Src\debug_uart.c", 1),
        ("retarget.c", r"..\..\Core\Src\retarget.c", 1),
        ("protocol.c", r"..\..\Core\Src\protocol.c", 1),
        ("stm32f1xx_it.c", r"..\..\Core\Src\stm32f1xx_it.c", 1),
        ("system_stm32f1xx.c", r"..\..\Core\Src\system_stm32f1xx.c", 1),
    ]),
    ("Drivers/SYSTEM", [
        ("delay.c", r"..\..\Drivers\SYSTEM\delay\delay.c", 1),
        ("sys.c", r"..\..\Drivers\SYSTEM\sys\sys.c", 1),
    ]),
    ("Drivers/BSP", [
        ("lcd.c", r"..\..\Drivers\BSP\LCD\lcd.c", 1),
    ]),
    ("Drivers/STM32F1xx_HAL_Driver", [
        ("stm32f1xx_hal.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal.c", 1),
        ("stm32f1xx_hal_cortex.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_cortex.c", 1),
        ("stm32f1xx_hal_dma.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_dma.c", 1),
        ("stm32f1xx_hal_gpio.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_gpio.c", 1),
        ("stm32f1xx_hal_gpio_ex.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_gpio_ex.c", 1),
        ("stm32f1xx_hal_pwr.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_pwr.c", 1),
        ("stm32f1xx_hal_rcc.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_rcc.c", 1),
        ("stm32f1xx_hal_rcc_ex.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_rcc_ex.c", 1),
        ("stm32f1xx_hal_uart.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_uart.c", 1),
        ("stm32f1xx_hal_i2c.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_i2c.c", 1),
        ("stm32f1xx_hal_iwdg.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_iwdg.c", 1),
        ("stm32f1xx_hal_adc.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_adc.c", 1),
        ("stm32f1xx_hal_adc_ex.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_adc_ex.c", 1),
        ("stm32f1xx_hal_sram.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_hal_sram.c", 1),
        ("stm32f1xx_ll_fsmc.c", r"..\..\Drivers\STM32F1xx_HAL_Driver\Src\stm32f1xx_ll_fsmc.c", 1),
    ]),
    ("Middlewares", [
        ("cJSON.c", r"..\..\Middlewares\cJSON\cJSON.c", 1),
    ]),
]

STARTUP_ENTRY = ("startup_stm32f103xe.s", r"startup_stm32f103xe.s", 2)


def _sub(parent: ET.Element, tag: str, text: str) -> None:
    el = parent.find(tag)
    if el is None:
        el = ET.SubElement(parent, tag)
    el.text = text


def _verify() -> list[str]:
    """校验 GROUPS 与 INCLUDE_PATHS 引用的文件/目录是否真实存在。"""
    missing: list[str] = []
    for inc in INCLUDE_PATHS:
        p = (MDK / inc).resolve()
        if not p.is_dir():
            missing.append("包含路径不存在: %s" % inc)
    for gname, files in GROUPS:
        for name, rel, _t in files:
            p = (MDK / rel).resolve()
            if not p.is_file():
                missing.append("%s  (%s)" % (name, rel))
    return missing


def generate() -> None:
    if not TEMPLATE.is_file():
        sys.exit("缺少模板：%s\n请从资料盘复制 atk_f103.uvprojx 到该路径。" % TEMPLATE)

    tree = ET.parse(str(TEMPLATE))
    root = tree.getroot()

    target = root.find("./Targets/Target")
    if target is None:
        sys.exit("模板结构异常：找不到 ./Targets/Target")
    _sub(target, "TargetName", TARGET_NAME)

    common = target.find("./TargetOption/TargetCommonOption")
    if common is None:
        sys.exit("模板结构异常：找不到 TargetCommonOption")
    _sub(common, "OutputName", OUTPUT_NAME)

    vc = target.find("./TargetOption/TargetArmAds/Cads/VariousControls")
    if vc is None:
        sys.exit("模板结构异常：找不到 VariousControls")
    defs = DEFINES
    if DEBUG_BUILD:
        defs += "," + DEBUG_DEFINE
    _sub(vc, "Define", defs)
    _sub(vc, "IncludePath", ";".join(INCLUDE_PATHS))

    groups = target.find("Groups")
    if groups is None:
        groups = ET.SubElement(target, "Groups")
    for child in list(groups):
        groups.remove(child)

    for gname, files in GROUPS:
        g = ET.SubElement(groups, "Group")
        _sub(g, "GroupName", gname)
        fs = ET.SubElement(g, "Files")
        for name, rel, ftype in files:
            f = ET.SubElement(fs, "File")
            _sub(f, "FileName", name)
            _sub(f, "FileType", str(ftype))
            _sub(f, "FilePath", rel)

    tree.write(str(OUTPUT), encoding="UTF-8", xml_declaration=True)
    generate_uvoptx()

    total = sum(len(f) for _, f in GROUPS)
    print("[OK] 生成 %s" % OUTPUT.name)
    print("     目标 %s / 产物 %s / %d 个源文件 / %d 条包含路径"
          % (TARGET_NAME, OUTPUT_NAME, total, len(INCLUDE_PATHS)))
    print("     注意：官方模板缺 stm32f1xx_hal_i2c.c 与 stm32f1xx_hal_iwdg.c，"
          "本脚本已补入")


def generate_uvoptx() -> None:
    """生成 greenhouse.uvoptx，把调试器锁定为 ST-Link（详见文件头说明）。"""
    if not UVOPTX_TEMPLATE.is_file():
        print("  ! 缺 uvoptx 模板 %s，跳过（烧录前需在 Keil 里手动选 ST-Link）"
              % UVOPTX_TEMPLATE.name)
        return

    text = UVOPTX_TEMPLATE.read_text(encoding="utf-8", errors="ignore")
    text = re.sub(r"<nTsel>\d+</nTsel>", "<nTsel>%d</nTsel>" % NTESL_STLINK, text)
    text = re.sub(r"<pMon>[^<]*</pMon>",
                  lambda _m: "<pMon>%s</pMon>" % PMON_STLINK, text)
    text = re.sub(r"<TargetName>[^<]*</TargetName>",
                  lambda _m: "<TargetName>%s</TargetName>" % TARGET_NAME, text)
    UVOPTX_OUTPUT.write_text(text, encoding="utf-8")

    n = re.search(r"<nTsel>(\d+)</nTsel>", text)
    p = re.search(r"<pMon>([^<]*)</pMon>", text)
    print("[OK] 生成 %s（调试器 = ST-Link：nTsel=%s, pMon=%s）"
          % (UVOPTX_OUTPUT.name, n.group(1) if n else "?", p.group(1) if p else "?"))


def main() -> None:
    global DEBUG_BUILD
    ap = argparse.ArgumentParser(description="生成 Keil MDK 工程")
    ap.add_argument("--check", action="store_true",
                    help="只校验引用文件是否存在（不生成）。引用缺失一律退出码 1，与本开关无关")
    ap.add_argument("--debug", action="store_true",
                    help="打开调试串口（追加 -DDEBUG_UART=1，用于上板诊断）")
    args = ap.parse_args()
    DEBUG_BUILD = args.debug

    missing = _verify()
    if missing:
        print("以下被引用文件缺失（%d 项）：" % len(missing))
        for m in missing:
            print("  X " + m)
        # ★ 引用缺失一律失败：裸跑（不带 --check）过去只打印警告就继续生成 uvprojx，
        #   于是 build.ps1 的 `if ($LASTEXITCODE -ne 0)` 形同虚设 —— 缺文件的工程
        #   会被"成功"生成，直到 Keil 编译才报错。--check 现在只表示"只校验不生成"，
        #   不再影响是否报错。
        sys.exit(1)
    else:
        print("引用校验通过：源文件与包含路径均存在")
    if DEBUG_BUILD:
        print("注意：本次为诊断构建（DEBUG_UART=1），发布前请重跑不带 --debug 的生成")

    if args.check:
        return
    generate()


if __name__ == "__main__":
    main()
