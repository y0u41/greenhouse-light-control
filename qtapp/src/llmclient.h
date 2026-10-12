#ifndef LLMCLIENT_H
#define LLMCLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>

// llama.cpp OpenAI 兼容 API 客户端。
//
// 线程纪律（Tech_Design §线程模型）：
//  - 本对象（含内部 QNetworkAccessManager）必须创建、使用、销毁在**同一个工作线程**；
//  - 不要在别的线程 new 完再 moveToThread；正确做法是把构造放进工作线程的
//    started 槽里（MainWindow 已按此实现）；
//  - HTTP 全异步 + setTransferTimeout 超时，不阻塞、不手写事件循环等待。
class LlmClient : public QObject
{
    Q_OBJECT
public:
    explicit LlmClient(QObject *parent = nullptr);

    void configure(const QString &url, const QString &model,
                   const QString &token, int timeoutMs);

public slots:
    void ask(const QString &userText, const QString &stateJson);
    void fetchModels();

signals:
    void finished(const QString &content);        // choices[0].message.content
    void failed(const QString &reason);
    void modelsReady(const QStringList &models);

private:
    QNetworkAccessManager *m_nam = nullptr;
    QString m_url;
    QString m_modelsUrl;
    QString m_model;
    QString m_token;
    int m_timeoutMs = 15000;

    QString buildSystemPrompt(const QString &stateJson) const;
    static QString extractContent(const QByteArray &raw, bool *ok);
};

#endif // LLMCLIENT_H
