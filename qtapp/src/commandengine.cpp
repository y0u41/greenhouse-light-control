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
        /* ★ 必须逐字镜像 Python 的**运算顺序**：先截断取整，再检查量程 ★
         *（截断语义的完整说明与坑位记录见 extractValue 的实现处） */
        const int n = extractValue(cmd);               /* 与 Python int() 同语义的截断 */
        if (n < 0 || n > Proto::ADC_MAX) {
            if (why != nullptr) {
                *why = QStringLiteral("阈值 %1 超出量程 0-4095").arg(n);
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

int CommandEngine::extractValue(const QJsonObject &cmd)
{
    /* ★ 必须逐字镜像 Python 的**运算顺序**：先截断取整 ★
     * 分叉修复（由 qtapp/tests/test_commandengine.cpp 的跨语言一致性用例定位）：
     *   QJsonValue::toInt() 对**非整数 double**（如 2000.9）**不转换、直接返回默认值 0**，
     *   于是 2000.9 被当成 0 -> 0 <= hys -> **拒绝**；
     *   而 Python 的 int(2000.9)=2000、固件 protocol.c 的 (int32_t)jval->valueint=2000
     *   —— 两者都**放行**。三套规则出现分歧，Qt 是离群的一方。
     * commandengine.h 承诺"与 tools/protocol_schema.py::validate_cmd 完全一致"，
     * 故此处对齐 Python 与固件的共同语义。
     *
     * ★ 顺序不能改 ★ 对应 protocol_schema.py:93-97：
     *       v = int(value)                       # 先截断（向零取整）
     *       if v < 0 or v > ADC_MAX: 拒绝        # 再检查量程
     *       if v <= hys: 拒绝
     * 若把量程检查改到 double 域（写成 `if (d < 0.0 ...)`）会**引入新的分叉**：
     * value = -0.5 时 Python 的 int(-0.5) = 0 落在量程内、继续往下判，
     * 而 double 域会直接以"超出量程"拒绝 —— 判定结果相反。
     * 这个坑我第一版就踩了，由 parityWithPythonSchema 用例抓出来。
     *
     * 超大值先夹到 int 可表示范围：仅为避免 static_cast 的未定义行为，
     * 夹取后必然超量程、判定不变（Python 侧是大整数比较，同样拒绝）。
     *
     * 缺 value 字段时 toDouble() 返回 0 —— 与"无参数动作"的语义一致，
     * 由 validate 先检查 contains("value")，extractValue 只在有字段时被调用。 */
    double dc = cmd.value(QStringLiteral("value")).toDouble();
    if (dc > 2147483647.0)  { dc = 2147483647.0; }
    if (dc < -2147483648.0) { dc = -2147483648.0; }
    return static_cast<int>(dc);                        /* 与 Python int() 同语义的截断 */
}
