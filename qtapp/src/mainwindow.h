#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QComboBox>
#include <QLabel>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>

#include "config.h"
#include "nodemodel.h"
#include "nodeserver.h"
#include "commandengine.h"
#include "command_ack_guard.h"
#include "csvlogger.h"
#include "llmcontroller.h"
#include "chartwidget.h"
#include "controlpanel.h"
#include "voicecontroller.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

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
    void onTtsToggled(bool on);

    void onLlmFinished(const QString &content);
    void onLlmFailed(const QString &reason);
    void onModelsReady(const QStringList &models);
    void onModelSwitched(const QString &from, const QString &to);
    void onLlmAvailabilityChanged(bool ok);
    void onAsrText(const QString &text);
    void onAsrFailed(const QString &reason);
    void onAsrListeningChanged(bool listening);

    void refreshNodeCombo();
    void refreshDataPanel();

private:
    void setupUi();
    void wireSignals();
    void appendChat(const QString &who, const QString &text);
    QString stateJsonFor(int nodeId) const;
    void dispatchUserText(const QString &text);
    void speakState(int nodeId);
    int  currentHys() const;

    Config &m_cfg;
    NodeModel *m_model;
    CommandAckGuard *m_guard;
    CsvLogger *m_csvLogger;
    NodeServer *m_server;
    CommandEngine *m_engine;
    ChartWidget *m_chart;
    ControlPanel *m_panel;
    LlmController *m_llmCtl;
    VoiceController *m_voice;

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

    int m_currentNode = 1;
    /* 大模型可用性：智能交互的唯一判据。
     * 注意不要用 m_ttsBtn->isEnabled() 代替 —— TTS 未装时按钮禁用，
     * 会把大模型指令链路一起拦死（本机 TextToSpeech 未安装，必现）。
     * 状态本身由 LlmController 持有（m_llmCtl->isAvailable()），此处不再另存一份。 */
};

#endif // MAINWINDOW_H
