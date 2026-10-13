#ifndef COMMANDENGINE_H
#define COMMANDENGINE_H

#include <QObject>
#include <QJsonObject>
#include <QString>

#include "nodeserver.h"
#include "nodemodel.h"

// 指令引擎：按钮 / 语音 / 大模型三路指令的统一入口与校验。
// 校验规则与 tools/protocol_schema.py::validate_cmd 完全一致（含吃实时 hys），
// UI 侧不得自行再写一套判断，避免规则分叉。
class CommandEngine : public QObject
{
    Q_OBJECT
public:
    CommandEngine(NodeServer *server, NodeModel *model, QObject *parent = nullptr);

    // 三路统一入口：返回 false 时 why 给出中文原因（不下发）
    bool submit(int nodeId, const QString &action, int value, QString *why = nullptr);

    static bool validate(const QJsonObject &cmd, int hys, QString *why = nullptr);
    static QJsonObject buildCmd(int nodeId, const QString &action, int value = -1);

    /* 与 validate 同语义的 value 提取：先截断取整（向零），超大值夹到 int 范围。
     * ★ 调用方不得直接用 QJsonValue::toInt()：它对**非整数 double**（如 2500.9）
     * 不转换、直接返回默认值 0 —— 与 validate 的截断语义分叉（2500.9 会变成
     * value=0 被二次校验拦下）。MainWindow 的 LLM 指令路径必须走本函数。 */
    static int extractValue(const QJsonObject &cmd);

signals:
    void accepted(int nodeId, const QString &action, int value);
    void rejected(const QString &why);

private:
    NodeServer *m_server;
    NodeModel  *m_model;
};

#endif // COMMANDENGINE_H
