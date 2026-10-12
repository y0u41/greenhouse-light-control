#ifndef CONTROLPANEL_H
#define CONTROLPANEL_H

#include <QWidget>

class QPushButton;
class QLineEdit;
class QLabel;

// 控制面板：只负责"收集用户意图"并发出信号，
// 真正的校验与统一下发由 CommandEngine 完成（UI 不自行判断合法性）。
class ControlPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ControlPanel(QWidget *parent = nullptr);

    void setCurrentNode(int nodeId);
    void setMode(int mode);
    void setThreshold(int th, int hys);
    void setOnline(bool online);

signals:
    void relayOnRequested();
    void relayOffRequested();
    void autoRequested();
    void setThresholdRequested(int value);
    void alarmTestRequested();
    void getStateRequested();

private:
    QPushButton *m_btnOn;
    QPushButton *m_btnOff;
    QPushButton *m_btnAuto;
    QPushButton *m_btnAlarm;
    QPushButton *m_btnState;
    QPushButton *m_btnSetTh;
    QLineEdit   *m_thEdit;
    QLabel      *m_modeLabel;
    QLabel      *m_hintLabel;
    int m_nodeId = -1;
};

#endif // CONTROLPANEL_H
