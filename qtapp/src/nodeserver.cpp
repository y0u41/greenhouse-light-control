#include "nodeserver.h"
#include "commandengine.h"
#include "protocol_actions.h"

#include <QDateTime>
#include <QJsonDocument>

NodeServer::NodeServer(NodeModel *model, CommandAckGuard *guard, CsvLogger *logger, QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_guard(guard)
    , m_logger(logger)
{
    connect(&m_server, &QTcpServer::newConnection, this, &NodeServer::onNewConnection);
    connect(m_guard, &CommandAckGuard::ackTimeout, this, &NodeServer::onAckTimeout);

    m_offlineTimer.setInterval(1000);
    connect(&m_offlineTimer, &QTimer::timeout, this, &NodeServer::onOfflineTick);
    m_offlineTimer.start();
}

bool NodeServer::start(const QString &bindAddr, quint16 port, QString *err)
{
    if (m_server.isListening()) {
        return true;
    }
    const QHostAddress addr(bindAddr);
    if (!m_server.listen(addr, port)) {
        if (err != nullptr) {
            *err = m_server.errorString();
        }
        return false;
    }
    emit logMessage(QStringLiteral("NodeServer 监听 %1:%2").arg(bindAddr).arg(port));
    return true;
}

void NodeServer::stop()
{
    const auto socks = m_sockets.values();
    for (QTcpSocket *s : socks) {
        s->abort();
    }
    m_server.close();
}

bool NodeServer::isListening() const
{
    return m_server.isListening();
}

int NodeServer::connectedNodeCount() const
{
    int n = 0;
    for (auto it = m_sockets.constBegin(); it != m_sockets.constEnd(); ++it) {
        if (it.value() != nullptr && it.value()->state() == QAbstractSocket::ConnectedState) {
            n++;
        }
    }
    return n;
}

void NodeServer::sendCommand(int nodeId, const QString &action, int value)
{
    QTcpSocket *s = m_sockets.value(nodeId, nullptr);
    if (s == nullptr || s->state() != QAbstractSocket::ConnectedState) {
        emit commandFailed(nodeId, action,
                           QStringLiteral("节点 %1 不在线，指令未下发").arg(nodeId));
        return;
    }

    const QJsonObject o = CommandEngine::buildCmd(nodeId, action, value);
    m_pendingNode = nodeId;
    m_guard->issue(action, value);
    s->write(QJsonDocument(o).toJson(QJsonDocument::Compact) + QByteArrayLiteral("\n"));
    s->flush();
    emit logMessage(QStringLiteral("下发 → 节点 %1：%2").arg(nodeId).arg(action));
}

void NodeServer::onNewConnection()
{
    while (QTcpSocket *s = m_server.nextPendingConnection()) {
        m_bufs.insert(s, QByteArray());
        connect(s, &QTcpSocket::readyRead, this, &NodeServer::onReadyRead);
        connect(s, &QTcpSocket::disconnected, this, &NodeServer::onDisconnected);
        connect(s, &QAbstractSocket::errorOccurred, this, &NodeServer::onSocketError);
        emit logMessage(QStringLiteral("新接入：%1:%2")
                            .arg(s->peerAddress().toString()).arg(s->peerPort()));
    }
}

void NodeServer::onReadyRead()
{
    QTcpSocket *s = qobject_cast<QTcpSocket *>(sender());
    if (s == nullptr) {
        return;
    }
    m_bufs[s].append(s->readAll());

    /* ★ 先拆帧、再处理残段超限（顺序不能反）：
     * 一次 readyRead 里可能积压一大批**完整合法帧**（例如 TCP 突发把多个
     * 1 Hz 上报攒在一起），旧逻辑先判"缓冲 > 8192 就清空"，会把这一批帧
     * **全部**丢掉——包括那些本该正常处理的。正确顺序：先把带 '\n' 的完整帧
     * 处理完；剩下的"无换行残段"才是真正的垃圾风险，才做超限丢弃。 */
    for (;;) {
        const int idx = m_bufs[s].indexOf('\n');
        if (idx < 0) {
            break;
        }
        const QByteArray line = m_bufs[s].left(idx).trimmed();
        m_bufs[s].remove(0, idx + 1);
        if (!line.isEmpty()) {
            handleFrame(s, line);
        }
    }

    // 单连接**残段**上限：对端持续发"无换行"数据时不能无限增长。
    // 固件侧对无换行数据的上限是 PROTOCOL_FRAME_MAX(200 B)——超长行被丢弃至行尾
    // （见 protocol.c 的 s_line_overlong）；服务端这里对称设限，但要允许
    // "突发积压多帧后被拆走"的合法场景，故取 8192。
    static constexpr int kMaxRxBuffer = 8192;
    if (m_bufs[s].size() > kMaxRxBuffer) {
        emit logMessage(QStringLiteral("接收残段超限（%1 B），已丢弃：%2:%3")
                            .arg(m_bufs[s].size())
                            .arg(s->peerAddress().toString())
                            .arg(s->peerPort()));
        m_bufs[s].clear();
    }
}

void NodeServer::handleFrame(QTcpSocket *sock, const QByteArray &line)
{
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        emit logMessage(QStringLiteral("丢弃非 JSON 帧：%1").arg(QString::fromUtf8(line)));
        return;
    }
    const QJsonObject o = doc.object();

    const int nodeId = o.value(QStringLiteral("node")).toInt(-1);
    if (nodeId < 0) {
        emit logMessage(QStringLiteral("丢弃缺 node 字段的帧：%1").arg(QString::fromUtf8(line)));
        return;
    }
    learnNode(sock, nodeId);

    const QString type = o.value(QStringLiteral("type")).toString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (type == QLatin1String(Proto::TYPE_REPORT)) {
        /* ★ 历史数据落盘在**这里**，三个理由：
         *  1) 这里是"帧刚被解析出来"的唯一位置（原始 JSON + 服务端收到时刻），
         *     落盘写的是**收到的事实**，而不是"界面最终显示的状态"；
         *  2) 必须在下面的 ACK 守卫过滤**之前**：守卫只是"ACK 未确认期间不让旧
         *     report 覆盖界面"，与"数据是否值得留档"无关。放后面会随守卫一起把
         *     受控字段（relay/mode/buzz/th）丢出 CSV —— 那就是数据丢失；
         *  3) 不放 NodeModel::applyReport：那是**应用状态**的地方（有字段过滤、
         *     还会被 ack/evt 复用，而 evt 帧只有 adc/light），落盘属于传输层职责。 */
        if (m_logger != nullptr) {
            m_logger->appendReport(o, now);
        }

        // ACK 未确认期间，受控字段（relay/mode/buzz/th）不让旧 report 回写，
        // 只放行 light/adc/hys —— 这就是"服务端权威渲染"的竞态防护。
        QSet<QString> fields;
        if (m_guard->isAckPending()) {
            fields = {QStringLiteral("light"), QStringLiteral("adc"), QStringLiteral("hys")};
        }
        m_model->applyReport(nodeId, o, now, fields);
        emit nodeDataChanged(nodeId);

    } else if (type == QLatin1String(Proto::TYPE_ACK)) {
        const QString action = o.value(QStringLiteral("action")).toString();
        const QString result = o.value(QStringLiteral("result")).toString();
        m_model->applyAck(nodeId, o, now);       // ack 是当前命令的权威回执
        m_guard->onAck(action, result);
        m_pendingNode = -1;
        emit nodeAckReceived(nodeId, action, result);
        emit nodeDataChanged(nodeId);

    } else if (type == QLatin1String(Proto::TYPE_EVT)) {
        const QString event = o.value(QStringLiteral("event")).toString();
        // evt 证明链路活着，刷新时间戳，避免被误判为静默失效
        m_model->applyReport(nodeId, o, now);
        emit logMessage(QStringLiteral("节点 %1 事件：%2").arg(nodeId).arg(event));
        emit nodeDataChanged(nodeId);

    } else {
        emit logMessage(QStringLiteral("忽略未知 type：%1").arg(type));
    }
}

