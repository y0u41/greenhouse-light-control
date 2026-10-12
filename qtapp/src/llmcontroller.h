#ifndef LLMCONTROLLER_H
#define LLMCONTROLLER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include "config.h"
#include "llmclient.h"

// 大模型链路控制器：把 MainWindow 里"LLM 工作线程 + 模型列表 + /health 周期可用性探测"
// 这一整块职责搬出来，UI 侧只负责装配与转发。
//
// 线程纪律（与 LlmClient 头文件一致）：
//  - LlmClient（含其 QNetworkAccessManager）必须在工作线程内构造 —— 见 startThread()；
//  - 本对象自身（QTimer m_probe / QNetworkAccessManager m_probeNam）与 MainWindow 同在 GUI 线程。
//
// 可用性判据：只认 /health 探测结果（信号 llmAvailabilityChanged）。
// ★ 不要用"输入框/播报按钮是否可点"代替 —— TTS 未安装时按钮禁用，
//   会把大模型指令链路一起拦死（本机 TextToSpeech 未安装，必现）。
class LlmController : public QObject
{
    Q_OBJECT
public:
    explicit LlmController(Config &cfg, QObject *parent = nullptr);
    ~LlmController() override;

    /* 大模型是否可用：MainWindow 的输入分发据此决定"下发 or 提示降级" */
    bool isAvailable() const { return m_available; }

public slots:
    /* 由 MainWindow 转发用户文字 / 语音识别文本 */
    void ask(const QString &userText, const QString &stateJson);
    void fetchModels();
    /* 推理失败时立刻降级为"不可用"（不必等下一次 /health 探测） */
    void setUnavailable();

signals:
    /* 发给工作线程里的 LlmClient（跨线程自动排队）。
     * 必须与下面的槽区分开名字：同名信号/槽在 connect(&ctl, &LlmController::ask, ...)
     * 这种 PMF 写法下会被解析成"连到自己"，造成自递归。 */
    void requestAsk(const QString &userText, const QString &stateJson);
    void requestFetchModels();

    /* 推理结果与失败原因，原样透传给 MainWindow（对话区/状态栏由 UI 决定） */
    void llmFinished(const QString &content);
    void llmFailed(const QString &reason);

    /* 周期探测改变了大模型可用性（含首次探测） */
    void llmAvailabilityChanged(bool ok);

    /* 服务端模型列表变化（含首次获取）。MainWindow 只消费"列表为空"这一条提示；
     * "配置模型不在列表则切换"的修正已在本控制器内完成。 */
    void modelsChanged(const QStringList &models);

private:
    void startThread();
    void probe();
    void setAvailable(bool ok);
    void onModelsReady(const QStringList &models);

    Config &m_cfg;

    QThread *m_thread = nullptr;
    LlmClient *m_llm = nullptr;           /* 工作线程内构造，m_thread 完成后 deleteLater */

    QNetworkAccessManager *m_probeNam = nullptr;
    QTimer m_probe;

    QStringList m_models;
    bool m_available = false;
};

#endif // LLMCONTROLLER_H
