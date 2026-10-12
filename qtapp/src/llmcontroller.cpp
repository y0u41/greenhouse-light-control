#include "llmcontroller.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

LlmController::LlmController(Config &cfg, QObject *parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_probeNam(new QNetworkAccessManager(this))
{
    startThread();

    m_probe.setInterval(5000);
    connect(&m_probe, &QTimer::timeout, this, &LlmController::probe);
    m_probe.start();
    probe();
}

LlmController::~LlmController()
{
    if (m_thread != nullptr) {
        m_thread->quit();
        if (!m_thread->wait(3000)) {
            m_thread->terminate();
            m_thread->wait(1000);
        }
    }
}

void LlmController::startThread()
{
    m_thread = new QThread(this);
    connect(m_thread, &QThread::started, this, [this]() {
        /* 在工作线程内构造：QNetworkAccessManager 与 LlmClient 同生共死 */
        m_llm = new LlmClient();
        m_llm->configure(m_cfg.llmUrl(), m_cfg.llmModel(),
                         m_cfg.llmToken(), m_cfg.llmTimeoutMs());
        connect(this, &LlmController::requestAsk, m_llm, &LlmClient::ask);
        connect(this, &LlmController::requestFetchModels, m_llm, &LlmClient::fetchModels);
        connect(m_llm, &LlmClient::finished, this, &LlmController::llmFinished);
        connect(m_llm, &LlmClient::failed, this, &LlmController::llmFailed);
        connect(m_llm, &LlmClient::modelsReady, this, &LlmController::onModelsReady);
        connect(m_thread, &QThread::finished, m_llm, &QObject::deleteLater);
        emit fetchModels();
    });
    m_thread->start();
}

void LlmController::ask(const QString &userText, const QString &stateJson)
{
    /* 只做转发：正文与拼装都在 LlmClient::ask 里，这里不改写任何参数 */
    emit requestAsk(userText, stateJson);
}

void LlmController::fetchModels()
{
    emit requestFetchModels();
}

void LlmController::setUnavailable()
{
    setAvailable(false);
}

void LlmController::probe()
{
    QUrl u(m_cfg.llmUrl());
    u.setPath(QStringLiteral("/health"));

    QNetworkRequest req(u);
    req.setTransferTimeout(4000);
    QNetworkReply *rep = m_probeNam->get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep]() {
        rep->deleteLater();
        setAvailable(rep->error() == QNetworkReply::NoError);
    });
}

void LlmController::setAvailable(bool ok)
{
    if (m_available == ok) {
        return;
    }
    m_available = ok;
    emit llmAvailabilityChanged(ok);
}

void LlmController::onModelsReady(const QStringList &models)
{
    m_models = models;
    const QString configured = m_cfg.llmModel();
    if (!models.isEmpty() && !models.contains(configured)) {
        m_cfg.setLlmModel(models.first());
        if (m_llm != nullptr) {
            QMetaObject::invokeMethod(m_llm, [this]() {
                m_llm->configure(m_cfg.llmUrl(), m_cfg.llmModel(),
                                 m_cfg.llmToken(), m_cfg.llmTimeoutMs());
            }, Qt::QueuedConnection);
        }
        /* 对话区的提示文案留在 MainWindow（UI 的事）；这里只报"从谁换到谁" */
        emit modelSwitched(configured, models.first());
    }
    emit modelsChanged(models);
}
