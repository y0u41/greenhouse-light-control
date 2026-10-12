#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
上板探针（可靠读数基础设施）。

存在的理由：本项目此前用 PowerShell 单行脚本读 STM32 寄存器，反复被
  · 内置别名冲突（r=Invoke-History、rd=Remove-Item、h=Get-History）
  · 多行输出只解析第一行
  · 把 "ST-LINK SN : ..." 之类的行误当数据行
坑到，产生了多次**错误结论**（例如把 ST-LINK 序列号当成 g_adc、把
s_transparent 读成 RAM 地址）。本脚本用严格正则只认
`0xAAAAAAAA : WWWWWWWW WWWWWWWW` 这种数据行，杜绝上述问题。

用法：
    py tools/probe.py --check                    # 判定是否在复位循环（采样 uwTick）
    py tools/probe.py --read g_adc g_th g_light  # 按符号名读 32 位字
    py tools/probe.py --addr 0x40021024          # 按地址读
    py tools/probe.py --core                     # 读核心寄存器（PC/SP/LR/XPSR）
    py tools/probe.py --fault                    # 读 CFSR/HFSR/MMFAR/BFAR 与异常栈帧
"""
import argparse
import os
import re
import subprocess
import sys
import time

LOCAL = os.environ.get("LOCALAPPDATA", "")
CLI = os.path.join(LOCAL, r"stm32cube\bundles\programmer\2.23.0\bin\STM32_Programmer_CLI.exe")
NM = os.path.join(LOCAL, r"stm32cube\bundles\gnu-tools-for-stm32\14.3.1+st.2\bin\arm-none-eabi-nm.exe")

AXF_CANDIDATES = [
    r"D:\glc\firmware\Output\greenhouse.axf",
    r"D:\正点原子\个人项目实战\智能温室感光控制-华清远见\firmware\Output\greenhouse.axf",
]

# 只匹配真正的数据行：行首是 8 位十六进制地址 + 冒号 + 至少一个 8 位十六进制字
DATA_RE = re.compile(r"^0x([0-9A-Fa-f]{8})\s*:\s*((?:[0-9A-Fa-f]{8}\s*)+)$", re.M)


def find_axf():
    for p in AXF_CANDIDATES:
        if os.path.isfile(p):
            return p
    raise SystemExit("找不到 greenhouse.axf，请检查构建产物路径")


def cli(*args, timeout=90):
    if not os.path.isfile(CLI):
        raise SystemExit("找不到 STM32_Programmer_CLI.exe: %s" % CLI)
    proc = subprocess.run([CLI] + list(args), capture_output=True, text=True,
                          errors="replace", timeout=timeout)
    return proc.stdout + proc.stderr


def read_words(addr, size):
    """读内存，返回 32 位字列表。size 必须是 4 的倍数。"""
    out = cli("-c", "port=SWD", "mode=hotplug", "-r32", hex(addr), hex(size))
    words = []
    for m in DATA_RE.finditer(out):
        for w in m.group(2).split():
            words.append(int(w, 16))
    if not words:
        raise RuntimeError("读取失败 @0x%08x\n%s" % (addr, out.strip()[:400]))
    return words


def read_word(addr):
    return read_words(addr, 4)[0]


def read_core():
    """解析 --coreReg 输出；同样只认 'NAME = 0x...' 这种行。"""
    out = cli("-c", "port=SWD", "mode=hotplug", "-coreReg")
    regs = {}
    for line in out.splitlines():
        m = re.match(r"^\s*([A-Za-z0-9]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*$", line)
        if m:
            regs[m.group(1)] = int(m.group(2), 16) if m.group(2).startswith("0x") else int(m.group(2))
    return regs


def load_symbols(names):
    axf = find_axf()
    out = subprocess.run([NM, "-n", axf], capture_output=True, text=True,
                         errors="replace").stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] in names:
            try:
                found[parts[-1]] = int(parts[0], 16)
            except ValueError:
                pass
    return found, axf


IPS = {0: "Thread", 3: "HardFault", 4: "MemManage", 5: "BusFault", 6: "UsageFault",
       11: "SVC", 14: "PendSV", 15: "SysTick"}


def cmd_check():
    syms, _ = load_symbols({"uwTick", "g_adc", "g_th", "s_state", "s_backoff_ms"})
    need = {"uwTick"}
    missing = need - set(syms)
    if missing:
        raise SystemExit("符号缺失: %s" % missing)
    print("采样 uwTick（应单调递增；不断变小 = 正在复位）")
    prev = None
    resets = 0
    for i in range(8):
        u = read_word(syms["uwTick"])
        extra = ""
        for n in ("g_th", "g_adc", "s_state", "s_backoff_ms"):
            if n in syms:
                extra += "  %s=0x%08x" % (n, read_word(syms[n]))
        flag = ""
        if prev is not None and u < prev:
            flag = "   <== 复位!"
            resets += 1
        print("  #%d  uwTick=%-10d (0x%08x)%s%s" % (i, u, u, extra, flag))
        prev = u
        time.sleep(1.5)
    print()
    print("判定: %s" % ("**正在反复复位**" if resets else "未观察到复位"))
    return 1 if resets else 0


def cmd_read(names):
    syms, axf = load_symbols(set(names))
    print("ELF: %s" % axf)
    for n in names:
        if n not in syms:
            print("  %-16s  <未找到>" % n)
            continue
        a = syms[n]
        w = read_word(a)
        print("  %-16s @0x%08x = 0x%08x (%d)" % (n, a, w, w))


def cmd_addr(addrs):
    for a in addrs:
        a = int(a, 0)
        print("  0x%08x = 0x%08x" % (a, read_word(a)))


def cmd_core():
    r = read_core()
    order = ["PC", "LR", "SP", "MSP", "PSP", "XPSR", "APSR", "IPSR", "EPSR",
             "PRIMASK", "FAULTMASK", "BASEPRI", "CONTROL"]
    for k in order:
        if k in r:
            print("  %-10s = 0x%08x" % (k, r[k]))
    if "XPSR" in r:
        ipsr = r["XPSR"] & 0x1FF
        print("  => IPSR = %d (%s)" % (ipsr, IPS.get(ipsr, "?")))
        print("  => %s" % ("**处于异常处理中**" if ipsr else "线程模式（正常）"))


def cmd_fault():
    w = read_words(0xE000ED28, 0x14)
    names = ["CFSR", "HFSR", "DFSR", "MMFAR", "BFAR"]
    for n, v in zip(names, w):
        print("  %-6s = 0x%08x" % (n, v))
    cfsr = w[0]
    bits = [(0, "IACCVIOL"), (1, "DACCVIOL"), (3, "MUNSTKERR"), (4, "MSTKERR"),
            (8, "IBUSERR"), (9, "PRECISERR"), (10, "IMPRECISERR"), (11, "UNSTKERR"),
            (12, "STKERR"), (16, "UNDEFINSTR"), (17, "INVSTATE"), (18, "INVPC"),
            (19, "NOCP"), (24, "UNALIGNED"), (25, "DIVBYZERO")]
    on = [nm for b, nm in bits if cfsr & (1 << b)]
    print("  CFSR 置位: %s" % (", ".join(on) if on else "无"))
    if w[1] & (1 << 30):
        print("  HFSR.FORCED: 有可配置故障升级为 HardFault")
    core = read_core()
    if core.get("XPSR", 0) & 0x1FF == 3:
        sp = core.get("MSP", core.get("SP", 0))
        print("\n  异常栈帧 @ 0x%08x：" % sp)
        f = read_words(sp, 0x20)
        lbl = ["R0", "R1", "R2", "R3", "R12", "LR", "PC", "xPSR"]
        for n, v in zip(lbl, f):
            print("    %-4s = 0x%08x" % (n, v))
        print("    >>> 故障指令 PC = 0x%08x   LR = 0x%08x" % (f[6], f[5]))


def main():
    ap = argparse.ArgumentParser(description="上板探针（可靠读数）")
    ap.add_argument("--check", action="store_true", help="采样 uwTick 判定是否复位循环")
    ap.add_argument("--read", nargs="+", metavar="SYM", help="按符号名读取")
    ap.add_argument("--addr", nargs="+", metavar="ADDR", help="按地址读取")
    ap.add_argument("--core", action="store_true", help="读核心寄存器")
    ap.add_argument("--fault", action="store_true", help="读故障寄存器与异常栈帧")
    a = ap.parse_args()
    if a.check:
        return cmd_check()
    if a.read:
        cmd_read(a.read)
    if a.addr:
        cmd_addr(a.addr)
    if a.core:
        cmd_core()
    if a.fault:
        cmd_fault()
    if not any([a.check, a.read, a.addr, a.core, a.fault]):
        ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
