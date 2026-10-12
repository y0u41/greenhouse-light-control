# tools/simulate_node.py
# 智能温室感光控制 —— 虚拟节点（用于 Qt 上位机演示与联调）。
#
# 完全照 Tech_Design.md 通信协议 + tools/protocol_schema.py 单一事实源实现：
#   - 上行：report（周期上报）/ ack（执行回执）/ evt（主动事件 alarm/recover）
#   - 下行：type="cmd" + action
#   - 所有 action 必须能通过 protocol_schema.is_valid_action / validate_cmd
#
# 用法：
#   py tools/simulate_node.py --id 2                 # 连本机 9092，周期上报，模拟节点 2
#   py tools/simulate_node.py --id 1 --host 192.168.1.50 --port 9092
#   py tools/simulate_node.py --selftest             # 离线冒烟（不连网），校验契约逻辑
#
# 设计要点：
#   - 这是"纯 Python 仿真节点"，行为对齐固件 Control_OnCommand 的状态机，
#     方便在没有 STM32 实物时把 Qt 全链路跑通并演示多点位。
#   - 断线自动重连（指数退避），对应固件"退避重连"纪律。

import argparse
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import protocol_schema as P

# 默认阈值/回差与固件保持一致
TH_DEFAULT = P.TH_DEFAULT
HYS_DEFAULT = P.HYS_DEFAULT
REPORT_INTERVAL = 1.0        # 秒
RECONNECT_BASE = 1.0         # 重连退避基值


def _split_frames(buf):
    r"""按 \n 断帧，返回 (完整帧列表, 剩余缓冲)。

    注意必须把"剩余缓冲"返回给调用方保存：只收半个 JSON 帧是常态
    （TCP 不保证一次 recv 就是一条完整帧），丢掉残留会导致后续帧解析失败。
    """
    lines = []
    while True:
        idx = buf.find(b"\n")
        if idx < 0:
            break
        line = buf[:idx].strip()
        buf = buf[idx + 1:]
        if line:
            lines.append(line)
    return lines, buf


MAX_RX_BUF = 8192        # 无 \n 垃圾的保护上限（与 Qt 侧 nodeserver 一致）


