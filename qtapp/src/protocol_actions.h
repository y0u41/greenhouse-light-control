#pragma once
// qtapp/src/protocol_actions.h
//
// 协议词汇的 C++ 侧单一事实源，与 tools/protocol_schema.py 一一对应。
// 新增动作时必须三处同步：protocol_schema.py / 本文件 / 固件 Control_OnCommand，
// 并补 tools/contract_test.py 用例（get_state 是合法动作，query 不是）。

#include <QString>
#include <QStringList>

namespace Proto {

// 下行 action
constexpr char ACTION_RELAY_ON[]  = "relay_on";
constexpr char ACTION_RELAY_OFF[] = "relay_off";
constexpr char ACTION_AUTO[]      = "auto";
constexpr char ACTION_SET_TH[]    = "set_th";
constexpr char ACTION_GET_STATE[] = "get_state";
constexpr char ACTION_ALARM_TEST[] = "alarm_test";   // 报警测试：蜂鸣器脉冲 1 s，不改模式

// ★ 允许动作的全集，**唯一的数组式清单** ★
//
// 为什么要有一个数组：System Prompt 必须把"合法动作白名单"逐字列给模型
// （实测：不列全时模型会自造 set_threshold / threshold 之类的名字，而
// CommandEngine::validate 会把这些判为"未知动作"直接拒收，表现为"用户按了没反应"）。
// 若在白名单处再手抄一份动作名，就出现第二事实源、必然漂移 —— 故此处统一
// 从 ACTION_* 常量取，Tools/consistency_check.py 也按同样的数组写法解析本文件。
inline const char *const *allActions(int *count)
{
    static const char *const ALL[] = {
        ACTION_RELAY_ON,
        ACTION_RELAY_OFF,
        ACTION_AUTO,
        ACTION_SET_TH,
        ACTION_GET_STATE,
        ACTION_ALARM_TEST,
    };
    if (count != nullptr) {
        *count = static_cast<int>(sizeof(ALL) / sizeof(ALL[0]));
    }
    return ALL;
}

inline QStringList allActionNames()
{
    int n = 0;
    const char *const *a = allActions(&n);
    QStringList out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        out.append(QLatin1String(a[i]));
    }
    return out;
}

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

// 动作的中文含义 + 输出形状，供 System Prompt 生成白名单条目（llmclient.cpp 消费）。
// 与 isValidAction 同源：这里的每一条都必须对应 ACTION_* 之一，顺序也一致。
inline QString actionLine(int index)
{
    static const char *const LINES[] = {
        "  用 {\"action\":\"relay_on\"}   手动吸合继电器（副作用：切到手动模式）",
        "  用 {\"action\":\"relay_off\"}  手动释放继电器（副作用：切到手动模式）",
        "  用 {\"action\":\"auto\"}        切回自动模式",
        "  用 {\"action\":\"set_th\",\"value\":N}  把光强阈值设为 N（必须同时给 value）",
        "  用 {\"action\":\"get_state\"}   查询当前状态",
        "  用 {\"action\":\"alarm_test\"}  报警测试（蜂鸣器响 1 秒，不改模式）",
    };
    const int n = static_cast<int>(sizeof(LINES) / sizeof(LINES[0]));
    if (index < 0 || index >= n) {
        return QString();
    }
    return QString::fromUtf8(LINES[index]);
}

} // namespace Proto
