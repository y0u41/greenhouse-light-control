#include "llmclient.h"
#include "protocol_actions.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

LlmClient::LlmClient(QObject *parent)
    : QObject(parent)
{
    // QNetworkAccessManager 在本对象所在线程创建 —— 调用方必须保证本对象
    // 运行在工作线程内（见头文件的线程纪律）。
    m_nam = new QNetworkAccessManager(this);
}

void LlmClient::configure(const QString &url, const QString &model,
                          const QString &token, int timeoutMs)
{
    m_url = url;
    m_modelsUrl = QUrl(url).adjusted(QUrl::RemoveFilename).toString()
                  + QStringLiteral("models");
    m_model = model;
    m_token = token;
    m_timeoutMs = (timeoutMs > 0) ? timeoutMs : 15000;
}

QString LlmClient::buildSystemPrompt(const QString &stateJson) const
{
    // 提示词与协议、固件三处一致（Tech_Design §System Prompt）：
    //   ① 动作名统一 get_state，不得出现 query；
    //   ② 示例语义与固件 light > th 判断一致："光太强 → relay_on（遮阳）"，
    //      不要写成"太暗了开灯"；
    //   ③ relay_on/relay_off 会切入手动模式，需在提示中说明。
    return QStringLiteral(
        "你是智能温室控制助手。根据用户指令，只输出 JSON，不要输出任何其他文字，不要解释。\n"
        "\n"
        "继电器语义：光照过强时吸合继电器遮阳/报警，光照恢复正常后释放。\n"
        "relay_on / relay_off 会切换到手动模式，auto 才切回自动模式。\n"
        "\n"
        "可用动作：\n"
        "  {\"action\":\"relay_on\"}            手动吸合继电器（进入手动模式）\n"
        "  {\"action\":\"relay_off\"}           手动释放继电器（进入手动模式）\n"
        "  {\"action\":\"auto\"}                切回自动模式\n"
        "  {\"action\":\"set_th\",\"value\":N}   将光强阈值设为 N（0-4095，且必须大于当前回差）\n"
        "  {\"action\":\"alarm_test\"}          报警测试（蜂鸣器响 1 秒，不改模式）\n"
        "  {\"action\":\"get_state\"}           查询当前状态\n"
        "\n"
        "当前状态：%1\n"
        "\n"
        "示例：\n"
        "用户：光太强了，把遮阳帘放下来\n"
        "助手：{\"action\":\"relay_on\"}\n"
        "\n"
        "用户：把阈值调到 2500\n"
        "助手：{\"action\":\"set_th\",\"value\":2500}\n"
        "\n"
        "用户：现在光照多少\n"
        "助手：{\"action\":\"get_state\"}"
    ).arg(stateJson);
}

void LlmClient::ask(const QString &userText, const QString &stateJson)
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), m_model);        // 必须来自 GET /v1/models
    body.insert(QStringLiteral("temperature"), 0.1);      // 控制类任务需稳定输出
    body.insert(QStringLiteral("max_tokens"), 512);       // 太小会导致 content 为空
    body.insert(QStringLiteral("stream"), false);

    // 关闭思考（llama.cpp 较新版本支持）；若本机版本不支持该键会报错，
    // 届时删除此键，改为在提示词中要求"直接给最终 JSON"并调大 max_tokens。
    QJsonObject ctkw;
    ctkw.insert(QStringLiteral("enable_thinking"), false);
    body.insert(QStringLiteral("chat_template_kwargs"), ctkw);

    QJsonArray messages;
    QJsonObject sys;
    sys.insert(QStringLiteral("role"), QStringLiteral("system"));
    sys.insert(QStringLiteral("content"), buildSystemPrompt(stateJson));
    messages.append(sys);

    QJsonObject usr;
    usr.insert(QStringLiteral("role"), QStringLiteral("user"));
    usr.insert(QStringLiteral("content"), userText);
    messages.append(usr);

    body.insert(QStringLiteral("messages"), messages);

    QNetworkRequest req{QUrl(m_url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(m_timeoutMs);      // Qt 5.15+ 内置传输超时
    if (!m_token.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    }

    QNetworkReply *rep = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(rep, &QNetworkReply::finished, this, [this, rep]() {
        rep->deleteLater();
        if (rep->error() != QNetworkReply::NoError) {
            emit failed(QStringLiteral("推理服务不可用：%1").arg(rep->errorString()));
            return;
        }
        const QByteArray raw = rep->readAll();
        bool ok = false;
        const QString content = extractContent(raw, &ok);
        if (!ok) {
            emit failed(QStringLiteral("响应格式异常：缺少 choices[0].message.content"));
            return;
        }
        emit finished(content);
    });
}

void LlmClient::fetchModels()
{
    QNetworkRequest req{QUrl(m_modelsUrl)};
    if (!m_token.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    }
    QNetworkReply *rep = m_nam->get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep]() {
        rep->deleteLater();
        if (rep->error() != QNetworkReply::NoError) {
            emit failed(QStringLiteral("获取模型列表失败：%1").arg(rep->errorString()));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
        QStringList models;
        const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
        for (const QJsonValue &v : data) {
            const QString id = v.toObject().value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) {
                models.append(id);
            }
        }
        emit modelsReady(models);
    });
}

QString LlmClient::extractContent(const QByteArray &raw, bool *ok)
{
    *ok = false;
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    const QJsonArray choices = doc.object().value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return QString();
    }
    const QJsonObject msg = choices.first().toObject()
                                .value(QStringLiteral("message")).toObject();

    // 正常路径：content。思考型模型关闭思考失败时 content 可能为空，
    // 此时退回 reasoning_content 尽力解析，交给 CommandEngine 校验兜底。
    QString content = msg.value(QStringLiteral("content")).toString();
    if (content.isEmpty()) {
        content = msg.value(QStringLiteral("reasoning_content")).toString();
    }
    if (content.isEmpty()) {
        return QString();
    }
    *ok = true;
    return content;
}
