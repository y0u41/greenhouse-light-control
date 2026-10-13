#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
主机端行为测试运行器 —— 在 PC 上编译并运行 control.c / protocol.c / eeprom.c 的用例。

为什么值得做（P0-2）：
  control.c 是全项目改动最频繁的文件，却只依赖 4 个 HAL 符号
  （HAL_GetTick / HAL_GPIO_Init / HAL_GPIO_WritePin / 时钟宏）。
  没有它时，每次改控制逻辑都要"烧录 -> 接硬件 -> 手电照"，迭代成本极高；
  有了它，改完秒级验证，硬件只用于最终确认。

实现要点：
  · 用 tests/host/shim/stm32f1xx_hal.h 作为 HAL 垫片，放在 -I 最前面。
    真实 HAL 头会经 stm32f1xx.h -> core_cm3.h 引入 ARM 内联汇编，
    x86 的 gcc 在语法解析阶段就会失败。
  · 编译的是 **firmware/Core/Src/control.c 原文**，不做任何复制或改动。
  · 全部用例纯逻辑，不碰硬件、不需要串口。

用法：
  python tests/host/run_host_tests.py            # 跑全部
  python tests/host/run_host_tests.py -v         # 顺便打印编译命令
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
HOST = ROOT / "tests" / "host"
SHIM = HOST / "shim"
BUILD = HOST / "build"

GCC_CANDIDATES = [
    r"E:\Qt\Tools\mingw1310_64\bin\gcc.exe",
    r"C:\Qt\Tools\mingw1310_64\bin\gcc.exe",
    "gcc",
]

DEFINES = ["-DUSE_HAL_DRIVER", "-DSTM32F103xE", "-DDEBUG_UART=1"]
INCLUDES = [
    "-I" + str(SHIM),                       # 必须最先：让 stm32f1xx_hal.h 命中垫片
    "-I" + str(ROOT / "firmware" / "Core" / "Inc"),
    "-I" + str(ROOT / "firmware" / "Middlewares" / "cJSON"),
]
CFLAGS = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra",
          "-Wno-unused-parameter", "-Wno-sign-compare"]

# (用例名, [源文件...], 可执行名)
SUITES = [
    ("control",  [HOST / "test_control.c", HOST / "stubs.c",
                  ROOT / "firmware" / "Core" / "Src" / "control.c"], "test_control"),
    ("protocol", [HOST / "test_protocol.c", HOST / "stubs_protocol.c",
                  ROOT / "firmware" / "Core" / "Src" / "protocol.c",
                  ROOT / "firmware" / "Middlewares" / "cJSON" / "cJSON.c"], "test_protocol"),
    ("eeprom",   [HOST / "test_eeprom.c", HOST / "stubs_eeprom.c",
                  ROOT / "firmware" / "Core" / "Src" / "eeprom.c"], "test_eeprom"),
]


def find_gcc(explicit=None):
    if explicit:
        return explicit
    for c in GCC_CANDIDATES:
        if os.path.sep in c:
            if Path(c).is_file():
                return c
        else:
            p = shutil.which(c)
            if p:
                return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--gcc", default=None)
    args = ap.parse_args()

    gcc = find_gcc(args.gcc)
    if not gcc:
        print("!! 找不到 gcc。请用 --gcc <path> 指定，或安装 MinGW。")
        print("   候选: " + ", ".join(GCC_CANDIDATES))
        return 2

    # ★★ 必须把 MinGW 的 bin 目录加进 PATH ★★
    # 否则会出现一个**极难排查**的失败：gcc 驱动本身能启动（它就在 bin 里，
    # 所以 `gcc --version` 正常），但它孵化的 cc1.exe 找不到运行库 DLL，
    # 以 0xC0000135 (STATUS_DLL_NOT_FOUND) 退出 —— 而 gcc 驱动**一条错误信息都不打**，
    # 只返回 exit 1。本次实测就是被这个坑卡住，排查成本很高。
    gcc_dir = str(Path(gcc).resolve().parent)
    os.environ["PATH"] = gcc_dir + os.pathsep + os.environ.get("PATH", "")
    print("gcc: %s" % gcc)

    BUILD.mkdir(parents=True, exist_ok=True)
    rc_all = 0

    for name, sources, exe in SUITES:
        for s in sources:
            if not Path(s).is_file():
                print("!! 源文件缺失: %s" % s)
                return 2

        out = BUILD / (exe + ".exe")
        cmd = [gcc] + CFLAGS + DEFINES + INCLUDES + [str(s) for s in sources] + \
              ["-o", str(out), "-lm"]

        if args.verbose:
            print("\n$ " + " ".join(cmd))
        print("\n" + "=" * 64)
        print("编译并运行: %s" % name)
        print("=" * 64)

        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        if r.returncode != 0:
            print("编译失败：")
            print(r.stdout)
            print(r.stderr)
            rc_all = 1
            continue
        if r.stderr.strip():
            print("[编译警告]")
            print(r.stderr)

        run = subprocess.run([str(out)], capture_output=True, text=True,
                             encoding="utf-8", errors="replace")
        sys.stdout.write(run.stdout)
        if run.stderr.strip():
            sys.stdout.write(run.stderr)
        if run.returncode != 0:
            rc_all = 1

    print("\n" + ("全部通过" if rc_all == 0 else "存在失败用例"))
    return rc_all


if __name__ == "__main__":
    sys.exit(main())
