#ifndef TTSSERVICE_H
#define TTSSERVICE_H

#include <QObject>
#include <QString>

#ifdef GREENHOUSE_ENABLE_TTS
#include <QTextToSpeech>
#endif

// 语音播报：Qt TextToSpeech 封装。
// 纪律（AGENTS 已知陷阱）：不硬编码语音名（本机无 Microsoft Xiaoxiao），
// 运行时枚举 availableVoices() 按 zh-CN 过滤 + 回退；无可用中文语音则置灰。
class TtsService : public QObject
{
    Q_OBJECT
public:
    explicit TtsService(QObject *parent = nullptr);

    bool available() const;
    void configure(bool enabled, const QString &preferredVoice);

public slots:
    void speak(const QString &text);
    void stop();

signals:
    void availabilityChanged(bool ok);
    void spoken(const QString &text);

private:
#ifdef GREENHOUSE_ENABLE_TTS
    QTextToSpeech *m_tts = nullptr;
#endif
    bool m_enabled = true;
    /* 是否已选定可用语音：configure() 里 voices 为空时必须置 false，
     * 否则 available() 仍返回 true，与 availabilityChanged(false) 自相矛盾。 */
    bool m_voiceOk = true;
};

#endif // TTSSERVICE_H
