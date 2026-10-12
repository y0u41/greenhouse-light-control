#include "config.h"

Config::Config(QObject *parent)
    : QObject(parent)
    , m_s(QStringLiteral("Greenhouse"), QStringLiteral("LightControl"))
{
}

Config &Config::instance()
{
    static Config s_instance;
    return s_instance;
}

int Config::tcpPort() const
{
    return m_s.value(QStringLiteral("tcp/port"), 9092).toInt();
}

void Config::setTcpPort(int v)
{
    m_s.setValue(QStringLiteral("tcp/port"), v);
}

QString Config::bindAddress() const
{
    return m_s.value(QStringLiteral("tcp/bindAddress"), QStringLiteral("0.0.0.0")).toString();
}

void Config::setBindAddress(const QString &v)
{
    m_s.setValue(QStringLiteral("tcp/bindAddress"), v);
}

QString Config::llmUrl() const
{
    return m_s.value(QStringLiteral("llm/url"),
                     QStringLiteral("http://127.0.0.1:8092/v1/chat/completions")).toString();
}

void Config::setLlmUrl(const QString &v)
{
    m_s.setValue(QStringLiteral("llm/url"), v);
}

QString Config::llmModel() const
{
    return m_s.value(QStringLiteral("llm/model"), QStringLiteral("glm-4.7-flash")).toString();
}

void Config::setLlmModel(const QString &v)
{
    m_s.setValue(QStringLiteral("llm/model"), v);
}

QString Config::llmToken() const
{
    return m_s.value(QStringLiteral("llm/token"), QString()).toString();
}

void Config::setLlmToken(const QString &v)
{
    m_s.setValue(QStringLiteral("llm/token"), v);
}

int Config::llmTimeoutMs() const
{
    return m_s.value(QStringLiteral("llm/timeoutMs"), 15000).toInt();
}

void Config::setLlmTimeoutMs(int v)
{
    m_s.setValue(QStringLiteral("llm/timeoutMs"), v);
}

bool Config::ttsEnabled() const
{
    return m_s.value(QStringLiteral("tts/enabled"), true).toBool();
}

void Config::setTtsEnabled(bool v)
{
    m_s.setValue(QStringLiteral("tts/enabled"), v);
}

QString Config::ttsVoice() const
{
    return m_s.value(QStringLiteral("tts/voice"), QStringLiteral("Microsoft Huihui")).toString();
}

void Config::setTtsVoice(const QString &v)
{
    m_s.setValue(QStringLiteral("tts/voice"), v);
}

QString Config::asrLocale() const
{
    return m_s.value(QStringLiteral("asr/locale"), QStringLiteral("zh-CN")).toString();
}

void Config::setAsrLocale(const QString &v)
{
    m_s.setValue(QStringLiteral("asr/locale"), v);
}

int Config::chartMaxPoints() const
{
    return m_s.value(QStringLiteral("chart/maxPoints"), 300).toInt();
}

int Config::offlineTimeoutMs() const
{
    return m_s.value(QStringLiteral("node/offlineTimeoutMs"), 3000).toInt();
}

void Config::setOfflineTimeoutMs(int v)
{
    m_s.setValue(QStringLiteral("node/offlineTimeoutMs"), v);
}

int Config::ackTimeoutMs() const
{
    return m_s.value(QStringLiteral("node/ackTimeoutMs"), 3000).toInt();
}

void Config::setAckTimeoutMs(int v)
{
    m_s.setValue(QStringLiteral("node/ackTimeoutMs"), v);
}
