#pragma once
// qtapp/src/protocol_actions.h
//
// 协议词汇的 C++ 侧单一事实源，与 tools/protocol_schema.py 一一对应。
// 新增动作时必须三处同步：protocol_schema.py / 本文件 / 固件 Control_OnCommand，
// 并补 tools/contract_test.py 用例（get_state 是合法动作，query 不是）。

#include <QString>

namespace Proto {

// 下行 action
constexpr char ACTION_RELAY_ON[]  = "relay_on";
constexpr char ACTION_RELAY_OFF[] = "relay_off";
constexpr char ACTION_AUTO[]      = "auto";
constexpr char ACTION_SET_TH[]    = "set_th";
constexpr char ACTION_GET_STATE[] = "get_state";
constexpr char ACTION_ALARM_TEST[] = "alarm_test";   // 报警测试：蜂鸣器脉冲 1 s，不改模式

// 上行 type
constexpr char TYPE_REPORT[] = "report";
constexpr char TYPE_ACK[]    = "ack";
constexpr char TYPE_EVT[]    = "evt";
// 下行 type
constexpr char TYPE_CMD[]    = "cmd";

// 主动事件
constexpr char EVENT_ALARM[]   = "alarm";
constexpr char EVENT_RECOVER[] = "recover";

// 模式
constexpr int MODE_AUTO   = 0;
constexpr int MODE_MANUAL = 1;

// 量程与默认值（light 单位）——与固件 main.h / protocol_schema.py 同源
constexpr int ADC_MAX     = 4095;
constexpr int TH_DEFAULT  = 2000;
constexpr int HYS_DEFAULT = 150;

// 动作是否合法（替代历史误写 query）
inline bool isValidAction(const QString &a)
{
    return a == QLatin1String(ACTION_RELAY_ON)
        || a == QLatin1String(ACTION_RELAY_OFF)
        || a == QLatin1String(ACTION_AUTO)
        || a == QLatin1String(ACTION_SET_TH)
        || a == QLatin1String(ACTION_GET_STATE)
        || a == QLatin1String(ACTION_ALARM_TEST);
}

} // namespace Proto
