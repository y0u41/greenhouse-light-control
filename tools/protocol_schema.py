# tools/protocol_schema.py
# 通信协议的"机器可读单一事实源"。
#
# 目的：消除 Tech_Design.md 里反复出现的"三处必须一致"人工同步风险。
#       固件与 Qt 的指令校验逻辑都必须与本文件保持一致；
#       tools/contract_test.py 在改动任一侧后运行，防止 get_state/query 类漂移。
#
# 本文件只定义"契约"，不依赖任何固件/Qt 运行时，纯标准库，可被 pytest 直接 import。

# ---- 动作词汇（下行 action，唯一合法取值）----
ACTION_RELAY_ON = "relay_on"
ACTION_RELAY_OFF = "relay_off"
ACTION_AUTO = "auto"
ACTION_SET_TH = "set_th"
ACTION_GET_STATE = "get_state"
# 报警测试：蜂鸣器脉冲 1 s，不改模式与执行器（固件 Control_OnCommand 分支）
ACTION_ALARM_TEST = "alarm_test"

# 合法动作集合。新增动作必须同时加进这里、固件 Control_OnCommand 分支、
# Qt protocol_actions.h 与 CommandEngine::validate，并补 contract_test。
ACTION_SET = frozenset({
    ACTION_RELAY_ON,
    ACTION_RELAY_OFF,
    ACTION_AUTO,
    ACTION_SET_TH,
    ACTION_GET_STATE,
    ACTION_ALARM_TEST,
})

# ---- 上行 type ----
TYPE_REPORT = "report"
TYPE_ACK = "ack"
TYPE_EVT = "evt"
UPLINK_TYPE_SET = frozenset({TYPE_REPORT, TYPE_ACK, TYPE_EVT})

# 下行帧统一用 type="cmd" + action 字段
DOWNLINK_TYPE = "cmd"

# ---- 主动事件 event ----
EVENT_ALARM = "alarm"
EVENT_RECOVER = "recover"
EVENT_SET = frozenset({EVENT_ALARM, EVENT_RECOVER})

# ---- 模式 ----
MODE_AUTO = 0
MODE_MANUAL = 1

# ---- 量程与默认值（light 单位）----
ADC_MAX = 4095
LIGHT_MAX = 4095
TH_DEFAULT = 2000
HYS_DEFAULT = 150

# 不变式：0 <= hys < th <= 4095
INVARIANT_TH_MAX = 4095


def is_valid_action(action) -> bool:
    """动作名是否合法（替代 'query' 等历史误写）。"""
    return action in ACTION_SET


def is_valid_uplink_type(t) -> bool:
    return t in UPLINK_TYPE_SET


def is_valid_event(event) -> bool:
    return event in EVENT_SET


def validate_cmd(obj, hys: int = HYS_DEFAULT):
    """校验一条下行指令，与 Tech_Design.md §指令引擎校验规则 完全一致。

    返回 (ok: bool, why: str)。
    固件 Control_OnCommand 与 Qt CommandEngine::validate 都必须产生与此相同的结果。
    """
    if not isinstance(obj, dict):
        return False, "指令不是 JSON 对象"

    action = obj.get("action")
    if action is None:
        return False, "缺少 action 字段"
    if action not in ACTION_SET:
        # 关键不变式：'query' 不是合法动作名，统一用 get_state
        return False, "未知动作：%s" % action

    if action == ACTION_SET_TH:
        if "value" not in obj:
            return False, "set_th 缺少数值参数 value"
        value = obj.get("value")
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            return False, "set_th 的 value 不是数字"
        v = int(value)
        if v < 0 or v > ADC_MAX:
            return False, "阈值 %d 超出量程 0-4095" % v
        if v <= hys:
            return False, "阈值 %d 必须大于回差 %d" % (v, hys)
        return True, "ok"

    # 其余动作无需参数
    return True, "ok"


def build_example_frames():
    """返回 Tech_Design.md 示例帧，供契约测试做往返校验。"""
    return [
        '{"node":1,"type":"report","adc":1200,"light":2895,"th":2000,"hys":150,"relay":1,"buzz":1,"mode":0}',
        '{"node":1,"type":"cmd","action":"relay_off"}',
        '{"node":1,"type":"cmd","action":"auto"}',
        '{"node":1,"type":"cmd","action":"set_th","value":2500}',
        '{"node":1,"type":"cmd","action":"get_state"}',
        '{"node":1,"type":"ack","action":"set_th","result":"ok","adc":1200,"light":2895,"th":2500,"hys":150,"relay":0,"buzz":0,"mode":0}',
        '{"node":1,"type":"evt","event":"alarm","adc":1200,"light":2895}',
    ]


if __name__ == "__main__":
    # 自测：跑一遍契约核心不变量
    assert is_valid_action(ACTION_GET_STATE)
    assert not is_valid_action("query")          # get_state/query 漂移防护
    assert validate_cmd({"action": "relay_on"})[0]
    assert not validate_cmd({"action": "set_th", "value": 100})[0]   # <= hys
    assert not validate_cmd({"action": "set_th", "value": 5000})[0]  # 超量程
    assert validate_cmd({"action": "set_th", "value": 2500})[0]
    print("protocol_schema self-check OK")
