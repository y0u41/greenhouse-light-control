#include "nodemodel.h"
#include "protocol_actions.h"

NodeModel::NodeModel(QObject *parent)
    : QObject(parent)
{
}

bool NodeModel::contains(int id) const
{
    return m_nodes.contains(id);
}

QList<int> NodeModel::nodeIds() const
{
    return m_nodes.keys();
}

NodeState &NodeModel::node(int id)
{
    if (!m_nodes.contains(id)) {
        NodeState st;
        st.nodeId = id;
        m_nodes.insert(id, st);
    }
    return m_nodes[id];
}

const NodeState *NodeModel::peek(int id) const
{
    auto it = m_nodes.constFind(id);
    return (it != m_nodes.constEnd()) ? &it.value() : nullptr;
}

void NodeModel::applyReport(int id, const QJsonObject &o, qint64 recvMs,
                            const QSet<QString> &applyFields)
{
    NodeState &st = node(id);
    st.lastReportMs = recvMs;

    const auto allowed = [&applyFields](const QString &key) {
        return applyFields.isEmpty() || applyFields.contains(key);
    };

    if (allowed(QStringLiteral("adc")) && o.contains(QLatin1String("adc"))) {
        st.adc = o.value(QLatin1String("adc")).toInt();
    }
    if (allowed(QStringLiteral("light")) && o.contains(QLatin1String("light"))) {
        st.light = o.value(QLatin1String("light")).toInt();
    }
    if (allowed(QStringLiteral("th")) && o.contains(QLatin1String("th"))) {
        st.th = o.value(QLatin1String("th")).toInt();
    }
    if (allowed(QStringLiteral("hys")) && o.contains(QLatin1String("hys"))) {
        st.hys = o.value(QLatin1String("hys")).toInt();
    }
    if (allowed(QStringLiteral("relay")) && o.contains(QLatin1String("relay"))) {
        st.relay = o.value(QLatin1String("relay")).toInt();
    }
    if (allowed(QStringLiteral("buzz")) && o.contains(QLatin1String("buzz"))) {
        st.buzz = o.value(QLatin1String("buzz")).toInt();
    }
    if (allowed(QStringLiteral("mode")) && o.contains(QLatin1String("mode"))) {
        st.mode = o.value(QLatin1String("mode")).toInt();
    }

    emit nodeChanged(id);
}

void NodeModel::applyAck(int id, const QJsonObject &o, qint64 recvMs)
{
    NodeState &st = node(id);
    st.lastReportMs = recvMs;

    if (o.contains(QLatin1String("adc"))) {
        st.adc = o.value(QLatin1String("adc")).toInt();
    }
    if (o.contains(QLatin1String("light"))) {
        st.light = o.value(QLatin1String("light")).toInt();
    }
    if (o.contains(QLatin1String("th"))) {
        st.th = o.value(QLatin1String("th")).toInt();
    }
    if (o.contains(QLatin1String("hys"))) {
        st.hys = o.value(QLatin1String("hys")).toInt();
    }
    if (o.contains(QLatin1String("relay"))) {
        st.relay = o.value(QLatin1String("relay")).toInt();
    }
    if (o.contains(QLatin1String("buzz"))) {
        st.buzz = o.value(QLatin1String("buzz")).toInt();
    }
    if (o.contains(QLatin1String("mode"))) {
        st.mode = o.value(QLatin1String("mode")).toInt();
    }

    emit nodeChanged(id);
}

void NodeModel::setOnline(int id, bool online)
{
    NodeState &st = node(id);
    if (st.online != online) {
        st.online = online;
        if (!online) {
            st.silent = false;
        }
        emit nodeChanged(id);
    }
}

void NodeModel::markSilent(int id, bool silent)
{
    NodeState &st = node(id);
    if (st.silent != silent) {
        st.silent = silent;
        emit nodeChanged(id);
    }
}