class SimNode:
    """单个虚拟节点的状态机，与固件 Control_Loop / Control_OnCommand 对齐。"""

    def __init__(self, node_id, light=2000):
        self.node_id = node_id
        self.adc = P.ADC_MAX - light
        self.light = light
        self.th = TH_DEFAULT
        self.hys = HYS_DEFAULT
        self.relay = 0
        self.buzz = 0
        self.mode = P.MODE_AUTO          # 0 自动 / 1 手动
        self._t = time.time()

    # ---------- 自动模式决策（回差状态机）----------
    def auto_step(self):
        """自动模式决策；返回本次是否发生状态变化。

        与固件一致：只有自动模式下的状态变化才由 Control_Loop 发 evt(alarm/recover)，
        手动模式自动逻辑完全不介入。
        """
        if self.mode != P.MODE_AUTO:
            return False
        if self.light > self.th and self.relay == 0:        # 光太强 → 吸合（遮阳/报警）
            self.relay = 1
            return True
        if self.light < (self.th - self.hys) and self.relay == 1:   # 回落到回差以下 → 释放
            self.relay = 0
            return True
        return False

    # ---------- 模拟物理量缓慢漂移 ----------
    def tick_light(self):
        self._t += REPORT_INTERVAL
        # 0.05 Hz 正弦 + 微噪声，量程内往返，确保能触发回差动作
        val = 2048 + 1600 * __import__("math").sin(self._t * 0.05)
        self.light = int(max(0, min(P.LIGHT_MAX, val)))
        self.adc = P.ADC_MAX - self.light

    # ---------- 帧构造 ----------
    def report_frame(self):
        return self._frame(P.TYPE_REPORT,
                           adc=self.adc, light=self.light,
                           th=self.th, hys=self.hys,
                           relay=self.relay, buzz=self.buzz, mode=self.mode)

    def ack_frame(self, action, result, evt=None):
        o = self._frame(P.TYPE_ACK, action=action, result=result,
                        adc=self.adc, light=self.light,
                        th=self.th, hys=self.hys,
                        relay=self.relay, buzz=self.buzz, mode=self.mode)
        return o

    def evt_frame(self, event):
        return self._frame(P.TYPE_EVT, event=event,
                           adc=self.adc, light=self.light)

    def _frame(self, ftype, **kw):
        o = {"node": self.node_id, "type": ftype}
        o.update(kw)
        return (json.dumps(o, ensure_ascii=False) + "\n").encode("utf-8")

    # ---------- 处理下行指令（对齐固件 Control_OnCommand）----------
    def handle_cmd(self, obj):
        """返回需要发送的帧列表（ack，可能附带 evt）。"""
        out = []
        action = obj.get("action")
        if not P.is_valid_action(action):
            out.append(self.ack_frame(action, "err"))
            return out

        if action == P.ACTION_RELAY_ON:
            # 手动优先：切手动 + 改输出，只回 ack（不发 evt）——与固件 Control_OnCommand 一致
            self.mode = P.MODE_MANUAL
            self.relay = 1
            out.append(self.ack_frame(action, "ok"))

        elif action == P.ACTION_RELAY_OFF:
            self.mode = P.MODE_MANUAL
            self.relay = 0
            out.append(self.ack_frame(action, "ok"))

        elif action == P.ACTION_AUTO:
            self.mode = P.MODE_AUTO
            # 与固件一致：切回自动后由自动逻辑重算；若因此变化则补发 evt
            if self.auto_step():
                out.append(self.evt_frame(P.EVENT_ALARM if self.relay else P.EVENT_RECOVER))
            out.append(self.ack_frame(action, "ok"))

        elif action == P.ACTION_SET_TH:
            v = obj.get("value")
            ok, why = P.validate_cmd(obj, self.hys)
            if not ok:
                out.append(self.ack_frame(action, "err"))
            else:
                self.th = int(v)
                # 切回自动时立即按新阈值重算
                if self.auto_step():
                    out.append(self.evt_frame(P.EVENT_ALARM if self.relay else P.EVENT_RECOVER))
                out.append(self.ack_frame(action, "ok"))

        elif action == P.ACTION_ALARM_TEST:
            # 蜂鸣器脉冲 1s，不改模式/执行器（仿真节点无蜂鸣器，仅回执）
            out.append(self.ack_frame(action, "ok"))

        elif action == P.ACTION_GET_STATE:
            out.append(self.ack_frame(action, "ok"))

        return out


def _run_network(node, host, port):
    """联网模式：连 Qt NodeServer，周期上报，响应下行。"""
    sock = None
    buf = b""
    last_report = 0.0
    backoff = RECONNECT_BASE

    while True:
        if sock is None:
            try:
                sock = socket.create_connection((host, port), timeout=5.0)
                sock.settimeout(0.2)
                buf = b""
                backoff = RECONNECT_BASE
                print("[node %d] 已连接 %s:%d" % (node.node_id, host, port))
            except OSError as e:
                print("[node %d] 连接失败（%s），%.1fs 后重试" % (node.node_id, e, backoff))
                time.sleep(backoff)
                backoff = min(backoff * 2, 8.0)
                continue

        try:
            # 上行：周期上报（保持长连接，与固件一致——绝不每帧重连）
            now = time.time()
            if now - last_report >= REPORT_INTERVAL:
                node.tick_light()
                if node.auto_step():            # 自动模式状态变化才补发 evt（对齐固件）
                    sock.sendall(node.evt_frame(
                        P.EVENT_ALARM if node.relay else P.EVENT_RECOVER))
                sock.sendall(node.report_frame())
                last_report = now

            # 下行：读并断帧
            try:
                data = sock.recv(4096)
            except socket.timeout:
                data = None                      # 超时 = 暂时没数据，连接仍然正常
            if data is None:
                pass                             # 正常：等下一个上报周期
            elif data:
                buf += data
                lines, buf = _split_frames(buf)  # 残留缓冲必须保留，否则半个帧会丢
                if len(buf) > MAX_RX_BUF:
                    print("[node %d] 接收缓冲超限，已丢弃" % node.node_id)
                    buf = b""
                for line in lines:
                    try:
                        obj = json.loads(line.decode("utf-8"))
                    except (ValueError, UnicodeDecodeError):
                        continue
                    if obj.get("type") == P.DOWNLINK_TYPE:
                        for f in node.handle_cmd(obj):
                            sock.sendall(f)
            else:
                # recv 返回 b"" 才代表对端正常关闭
                sock.close()
                sock = None
                print("[node %d] 连接被关闭，重连中" % node.node_id)

        except OSError as e:
            print("[node %d] 通信异常：%s，重连中" % (node.node_id, e))
            try:
                sock.close()
            except OSError:
                pass
            sock = None


