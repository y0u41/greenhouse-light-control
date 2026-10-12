#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QNetworkAccessManager>
#include <QComboBox>
#include <QLabel>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>

#include "config.h"
#include "nodemodel.h"
#include "nodeserver.h"
#include "commandengine.h"
#include "command_ack_guard.h"
#include "csvlogger.h"
#include "llmclient.h"
#include "chartwidget.h"
#include "controlpanel.h"
#include "ttsservice.h"
#include "asrservice.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    /* 发给工作线程里的 LlmClient（跨线程自动排队） */
    void requestLlm(const QString &userText, const QString &stateJson);
    void requestModels();

private slots:
    void onNodeDataChanged(int id);
    void onNodeAckReceived(int id, const QString &action, const QString &result);
    void onCommandFailed(int nodeId, const QString &action, const QString &reason);
    void onNodeOffline(int id);
    void onNodeSilent(int id);
    void onLogMessage(const QString &msg);

    void onCurrentNodeChanged(int index);

    void onRelayOn();
    void onRelayOff();
    void onAuto();
    void onSetThreshold(int value);
    void onAlarmTest();
    void onGetState();

    void onSendText();
    void onSpeakClicked();
    void onTtsToggled(bool on);

    void onLlmFinished(const QString &content);
    void onLlmFailed(const QString &reason);
    void onModelsReady(const QStringList &models);
    void onAsrText(const QString &text);
    void onAsrFailed(const QString &reason);
    void onAsrListeningChanged(bool listening);

    void probeLlm();
    void refreshNodeCombo();
    void refreshDataPanel();

private:
    void setupUi();
    void wireSignals();
    void startLlmThread();
    void appendChat(const QString &who, const QString &text);
    QString stateJsonFor(int nodeId) const;
    void dispatchUserText(const QString &text);
    void speakState(int nodeId);
    int  currentHys() const;
    void setLlmAvailable(bool ok);

    Config &m_cfg;
    NodeModel *m_model;
    CommandAckGuard *m_guard;
    CsvLogger *m_csvLogger;
    NodeServer *m_server;
    CommandEngine *m_engine;
    ChartWidget *m_chart;
    ControlPanel *m_panel;
    TtsService *m_tts;
    AsrService *m_asr;

    QThread *m_llmThread = nullptr;
    LlmClient *m_llm = nullptr;

    QComboBox *m_nodeCombo = nullptr;
    QLabel *m_lblLight = nullptr;
    QLabel *m_lblAdc = nullptr;
    QLabel *m_lblTh = nullptr;
    QLabel *m_lblRelay = nullptr;
    QLabel *m_lblMode = nullptr;
    QLabel *m_lblOnline = nullptr;
    QLabel *m_lblServerStatus = nullptr;
    QLabel *m_lblLlmStatus = nullptr;
    QTextEdit *m_chatView = nullptr;
    QLineEdit *m_inputEdit = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_speakBtn = nullptr;
    QPushButton *m_ttsBtn = nullptr;
    QNetworkAccessManager *m_probeNam = nullptr;
    QTimer m_llmProbe;

    int m_currentNode = 1;
    QStringList m_models;
    /* 大模型可用性：智能交互的唯一判据。
     * 注意不要用 m_ttsBtn->isEnabled() 代替 —— TTS 未装时按钮禁用，
     * 会把大模型指令链路一起拦死（本机 TextToSpeech 未安装，必现）。 */
    bool m_llmAvailable = false;
};

#endif // MAINWINDOW_H
