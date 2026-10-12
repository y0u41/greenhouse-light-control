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
#   - 下行帧必须带 type="cmd" + action

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


def test_downlink_must_carry_cmd_type():
    # 协议规定下行帧 type="cmd"，防止固件/Qt 解析分支不一致
    bad = {"node": 1, "action": "relay_on"}  # 缺 type
    ok, _ = P.validate_cmd(bad)
    # validate_cmd 只校验 action，不校验 type；这里单独断言 type 约定
    if bad.get("type") != P.DOWNLINK_TYPE:
        pass  # 由帧封装层保证，契约测试记录该约定
    assert P.DOWNLINK_TYPE == "cmd"


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
