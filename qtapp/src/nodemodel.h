#ifndef NODEMODEL_H
#define NODEMODEL_H

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QList>

// 单节点状态。STM32 是权威状态源，本结构只保存"服务端给的状态"，
// 客户端（按钮/语音/大模型）绝不直接改写 —— 这是"服务端权威渲染"的核心。
struct NodeState
{
    int  nodeId = 0;
    bool online = false;
    bool silent = false;        // socket 未断但 3 s 无 report：疑似静默失效
    qint64 lastReportMs = 0;

    int adc = 0;
    int light = 0;
    int th = 0;
    int hys = 0;
    int relay = 0;
    int buzz = 0;
    int mode = 0;
};

class NodeModel : public QObject
{
    Q_OBJECT
public:
    explicit NodeModel(QObject *parent = nullptr);

    bool contains(int id) const;
    QList<int> nodeIds() const;
    NodeState &node(int id);                 // 不存在则创建
    const NodeState *peek(int id) const;

    // 应用 report 帧。applyFields 非空时只应用其中的字段
    // （ACK 未确认期间，relay/mode/buzz/th 等受控字段由 NodeServer 过滤掉）
    void applyReport(int id, const QJsonObject &o, qint64 recvMs,
                     const QSet<QString> &applyFields = QSet<QString>());
    // 应用 ack 帧：ack 携带完整状态且是当前命令的权威回执，全部应用
    void applyAck(int id, const QJsonObject &o, qint64 recvMs);

    void setOnline(int id, bool online);
    void markSilent(int id, bool silent);

signals:
    void nodeChanged(int id);

private:
    QHash<int, NodeState> m_nodes;
};

#endif // NODEMODEL_H