void NodeServer::learnNode(QTcpSocket *sock, int nodeId)
{
    if (!m_sockNode.contains(sock)) {
        m_sockNode.insert(sock, nodeId);
    }
    if (m_sockets.value(nodeId, nullptr) != sock) {
        /* 同一 nodeId 被新连接认领：先断开旧 socket，避免两条连接互相争抢
         * 路由（旧连接的 readyRead 仍会触发 learnNode，把 nodeId 又抢回去，
         * 造成新旧连接"乒乓"交替占有）。abort() 触发 disconnected ->
         * forgetSocket，由它清理旧 socket 的缓冲与路由残留。 */
        if (QTcpSocket *old = m_sockets.value(nodeId, nullptr)) {
            if (old != nullptr && old != sock) {
                emit logMessage(QStringLiteral("节点 %1 被新连接接管，断开旧连接").arg(nodeId));
                old->abort();
            }
        }
        m_sockets.insert(nodeId, sock);
    }
    if (!m_model->node(nodeId).online) {
        m_model->setOnline(nodeId, true);
        emit logMessage(QStringLiteral("节点 %1 在线").arg(nodeId));
    }
}

void NodeServer::forgetSocket(QTcpSocket *sock)
{
    const int nodeId = m_sockNode.take(sock);
    m_bufs.remove(sock);
    if (nodeId > 0 && m_sockets.value(nodeId, nullptr) == sock) {
        m_sockets.remove(nodeId);
        m_model->setOnline(nodeId, false);
        emit nodeOffline(nodeId);
        emit logMessage(QStringLiteral("节点 %1 离线").arg(nodeId));
    }
}

void NodeServer::onDisconnected()
{
    QTcpSocket *s = qobject_cast<QTcpSocket *>(sender());
    if (s != nullptr) {
        forgetSocket(s);
        s->deleteLater();
    }
}

void NodeServer::onSocketError(QAbstractSocket::SocketError)
{
    QTcpSocket *s = qobject_cast<QTcpSocket *>(sender());
    if (s != nullptr) {
        emit logMessage(QStringLiteral("socket 错误：%1").arg(s->errorString()));
    }
}

void NodeServer::onOfflineTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<int> ids = m_model->nodeIds();
    for (int id : ids) {
        const NodeState *st = m_model->peek(id);
        if (st == nullptr) {
            continue;
        }
        if (!st->online) {
            continue;
        }
        const bool silent = (now - st->lastReportMs) > m_offlineTimeoutMs;
        if (silent && !st->silent) {
            m_model->markSilent(id, true);
            emit nodeSilent(id);
            emit logMessage(QStringLiteral("节点 %1 链路异常：%2 ms 未上报")
                                .arg(id).arg(now - st->lastReportMs));
        } else if (!silent && st->silent) {
            m_model->markSilent(id, false);
        }
    }
}

void NodeServer::onAckTimeout(const QString &action)
{
    const int nodeId = m_pendingNode;
    m_pendingNode = -1;
    emit commandFailed(nodeId, action,
                       QStringLiteral("指令 %1 执行超时（%2 ms 未收到 ack）")
                           .arg(action).arg(m_guard->ackTimeoutMs()));
}
