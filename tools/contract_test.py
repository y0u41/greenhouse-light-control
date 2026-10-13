# tools/contract_test.py
# 协议契约测试：把"三处必须一致"从文档警告变成可执行的 CI/本地检查。
#
# 运行：
#   py tools/contract_test.py            # 无 pytest 也能跑（自带 runner）
#   py -m pytest tools/contract_test.py  # 已有 pytest 时
#
# 覆盖：
#   - 动作词汇单一事实源（get_state 合法 / query 非法）
#   - 下行指令校验与固件/Qt 的 validate() 一致
#   - 上行 type / event 合法
#   - 示例帧往返解析
#   - 下行帧的**职责边界**：type="cmd" + node 由帧封装层保证（固件硬要求），
#     validate_cmd 只校验 action —— 见 test_downlink_must_carry_cmd_type

import json
import sys
import os

# 允许 `py tools/contract_test.py` 直接 import 同目录的 protocol_schema
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import protocol_schema as P


def _fail(msg):
    print("FAIL: " + msg)
    raise AssertionError(msg)


# ---------------- 动作词汇单一事实源 ----------------

def test_action_vocabulary_complete():
    expected = {"relay_on", "relay_off", "auto", "set_th", "get_state", "alarm_test"}
    if P.ACTION_SET != expected:
        _fail("ACTION_SET 与文档不一致: %s" % P.ACTION_SET)


def test_alarm_test_action():
    # 报警测试：合法动作，无需参数
    if not P.is_valid_action("alarm_test"):
        _fail("alarm_test 应合法（PRD 报警测试按钮）")
    ok, _ = P.validate_cmd({"action": "alarm_test"})
    if not ok:
        _fail("alarm_test 无需参数却被判非法")


def test_get_state_not_query():
    # Tech_Design 反复强调：get_state 是合法动作，query 不是
    if not P.is_valid_action("get_state"):
        _fail("get_state 应合法")
    if P.is_valid_action("query"):
        _fail("query 不应是合法动作（与 get_state 漂移）")


def test_unknown_action_rejected():
    ok, why = P.validate_cmd({"action": "open_relay"})
    if ok:
        _fail("未知动作应被拒绝")


# ---------------- 下行指令校验（与固件/Qt validate 一致） ----------------

def test_cmd_no_param_actions():
    for a in ("relay_on", "relay_off", "auto", "get_state"):
        ok, _ = P.validate_cmd({"action": a})
        if not ok:
            _fail("%s 无需参数却被判非法" % a)


def test_set_th_range():
    # 超量程
    if P.validate_cmd({"action": "set_th", "value": 5000})[0]:
        _fail("value>4095 应拒绝")
    if P.validate_cmd({"action": "set_th", "value": -1})[0]:
        _fail("value<0 应拒绝")
    # 不大于回差
    if P.validate_cmd({"action": "set_th", "value": 100}, hys=150)[0]:
        _fail("value<=hys 应拒绝")
    # 边界：等于 hys 拒绝，hys+1 通过
    if P.validate_cmd({"action": "set_th", "value": 150}, hys=150)[0]:
        _fail("value==hys 应拒绝")
    if not P.validate_cmd({"action": "set_th", "value": 151}, hys=150)[0]:
        _fail("value==hys+1 应通过")
    # 缺少 value
    if P.validate_cmd({"action": "set_th"})[0]:
        _fail("缺 value 应拒绝")


def test_set_th_uses_live_hys():
    # 校验必须吃当前回差，而不是写死默认值
    if P.validate_cmd({"action": "set_th", "value": 160}, hys=200)[0]:
        _fail("应吃实时 hys=200，value=160 需拒绝")


# ---------------- 上行 type / event ----------------

def test_uplink_types():
    for t in ("report", "ack", "evt"):
        if not P.is_valid_uplink_type(t):
            _fail("上行 type %s 应合法" % t)
    if P.is_valid_uplink_type("cmd"):
        _fail("上行不应出现 type=cmd")


def test_events():
    for e in ("alarm", "recover"):
        if not P.is_valid_event(e):
            _fail("event %s 应合法" % e)


# ---------------- 示例帧往返 ----------------

