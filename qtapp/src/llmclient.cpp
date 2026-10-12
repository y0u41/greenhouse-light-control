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
    //
    // ★ 本次加强（实测驱动，2026-10-12）★
    // 基线实测：同一 prompt 下 glm-4.7-flash 会在两种情况下不守协议词汇 ——
    //   A. 白名单只是罗列、没有"只能从里面选、不得造名字"的硬约束时，
    //      模型会输出 set_threshold / threshold 这类自造名；
    //   B. enable_thinking 未被关掉（content 为空、回落 reasoning_content）时，
    //      512 token 预算被"先分析再输出"的推理吃满，content 截断在分析中途，
    //      MainWindow 取首 { 到末 } 解析失败 —— 表现为"按了没反应"。
    // 故此处：动作白名单**由 Proto:: 常量生成**（避免第二处硬编码），
    // 并显式禁止自造名 + 禁止任何前置叙述 + 补 set_th 的 value 字段名与量程。
    // 注意：temperature 0.1 / max_tokens 512 / enable_thinking=false 保持不变
    //（见 ask()，三者的实测依据写在那边，别在这里改）。

    // 白名单逐条由 Proto 的动作清单生成：Protocol_actions.h 是动作名的唯一事实源，
    // 这里不出现任何手抄的动作名字面量。
    const QStringList names = Proto::allActionNames();
    QStringList whitelist;
    for (int i = 0; i < names.size(); ++i) {
        whitelist.append(QStringLiteral("  %1  %2").arg(names.at(i), Proto::actionLine(i)));
    }
    const QString nameList = names.join(QStringLiteral(" / "));

    return QStringLiteral(
        "你是智能温室感光控制系统的指令解析器。"
        "把你的输出当作机器可执行的指令帧，而不是回答。\n"
        "\n"
        "【输出格式·硬性】\n"
        "只输出一个 JSON 对象，对象里必须有 action 字段。\n"
        "禁止输出任何前置叙述，例如\"分析\"\"步骤如下\"\"首先\"\"1.\"；"
        "这种内容一旦出现，整条指令就作废。\n"
        "禁止 markdown 代码围栏（不要 ```）、禁止注释、禁止解释、禁止说客套话。\n"
        "第一个字符必须是 {，最后一个字符必须是 }。\n"
        "\n"
        "【合法动作白名单·只能从这里选】\n"
        "action 的取值只允许这 %1 个字符串，必须逐字符完全一致：\n"
        "%2\n"
        "%3\n"
        "\n"
        "★ 只能输出上面这些动作名，不得自造新名字 ★\n"
        "严禁翻译、缩写、加后缀或改写动作名。以下写法一律非法：\n"
        "  set_threshold（应为 set_th）、threshold、setThreshold、"
        "open_relay、close_relay、switch_auto、manual、query、status、test_alarm。\n"
        "\n"
        "【口语 → 动作 对应】\n"
        "  打开/吸合/合上继电器、放下遮阳帘 → action 必须是 relay_on\n"
        "  关闭/断开/释放继电器、收起遮阳帘 → action 必须是 relay_off\n"
        "  自动模式/恢复自动          → action 必须是 auto\n"
        "  阈值/门槛/阈值改/阈值设 + 数字 → action 必须是 set_th，并给出 value\n"
        "  状态/怎么样/现在多少/查一下   → action 必须是 get_state\n"
        "  测试报警/蜂鸣器响一下/试一下报警 → action 必须是 alarm_test\n"
        "  切手动（只说切手动、没说吸合还是释放）→ action 必须是 get_state，"
        "因为协议里没有单独的\"切手动\"动作，不要猜。\n"
        "\n"
        "【set_th 的参数】\n"
        "  字段名固定为 value（不是 threshold、不是 num、不是 val），类型为整数。\n"
        "  取值范围 0 < value <= 4095，且必须大于当前回差 hys。\n"
        "  例：把阈值调到 2500 → {\"action\":\"set_th\",\"value\":2500}\n"
        "\n"
        "【继电器语义】\n"
        "光照过强时吸合继电器遮阳/报警，光照恢复正常后释放。\n"
        "relay_on / relay_off 会切到手动模式，只有 auto 能切回自动模式。\n"
        "\n"
        "当前状态：%4\n"
        "\n"
        "【正例·照这样输出】\n"
        "用户：光太强了，把遮阳帘放下来\n"
        "助手：{\"action\":\"relay_on\"}\n"
        "用户：把阈值调到 2500\n"
        "助手：{\"action\":\"set_th\",\"value\":2500}\n"
        "用户：现在光照多少\n"
        "助手：{\"action\":\"get_state\"}\n"
        "用户：测试一下报警\n"
        "助手：{\"action\":\"alarm_test\"}\n"
        "\n"
        "【错误示范·绝不要这样】\n"
        "用户：把阈值调到 2500\n"
        "错误：{\"action\":\"set_threshold\",\"parameter\":2500}  ← 动作名和字段名都错了\n"
        "错误：{\"threshold\":2500}  ← 没有 action 字段\n"
        "错误：我先分析一下用户意图……  ← 任何前置叙述都作废\n"
        "正确：{\"action\":\"set_th\",\"value\":2500}\n"
        "\n"
        "现在直接给出那一个 JSON 对象（第一个字符就是 {）："
    ).arg(QString::number(names.size()), whitelist.join(QLatin1Char('\n')), nameList, stateJson);
}

void LlmClient::ask(const QString &userText, const QString &stateJson)
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), m_model);        // 必须来自 GET /v1/models
    body.insert(QStringLiteral("temperature"), 0.1);      // 控制类任务需稳定输出
    body.insert(QStringLiteral("max_tokens"), 512);       // 太小会导致 content 为空
    body.insert(QStringLiteral("stream"), false);

    // 关闭思考：本机 llama-server 已实测接受该键（非假设）。
    //   实测 2026-10-12，llama-server 0.6.0-dev build 11457 (5ad1c5da0)，
    //   同一 prompt 对照：enable_thinking=false → HTTP 200、无 reasoning_content、
    //   completion_tokens 23、content 直接给出 JSON；不带该键 → HTTP 200、434 token 的
    //   reasoning_content。true/false 均 HTTP 200，故键被接受且实际生效。
    // 注意：该 server 对 chat_template_kwargs 里的未知键不报错（实测塞入伪造键仍 200），
    //   所以"没报错"本身不能证明键生效 —— 上面对 reasoning_content 的差异才是判据。
    //   若将来换用更老的 llama.cpp 版本，请按同样方法重测，不要凭版本号推断。
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

    // 正常路径：content。但 content 可能为空、内容全在 reasoning_content ——
    // 实测 2026-10-12：enable_thinking=true 时，512 token 的 max_tokens 被
    // reasoning 吃满而 content 为空字符串。故此处退回 reasoning_content 尽力解析，
    // 交给 CommandEngine 校验兜底（解析不出合法动作时按"未识别"处理）。
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
