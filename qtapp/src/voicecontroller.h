#ifndef VOICECONTROLLER_H
#define VOICECONTROLLER_H

#include <QObject>
#include <QString>

#include "config.h"
#include "ttsservice.h"
#include "asrservice.h"

// 语音链路控制器：把 MainWindow 里"TTS 播报 + ASR 识别 + 播报开关"这一整块职责
// 搬出来。MVVM 意义上的 ViewModel —— 持有两个服务对象与 Config，只暴露信号/槽，
// 不反向依赖 MainWindow 的任何具体控件。
//
// 条件编译：GREENHOUSE_ENABLE_TTS / GREENHOUSE_ENABLE_ASR 的语义完全沿用原实现 ——
// 本类不做任何 #if 分支，TTS 未安装时行为由 TtsService 的编译桩决定
// （available() 恒 false → 按钮置灰 + 启动提示，其余功能不受影响）。
class VoiceController : public QObject
{
    Q_OBJECT
public:
    explicit VoiceController(Config &cfg, QObject *parent = nullptr);

    /* 服务可用性：MainWindow 用它决定"说话"按钮是否可点、启动时是否提示降级 */
    bool ttsAvailable() const;
    bool asrAvailable() const;

public slots:
    /* UI 装配完成后调用：配置 TTS（并按结果广播 ttsAvailabilityChanged） */
    void configureTts();
    /* ASR 探活：不可用时回报 asrUnavailable（由 UI 决定怎么提示） */
    void configureAsr();

    /* 播报开关（对应"播报：开/关"checkable 按钮） */
    void onTtsToggled(bool on);

    /* "说话/停止"按钮：按当前监听状态取反 */
    void toggleListening();

    /* 状态播报：文本由 UI 侧拼装（需要 NodeModel），本类只负责说 */
    void speakState(const QString &text);

signals:
    /* 识别结果 / 失败原因 / 监听状态变化（原样透传给对话区与按钮文案） */
    void asrText(const QString &text);
    void asrFailed(const QString &reason);
    void asrListeningChanged(bool listening);

    /* 可用性（构建期无 TTS 引擎/无 powershell 时回报 false）：
     *  - ttsAvailabilityChanged 由 TtsService 的 configure/语音枚举结果驱动（运行期可能变化）；
     *  - asrUnavailable 在启动探活失败时发一次，UI 决定怎么提示降级；
     *  - 启动期的同步读取走 ttsAvailable()/asrAvailable()。 */
    void ttsAvailabilityChanged(bool ok);
    void asrUnavailable();

private:
    Config &m_cfg;
    TtsService *m_tts;
    AsrService *m_asr;
    bool m_listening = false;
};

#endif // VOICECONTROLLER_H
