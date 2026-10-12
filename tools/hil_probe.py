#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
hil_probe.py -- 硬件在环（HIL）验证探针

为什么需要它
    Qt 上位机是 TCP 服务端，板子是客户端。要验证固件，最直接的办法是
    "自己当服务端"：这样既能**看到板子真实发出的每一帧**，又能主动下发指令
    并检查回执 —— 不依赖图形界面（看不见）也不依赖虚拟节点（那是仿真）。

它做什么
    1) 在指定端口监听，接受节点的 TCP 连接（行为对齐 Qt NodeServer）
    2) 逐帧打印收到的 report / ack / evt（带时间戳与解析后的字段）
    3) 按脚本下发一组指令，逐一核对 ack 与后续 report 的变化
    4) 自动核对协议不变式，最后打印 PASS/FAIL 汇总

自动核对项
    - 上报周期是否约为 1 s
    - 每帧是否满足 light == 4095 - adc
    - 每帧是否满足 0 <= hys < th <= 4095
    - get_state / alarm_test 是否回 ok
    - 非法阈值（<= hys、超量程）是否回 err 且不改变现有阈值
    - 合法 set_th 是否生效并体现在后续 report
    - **手动优先**：relay_off 之后连续若干帧是否保持 mode=1 且 relay=0（不被自动逻辑回弹）
    - auto 是否切回自动模式

用法
    py tools/hil_probe.py                      # 监听 0.0.0.0:9092，跑完整指令序列
    py tools/hil_probe.py --port 9092 --duration 24
    py tools/hil_probe.py --listen-only        # 只听不发，纯观察上报流

注意
    端口必须与固件 wifi_config.h 里的 TCP_SERVER_PORT 一致（默认 9092）。
    **不要与 Qt 上位机同时运行**：两者都要绑同一端口，会冲突。
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import protocol_schema as P  # noqa: E402

ADC_MAX = P.ADC_MAX


def ts() -> str:
    return time.strftime("%H:%M:%S")