def test_example_frames_roundtrip():
    for raw in P.build_example_frames():
        obj = json.loads(raw)  # 必须能解析
        if "node" not in obj:
            _fail("示例帧缺 node: %s" % raw)
        t = obj.get("type")
        if t == P.DOWNLINK_TYPE:
            if "action" not in obj:
                _fail("下行帧缺 action: %s" % raw)
            if not P.is_valid_action(obj["action"]):
                _fail("下行帧 action 非法: %s" % raw)
        elif t in P.UPLINK_TYPE_SET:
            continue
        else:
            _fail("未知 type: %s" % raw)


def _build_cmd(node_id, action, value=None):
    """下行帧**封装层**的等价实现。

    对应 qtapp/src/commandengine.cpp::CommandEngine::buildCmd()：
        o.insert("node", nodeId);
        o.insert("type", Proto::TYPE_CMD);   // protocol_actions.h: TYPE_CMD = "cmd"
        o.insert("action", action);
        set_th 时再 insert("value", value)；
    这是 Qt 唯一的下行组包点（nodeserver.cpp 与 mainwindow.cpp 都走它）。
    """
    o = {"node": node_id, "type": P.DOWNLINK_TYPE, "action": action}
    if action == P.ACTION_SET_TH:
        o["value"] = value
    return o


def _expect(cond, msg):
    """断言失败即走 _fail（带 FAIL: 前缀 + 非零退出码）。"""
    if not cond:
        _fail(msg)


def test_downlink_must_carry_cmd_type():
    """下行帧必须带 type="cmd" + node + action —— 但**由帧封装层保证**。

    职责边界（三处实现一致，这也是本测试要钉住的约定）：
      · 封装层  CommandEngine::buildCmd() 产出 {"node","type":"cmd","action"[,"value"]}；
                固件 protocol.c::Protocol_RxByte/OnFrame 则**硬要求** type=="cmd"
                且 node 存在 / 为数字 / int32 全宽相等，否则整帧丢弃。
      · 校验层  tools/protocol_schema.py::validate_cmd() 与
                CommandEngine::validate() **只看 action**，不重复校验 type/node。
    因此本测试断言的是"封装层产出的帧一定含 type/node"，而不是"validate_cmd 会校验
    type/node" —— 后者是假的（原实现 `if bad.get("type") != P.DOWNLINK_TYPE: pass`
    恒不触发，`assert P.DOWNLINK_TYPE == "cmd"` 恒真，等于没测）。
    """
    # 1) 校验层职责：只看 action，type/node 缺失/错误都不影响判定
    for act in sorted(P.ACTION_SET):
        extra = {"value": 2500} if act == P.ACTION_SET_TH else {}
        ok, why = P.validate_cmd({"action": act, **extra})
        _expect(ok, "validate_cmd 应接受合法 action=%s：%s" % (act, why))
        ok_missing, _ = P.validate_cmd(extra)          # 连 action 都没有
        _expect(not ok_missing, "缺 action 必须被 validate_cmd 拒绝")

    # 2) 封装层职责：产出的 dict 必须含 type/node/action，且 type 恰为 "cmd"
    for act in sorted(P.ACTION_SET):
        value = 2500 if act == P.ACTION_SET_TH else None
        frame = _build_cmd(1, act, value)
        _expect("type" in frame, "下行帧缺 type：%r" % frame)
        _expect(frame["type"] == P.DOWNLINK_TYPE,
                "下行帧 type 必须是 %r，实为 %r" % (P.DOWNLINK_TYPE, frame.get("type")))
        _expect("node" in frame, "下行帧缺 node：%r" % frame)
        _expect("action" in frame, "下行帧缺 action：%r" % frame)
        # 固件 protocol.c 用的是 cJSON_IsNumber -> JSON 布尔不算数字
        _expect(isinstance(frame["node"], int) and not isinstance(frame["node"], bool),
                "node 必须是数字（固件 cJSON_IsNumber）：%r" % frame["node"])

    # 3) 契约常量本身（唯一事实源）
    _expect(P.DOWNLINK_TYPE == "cmd", "DOWNLINK_TYPE 必须是 cmd")


# ---------------- 自带 runner（无 pytest 也可执行） ----------------

def _run_all():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    n = 0
    for t in tests:
        t()
        n += 1
        print("  PASS  %s" % t.__name__)
    print("全部 %d 个契约测试通过" % n)


if __name__ == "__main__":
    try:
        _run_all()
        print("contract_test OK")
        sys.exit(0)
    except AssertionError as e:
        print("contract_test FAILED: %s" % e)
        sys.exit(1)