def _selftest():
    """离线冒烟：校验契约逻辑，不连网，可在 CI 直接跑。"""
    print("== simulate_node.py selftest ==")
    assert P.is_valid_action("alarm_test")
    assert not P.is_valid_action("query")

    n = SimNode(1, light=100)
    # 自动模式：光强远低于阈值→继电器应断开
    assert n.auto_step() is False and n.relay == 0, "初始低光应断开且无变化"

    # 强光越过阈值 → 自动模式吸合（对应固件 Control_Loop 发 alarm evt）
    n.light = 3000
    assert n.auto_step() is True and n.relay == 1, "强光应吸合且回报变化"

    # 手动 relay_off：断开 + 切手动，且只回 ack（不发 evt）——与固件 Control_OnCommand 一致
    frames = n.handle_cmd({"node": 1, "type": "cmd", "action": "relay_off"})
    assert n.relay == 0 and n.mode == P.MODE_MANUAL, "relay_off 应断开并切手动"
    assert len(frames) == 1 and b'"ack"' in frames[0], "手动指令只回 ack，不发 evt"

    # 手动优先：此后即使光更强，自动逻辑也不得改动输出
    n.light = 4000
    assert n.auto_step() is False and n.relay == 0, "手动模式下自动逻辑不得介入"

    # 切回自动：应按当前光强重算并补发 evt
    frames = n.handle_cmd({"node": 1, "type": "cmd", "action": "auto"})
    assert n.relay == 1, "切回自动后强光应重新吸合"
    assert any(b"alarm" in f for f in frames), "状态变化应补发 alarm 事件"

    # set_th 必须大于回差
    ok, why = P.validate_cmd({"action": "set_th", "value": 100}, n.hys)
    assert not ok, "value<=hys 应拒绝"
    frames = n.handle_cmd({"node": 1, "type": "cmd", "action": "set_th", "value": 2500})
    assert n.th == 2500, "set_th 应生效"

    # 断帧：残留缓冲必须保住，半个帧不能被当成完整帧
    lines, rest = _split_frames(b'{"node":1}\n{"node":2,"t')
    assert len(lines) == 1 and rest == b'{"node":2,"t', "残留缓冲处理错误"

    # get_state 回执携带完整状态
    frames = n.handle_cmd({"node": 1, "type": "cmd", "action": "get_state"})
    ack = json.loads(frames[0].decode("utf-8"))
    assert ack["type"] == "ack" and ack["action"] == "get_state"
    assert "th" in ack and "relay" in ack, "ack 应含完整状态"

    # 非法动作回 err
    frames = n.handle_cmd({"node": 1, "type": "cmd", "action": "open_relay"})
    bad = json.loads(frames[0].decode("utf-8"))
    assert bad.get("result") == "err", "未知动作应回 err"

    # 帧是 \\n 结尾且能被 JSON 解析
    raw = n.report_frame()
    assert raw.endswith(b"\n")
    assert json.loads(raw.decode("utf-8"))["node"] == 1

    print("selftest OK")


def main():
    ap = argparse.ArgumentParser(description="智能温室虚拟节点（协议仿真）")
    ap.add_argument("--id", type=int, default=1, help="节点 ID（从 1 起）")
    ap.add_argument("--host", default="127.0.0.1", help="Qt NodeServer 地址")
    ap.add_argument("--port", type=int, default=9092, help="Qt NodeServer 端口")
    ap.add_argument("--selftest", action="store_true", help="离线冒烟测试（不连网）")
    args = ap.parse_args()

    if args.selftest:
        _selftest()
        return

    node = SimNode(args.id)
    print("[node %d] 启动虚拟节点，目标 %s:%d，Ctrl+C 退出" % (args.id, args.host, args.port))
    try:
        _run_network(node, args.host, args.port)
    except KeyboardInterrupt:
        print("\n[node %d] 已退出" % args.id)


if __name__ == "__main__":
    main()
