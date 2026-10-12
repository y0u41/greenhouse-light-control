#include "voicecontroller.h"

VoiceController::VoiceController(Config &cfg, QObject *parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_tts(new TtsService(this))
    , m_asr(new AsrService(this))
{
    /* TtsService::configure() 每次都会发 availabilityChanged —— 链接到此，
     * UI 据此置灰"播报"按钮（与原先 m_ttsBtn->setEnabled(m_tts->available()) 等价） */
    connect(m_tts, &TtsService::availabilityChanged,
            this, &VoiceController::ttsAvailabilityChanged);
    connect(m_asr, &AsrService::textRecognized, this, &VoiceController::asrText);
    connect(m_asr, &AsrService::failed, this, &VoiceController::asrFailed);
    connect(m_asr, &AsrService::listeningChanged, this, [this](bool listening) {
        /* m_listening 是"说话/停止"按钮文案与 toggleListening() 判据的唯一来源 */
        m_listening = listening;
        emit asrListeningChanged(listening);
    });
}

bool VoiceController::ttsAvailable() const
{
    return m_tts->available();
}

bool VoiceController::asrAvailable() const
{
    return m_asr->available();
}

void VoiceController::configureTts()
{
    m_tts->configure(m_cfg.ttsEnabled(), m_cfg.ttsVoice());
}

void VoiceController::configureAsr()
{
    if (!m_asr->available()) {
        emit asrUnavailable();
    }
}

void VoiceController::onTtsToggled(bool on)
{
    m_cfg.setTtsEnabled(on);
    m_tts->configure(on, m_cfg.ttsVoice());
}

void VoiceController::toggleListening()
{
    if (m_listening) {
        m_asr->stopListening();
    } else {
        m_asr->startListening();
    }
}

void VoiceController::speakState(const QString &text)
{
    if (!m_cfg.ttsEnabled() || !m_tts->available()) {
        return;
    }
    m_tts->speak(text);
}
