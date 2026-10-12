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

signals:
    void accepted(int nodeId, const QString &action, int value);
    void rejected(const QString &why);

private:
    NodeServer *m_server;
    NodeModel  *m_model;
};

#endif // COMMANDENGINE_H
