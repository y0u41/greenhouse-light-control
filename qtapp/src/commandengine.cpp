#include "commandengine.h"
#include "protocol_actions.h"

CommandEngine::CommandEngine(NodeServer *server, NodeModel *model, QObject *parent)
    : QObject(parent)
    , m_server(server)
    , m_model(model)
{
}

bool CommandEngine::submit(int nodeId, const QString &action, int value, QString *why)
{
    const QJsonObject cmd = buildCmd(nodeId, action, value);

    // 回差取当前节点实时值；节点尚未上线时按默认值（与固件 HYS_DEFAULT 同源）
    int hys = Proto::HYS_DEFAULT;
    if (const NodeState *st = m_model->peek(nodeId)) {
        if (st->hys > 0) {
            hys = st->hys;
        }
    }

    if (!validate(cmd, hys, why)) {
        emit rejected((why != nullptr) ? *why : QStringLiteral("指令校验失败"));
        return false;
    }

    m_server->sendCommand(nodeId, action, value);
    emit accepted(nodeId, action, value);
    return true;
}

bool CommandEngine::validate(const QJsonObject &cmd, int hys, QString *why)
{
    const QString action = cmd.value(QStringLiteral("action")).toString();

    if (action.isEmpty()) {
        if (why != nullptr) {
            *why = QStringLiteral("缺少 action 字段");
        }
        return false;
    }
    if (!Proto::isValidAction(action)) {
        if (why != nullptr) {
            *why = QStringLiteral("未知动作：%1").arg(action);
        }
        return false;
    }

    if (action == QLatin1String(Proto::ACTION_SET_TH)) {
        if (!cmd.contains(QStringLiteral("value"))) {
            if (why != nullptr) {
                *why = QStringLiteral("set_th 缺少数值参数 value");
            }
            return false;
        }
        const QJsonValue v = cmd.value(QStringLiteral("value"));
        if (!v.isDouble()) {
            if (why != nullptr) {
                *why = QStringLiteral("set_th 的 value 不是数字");
            }
            return false;
        }
        const int n = v.toInt();
        if (n < 0 || n > Proto::ADC_MAX) {
            if (why != nullptr) {
                *why = QStringLiteral("阈值 %1 超出 ADC 量程 0-4095").arg(n);
            }
            return false;
        }
        if (n <= hys) {
            if (why != nullptr) {
                *why = QStringLiteral("阈值 %1 必须大于回差 %2").arg(n).arg(hys);
            }
            return false;
        }
        return true;
    }

    return true;    // relay_on/relay_off/auto/get_state/alarm_test 无需参数
}

QJsonObject CommandEngine::buildCmd(int nodeId, const QString &action, int value)
{
    QJsonObject o;
    o.insert(QStringLiteral("node"), nodeId);
    o.insert(QStringLiteral("type"), QLatin1String(Proto::TYPE_CMD));
    o.insert(QStringLiteral("action"), action);
    if (action == QLatin1String(Proto::ACTION_SET_TH)) {
        o.insert(QStringLiteral("value"), value);
    }
    return o;
}