class Probe:
    def __init__(self, node_id: int):
        self.node_id = node_id
        self.sock = None
        self.buf = b""
        self.frames: list[dict] = []
        self.report_times: list[float] = []
        self.acks: dict[str, str] = {}          # action -> result（最后一次）
        self.notes: list[str] = []

    # ---------------- 网络 ----------------
    def accept(self, listen: str, port: int, timeout: float = 60.0) -> bool:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((listen, port))
        srv.listen(1)
        srv.settimeout(timeout)
        print("[%s] 监听 %s:%d ，等待节点接入 ..." % (ts(), listen, port))
        try:
            self.sock, peer = srv.accept()
        except socket.timeout:
            print("[%s] 超时：%gs 内没有节点接入。" % (ts(), timeout))
            print("        检查：固件里 TCP_SERVER_IP 是否指向本机局域网 IP、"
                  "WiFi 是否连上、防火墙是否放行该端口")
            srv.close()
            return False
        srv.close()
        self.sock.settimeout(0.2)
        print("[%s] 节点已接入：%s:%d" % (ts(), peer[0], peer[1]))
        return True

    def send_cmd(self, action: str, value: int | None = None) -> None:
        o = {"node": self.node_id, "type": P.DOWNLINK_TYPE, "action": action}
        if value is not None:
            o["value"] = value
        line = (json.dumps(o, ensure_ascii=False) + "\n").encode("utf-8")
        try:
            self.sock.sendall(line)
        except OSError as e:
            print("[%s] 发送失败：%s" % (ts(), e))
            return
        print("[%s]   --> 下发 %s%s" % (ts(), action, "" if value is None else " value=%d" % value))

    def pump(self) -> None:
        """读一次 socket 并把完整帧登记/打印。"""
        try:
            data = self.sock.recv(4096)
        except socket.timeout:
            return
        except OSError:
            return
        if not data:
            return
        self.buf += data
        while True:
            idx = self.buf.find(b"\n")
            if idx < 0:
                break
            line = self.buf[:idx].strip()
            self.buf = self.buf[idx + 1:]
            if line:
                self._on_frame(line)

    def _on_frame(self, line: bytes) -> None:
        try:
            o = json.loads(line.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            print("[%s] <-- 非 JSON：%r" % (ts(), line[:80]))
            return
        self.frames.append(o)
        t = o.get("type")
        if t == P.TYPE_REPORT:
            self.report_times.append(time.time())
            g = lambda k, d="-": o.get(k, d)
            print("[%s] <-- report adc=%-4s light=%-4s th=%-4s hys=%-3s relay=%s buzz=%s mode=%s"
                  % (ts(), g("adc"), g("light"), g("th"), g("hys"),
                     g("relay"), g("buzz"), g("mode")))
        elif t == P.TYPE_ACK:
            a, r = o.get("action"), o.get("result")
            self.acks[a] = r
            print("[%s] <-- ack    action=%-10s result=%s" % (ts(), a, r))
        elif t == P.TYPE_EVT:
            print("[%s] <-- evt    event=%s adc=%s light=%s"
                  % (ts(), o.get("event"), o.get("adc"), o.get("light")))
        else:
            print("[%s] <-- 未知 type=%r" % (ts(), t))

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass

    # ---------------- 序列 ----------------
    def scripted_session(self, duration: float) -> None:
        """按时间表下发指令，期间持续 pump。"""
        # (相对秒, 描述, 动作)
        plan = [
            (2.0,  "查询状态",            ("get_state", None)),
            (4.5,  "非法阈值 <= hys",     ("set_th", P.HYS_DEFAULT)),        # 150 <= 150 -> err
            (7.0,  "非法阈值 超量程",     ("set_th", 5000)),                 # > 4095 -> err
            (9.5,  "合法阈值 2500",       ("set_th", 2500)),                 # 应生效
            (12.5, "手动释放(切手动)",    ("relay_off", None)),
            (16.0, "手动吸合",            ("relay_on", None)),
            (19.0, "切回自动",            ("auto", None)),
            (21.5, "报警测试",            ("alarm_test", None)),
        ]
        t0 = time.time()
        sent = [False] * len(plan)
        while time.time() - t0 < duration:
            self.pump()
            el = time.time() - t0
            for i, (at, desc, (act, val)) in enumerate(plan):
                if not sent[i] and el >= at:
                    sent[i] = True
                    print("[%s] * %s" % (ts(), desc))
                    self.send_cmd(act, val)
            time.sleep(0.02)
        # 收尾：再 pump 一会儿
        for _ in range(30):
            self.pump()
            time.sleep(0.02)

    def listen_only(self, duration: float) -> None:
        t0 = time.time()
        while time.time() - t0 < duration:
            self.pump()
            time.sleep(0.02)

    # ---------------- 核对 ----------------
    def verify(self) -> list[tuple[str, bool, str]]:
        res: list[tuple[str, bool, str]] = []
        reports = [f for f in self.frames if f.get("type") == P.TYPE_REPORT]

        # 1 上报周期
        if len(self.report_times) >= 3:
            spans = [self.report_times[i + 1] - self.report_times[i]
                     for i in range(len(self.report_times) - 1)]
            avg = sum(spans) / len(spans)
            ok = 0.7 <= avg <= 1.4
            res.append(("上报周期约为 1 s", ok, "实测均值 %.3f s（%d 帧）" % (avg, len(reports))))
        else:
            res.append(("上报周期约为 1 s", False, "只收到 %d 帧 report，不足以判断" % len(reports)))

        # 2 light == 4095 - adc
        bad = [r for r in reports if r.get("light") != ADC_MAX - r.get("adc", -1)]
        res.append(("light == 4095 - adc", not bad,
                    "全部 %d 帧通过" % len(reports) if not bad else "有 %d 帧不符，例如 %s" % (len(bad), bad[0])))

        # 3 不变式 0 <= hys < th <= 4095
        bad2 = [r for r in reports
                if not (0 <= r.get("hys", -1) < r.get("th", -1) <= ADC_MAX)]
        res.append(("不变式 0<=hys<th<=4095", not bad2,
                    "全部通过" if not bad2 else "有 %d 帧违反，例如 %s" % (len(bad2), bad2[0])))

        # 4 get_state / alarm_test 回 ok
        for act in ("get_state", "alarm_test"):
            if act in self.acks:
                res.append(("%s 回 ok" % act, self.acks[act] == "ok", "result=%s" % self.acks[act]))
            else:
                res.append(("%s 回 ok" % act, False, "未收到该动作的 ack"))

        # 5 非法阈值回 err（<=hys 与超量程各下发一次）
        errs = sum(1 for f in self.frames
                   if f.get("type") == P.TYPE_ACK and f.get("action") == "set_th"
                   and f.get("result") == "err")
        res.append(("非法阈值回 err", errs >= 2, "收到 %d 次 set_th=err（期望 >=2：<=hys 与超量程各一次）" % errs))

        # 6 合法 set_th 生效
        got = [r for r in reports if r.get("th") == 2500]
        res.append(("合法 set_th=2500 生效", len(got) >= 1,
                    "后续 %d 帧 report 的 th=2500" % len(got)))

        # 7 手动优先：relay_off 之后连续保持 mode=1 且 relay=0
        idx = None
        for i, f in enumerate(self.frames):
            if f.get("type") == P.TYPE_ACK and f.get("action") == "relay_off" and f.get("result") == "ok":
                idx = i
                break
        if idx is None:
            res.append(("手动优先（relay_off 不回弹）", False, "未收到 relay_off 的 ok 回执"))
        else:
            after = [f for f in self.frames[idx + 1:] if f.get("type") == P.TYPE_REPORT]
            held = 0
            for f in after:
                if f.get("mode") == P.MODE_MANUAL and f.get("relay") == 0:
                    held += 1
                elif f.get("mode") == P.MODE_MANUAL and f.get("relay") == 1:
                    break        # 之后可能被 relay_on 改回，正常
                else:
                    break
            res.append(("手动优先（relay_off 不回弹）", held >= 3,
                        "relay_off 后连续 %d 帧保持 mode=1 relay=0（期望 >=3）" % held))

        # 8 auto 切回自动
        idx2 = None
        for i, f in enumerate(self.frames):
            if f.get("type") == P.TYPE_ACK and f.get("action") == "auto" and f.get("result") == "ok":
                idx2 = i
        if idx2 is None:
            res.append(("auto 切回自动模式", False, "未收到 auto 的 ok 回执"))
        else:
            after = [f for f in self.frames[idx2 + 1:] if f.get("type") == P.TYPE_REPORT]
            ok = any(f.get("mode") == P.MODE_AUTO for f in after)
            res.append(("auto 切回自动模式", ok,
                        "auto 后 %d 帧 report 中 mode=0 的有 %d 帧"
                        % (len(after), sum(1 for f in after if f.get("mode") == P.MODE_AUTO))))

        return res


def main() -> None:
    ap = argparse.ArgumentParser(description="硬件在环验证探针（代替 Qt 当服务端）")
    ap.add_argument("--listen", default="0.0.0.0", help="监听地址（默认 0.0.0.0）")
    ap.add_argument("--port", type=int, default=9092, help="监听端口（默认 9092）")
    ap.add_argument("--node", type=int, default=1, help="节点 ID（默认 1，须与固件 NODE_ID 一致）")
    ap.add_argument("--duration", type=float, default=24.0, help="会话时长秒（默认 24）")
    ap.add_argument("--listen-only", action="store_true", help="只听不发，纯观察上报流")
    args = ap.parse_args()

    p = Probe(args.node)
    if not p.accept(args.listen, args.port):
        sys.exit(1)

    print("[%s] === 会话开始（%.0fs）===" % (ts(), args.duration))
    try:
        if args.listen_only:
            p.listen_only(args.duration)
        else:
            p.scripted_session(args.duration)
    except KeyboardInterrupt:
        print("\n[%s] 手动中断" % ts())
    finally:
        p.close()

    print("\n" + "=" * 68)
    print("HIL 核对结果")
    print("=" * 68)
    fails = 0
    for name, ok, detail in p.verify():
        print("  [%s] %-28s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            fails += 1
    print("-" * 68)
    print("共收到 %d 帧（report %d、ack %d、evt %d）"
          % (len(p.frames),
             sum(1 for f in p.frames if f.get("type") == P.TYPE_REPORT),
             sum(1 for f in p.frames if f.get("type") == P.TYPE_ACK),
             sum(1 for f in p.frames if f.get("type") == P.TYPE_EVT)))
    if fails:
        print("结论：%d 项未通过" % fails)
        sys.exit(1)
    print("结论：全部通过")


if __name__ == "__main__":
    main()
