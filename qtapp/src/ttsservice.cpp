#include "ttsservice.h"

#ifdef GREENHOUSE_ENABLE_TTS

TtsService::TtsService(QObject *parent)
    : QObject(parent)
{
    if (QTextToSpeech::availableEngines().isEmpty()) {
        return;                        /* 无 TTS 引擎：available() 为 false，界面置灰 */
    }
    m_tts = new QTextToSpeech(this);
}

bool TtsService::available() const
{
    return (m_tts != nullptr) && m_voiceOk;
}

void TtsService::configure(bool enabled, const QString &preferredVoice)
{
    m_enabled = enabled;
    if (m_tts == nullptr) {
        return;
    }

    const QVector<QVoice> voices = m_tts->availableVoices();
    if (voices.isEmpty()) {
        m_voiceOk = false;             /* 无可用语音：available() 必须同步变 false */
        emit availabilityChanged(false);
        return;
    }

    /* 1) 精确匹配偏好名；2) 任一 zh-CN 语音；3) 第一个可用语音 */
    int pick = -1;
    for (int i = 0; i < voices.size(); ++i) {
        const QString name = voices.at(i).name();
        if (!preferredVoice.isEmpty() && name.contains(preferredVoice, Qt::CaseInsensitive)) {
            pick = i;
            break;
        }
    }
    if (pick < 0) {
        for (int i = 0; i < voices.size(); ++i) {
            const QLocale loc = voices.at(i).locale();
            if (loc.language() == QLocale::Chinese) {
                pick = i;
                break;
            }
        }
    }
    if (pick < 0) {
        pick = 0;
    }

    m_tts->setVoice(voices.at(pick));
    m_voiceOk = true;
    emit availabilityChanged(true);
}

void TtsService::speak(const QString &text)
{
    if (!available() || !m_enabled || text.isEmpty()) {
        return;
    }
    m_tts->say(text);
    emit spoken(text);
}

void TtsService::stop()
{
    if (m_tts != nullptr) {
        m_tts->stop();
    }
}

#else  /* GREENHOUSE_ENABLE_TTS 未定义：编译桩，保证工程可构建 */

TtsService::TtsService(QObject *parent)
    : QObject(parent)
{
}

bool TtsService::available() const
{
    return false;
}

void TtsService::configure(bool, const QString &)
{
    emit availabilityChanged(false);
}

void TtsService::speak(const QString &)
{
}

void TtsService::stop()
{
}

#endif /* GREENHOUSE_ENABLE_TTS */
