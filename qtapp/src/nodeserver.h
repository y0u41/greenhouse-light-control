#ifndef NODESERVER_H
#define NODESERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QTimer>
#include <QSet>

#include "nodemodel.h"
#include "command_ack_guard.h"
#include "csvlogger.h"

// TCP 服务：监听 9092，多节点接入、行分隔 JSON 解析、离线判定、ACK 守卫接入点。
class NodeServer : public QObject
{
    Q_OBJECT
public:
    /* logger 可为 nullptr（单测/无落盘场景）。落盘点选在 handleFrame 的 report 分支，
     * 而不是 NodeModel::applyReport —— 理由见 nodeserver.cpp 里的长注释。 */
    explicit NodeServer(NodeModel *model, CommandAckGuard *guard,
                        CsvLogger *logger = nullptr, QObject *parent = nullptr);

    bool start(const QString &bindAddr, quint16 port, QString *err = nullptr);
    void stop();
    bool isListening() const;
    int  connectedNodeCount() const;

    void setOfflineTimeoutMs(int ms) { m_offlineTimeoutMs = ms; }
    int  offlineTimeoutMs() const { return m_offlineTimeoutMs; }

    // 指令下发（CommandEngine 校验后调用）
    void sendCommand(int nodeId, const QString &action, int value);

signals:
    void nodeDataChanged(int id);
    void nodeAckReceived(int id, const QString &action, const QString &result);
    void nodeOffline(int id);
    void nodeSilent(int id);                          // socket 未断但超时无 report
    void commandFailed(int nodeId, const QString &action, const QString &reason);
    void logMessage(const QString &msg);

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void onSocketError(QAbstractSocket::SocketError);
    void onOfflineTick();
    void onAckTimeout(const QString &action);

private:
    void handleFrame(QTcpSocket *sock, const QByteArray &line);
    void learnNode(QTcpSocket *sock, int nodeId);
    void forgetSocket(QTcpSocket *sock);

    NodeModel *m_model;
    CommandAckGuard *m_guard;
    CsvLogger *m_logger = nullptr;   // 历史数据落盘（可为 nullptr）
    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_bufs;
    QHash<QTcpSocket *, int> m_sockNode;
    QHash<int, QTcpSocket *> m_sockets;
    QTimer m_offlineTimer;
    int m_offlineTimeoutMs = 3000;   // 静默失效判定，默认 3 × 上报周期
    int m_pendingNode = -1;      // 当前待 ACK 的节点（守卫只维护单条 pending）
};

#endif // NODESERVER_H
