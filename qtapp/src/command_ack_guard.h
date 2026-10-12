#pragma once
// qtapp/src/command_ack_guard.h
//
// 指令 ACK 守卫：解决"服务端权威 / 客户端只渲染"的竞态（P0-2）。
//
// 问题：NodeModel 同时被 report 帧与 ack 帧更新。用户点"关继电器"后，命令在飞，
//       一条旧的 report(relay=1) 先到会让 UI 闪回"开"。
// 职责：
//   1) 下发指令后置 pending，启动 ACK 超时定时器；
//   2) 收到匹配 ack 才清 pending；超时则上报 ackTimeout 让 UI 提示失败；
//   3) 暴露 shouldApplyReportField()，pending 期间 NodeModel 不应用受控字段
//      （relay/mode/buzz/th），避免 report 回写覆盖未确认的命令结果。
//
// 集成位置（qtapp 脚手架落地后）：
//   - NodeServer::sendCommand() 下发前调用 issue(action, value)
//   - NodeServer::onFrame(ack)   收到 ack 时调用 onAck(action, result)
//   - NodeModel 在应用 report 帧前，对每个受控字段先问 shouldApplyReportField(field)
//
// 本文件是参考模块，编译需 Qt6 + 已存在的 CMakeLists；不依赖固件侧任何符号。

#include <QObject>
#include <QTimer>
#include <QString>
#include <QSet>

class CommandAckGuard : public QObject
{
    Q_OBJECT
public:
    explicit CommandAckGuard(QObject *parent = nullptr, int ackTimeoutMs = 3000);

    // 下发前调用：记录待确认命令并启动超时定时器
    void issue(const QString &action, int value = -1);

    // 收到 ack 时调用；返回 true 表示本次 ack 匹配并清掉了 pending
    bool onAck(const QString &action, const QString &result);

    bool isAckPending() const { return m_pending; }
    QString pendingAction() const { return m_pendingAction; }
    int ackTimeoutMs() const { return m_ackTimeoutMs; }

    // pending 期间，report 帧里的受控字段不应覆盖本地状态
    bool shouldApplyReportField(const QString &field) const;

signals:
    void ackTimeout(const QString &action);   // UI 提示"指令执行超时/失败"用

public slots:
    void setAckTimeoutMs(int ms);

private slots:
    void onTimeout();

private:
    static const QSet<QString> CONTROLLED_FIELDS;  // relay/mode/buzz/th

    QTimer *m_timer;
    bool m_pending = false;
    QString m_pendingAction;
    int m_pendingValue = -1;
    int m_ackTimeoutMs;
};
