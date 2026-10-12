// qtapp/src/command_ack_guard.cpp
// 见 command_ack_guard.h 顶部说明。实现 ACK 守卫状态机。

#include "command_ack_guard.h"

const QSet<QString> CommandAckGuard::CONTROLLED_FIELDS = {
    QStringLiteral("relay"),
    QStringLiteral("mode"),
    QStringLiteral("buzz"),
    QStringLiteral("th"),
};

CommandAckGuard::CommandAckGuard(QObject *parent, int ackTimeoutMs)
    : QObject(parent), m_ackTimeoutMs(ackTimeoutMs)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);          // 单次：等 ACK，不等周期
    connect(m_timer, &QTimer::timeout, this, &CommandAckGuard::onTimeout);
}

void CommandAckGuard::issue(const QString &action, int value)
{
    m_pending = true;
    m_pendingAction = action;
    m_pendingValue = value;
    m_timer->start(m_ackTimeoutMs);        // 重新下发重置计时
}

bool CommandAckGuard::onAck(const QString &action, const QString &result)
{
    /* 守卫只负责"这条 ack 是否对应待确认命令"，不解释 result 的业务含义。
     * 但要注意：固件的 "err" 语义是**指令未生效**（参数非法或写盘失败，
     * 且固件侧已回滚状态）。因此 err 也必须清 pending —— 否则守卫会一直屏蔽
     * 受控字段的 report 更新，界面反而卡在"指令执行中"。
     * 对 err 的界面处理（提示失败、不播报成功）由 onNodeAckReceived 负责。 */
    Q_UNUSED(result);
    if (!m_pending)
        return false;                       //  stray ack，无待确认命令
    if (action != m_pendingAction)
        return false;                       // 动作不匹配，忽略（避免错清 pending）

    m_pending = false;
    m_pendingAction.clear();
    m_timer->stop();
    return true;
}

bool CommandAckGuard::shouldApplyReportField(const QString &field) const
{
    // pending 期间屏蔽受控字段；非受控字段（light/adc）始终实时刷新
    if (!m_pending)
        return true;
    return !CONTROLLED_FIELDS.contains(field);
}

void CommandAckGuard::setAckTimeoutMs(int ms)
{
    m_ackTimeoutMs = ms;
}

void CommandAckGuard::onTimeout()
{
    if (!m_pending)
        return;
    const QString acted = m_pendingAction;  // 先存，信号里不要再读成员
    m_pending = false;
    m_pendingAction.clear();
    emit ackTimeout(acted);                 // UI：标记"指令执行中→超时"
}
