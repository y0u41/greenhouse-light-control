#include "controlpanel.h"
#include "protocol_actions.h"

#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QIntValidator>

ControlPanel::ControlPanel(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *root = new QVBoxLayout(this);

    {
        auto *g = new QLabel(QStringLiteral("控制面板"));
        g->setStyleSheet(QStringLiteral("font-weight:600;"));
        root->addWidget(g);
    }

    QGridLayout *grid = new QGridLayout;
    m_btnOn    = new QPushButton(QStringLiteral("继电器吸合"));
    m_btnOff   = new QPushButton(QStringLiteral("继电器释放"));
    m_btnAuto  = new QPushButton(QStringLiteral("切回自动"));
    m_btnAlarm = new QPushButton(QStringLiteral("报警测试"));
    m_btnState = new QPushButton(QStringLiteral("查询状态"));
    grid->addWidget(m_btnOn,    0, 0);
    grid->addWidget(m_btnOff,   0, 1);
    grid->addWidget(m_btnAuto,  1, 0);
    grid->addWidget(m_btnAlarm, 1, 1);
    grid->addWidget(m_btnState, 2, 0, 1, 2);
    root->addLayout(grid);

    QHBoxLayout *thRow = new QHBoxLayout;
    thRow->addWidget(new QLabel(QStringLiteral("阈值(0-4095)：")));
    m_thEdit = new QLineEdit;
    m_thEdit->setValidator(new QIntValidator(0, Proto::ADC_MAX, this));
    m_thEdit->setMaximumWidth(90);
    thRow->addWidget(m_thEdit);
    m_btnSetTh = new QPushButton(QStringLiteral("设置"));
    thRow->addWidget(m_btnSetTh);
    thRow->addStretch();
    root->addLayout(thRow);

    m_modeLabel = new QLabel(QStringLiteral("模式：--"));
    m_hintLabel = new QLabel(QStringLiteral("提示：relay_on/off 会切入手动模式"));
    m_hintLabel->setStyleSheet(QStringLiteral("color:#888;"));
    root->addWidget(m_modeLabel);
    root->addWidget(m_hintLabel);
    root->addStretch();

    connect(m_btnOn,    &QPushButton::clicked, this, &ControlPanel::relayOnRequested);
    connect(m_btnOff,   &QPushButton::clicked, this, &ControlPanel::relayOffRequested);
    connect(m_btnAuto,  &QPushButton::clicked, this, &ControlPanel::autoRequested);
    connect(m_btnAlarm, &QPushButton::clicked, this, &ControlPanel::alarmTestRequested);
    connect(m_btnState, &QPushButton::clicked, this, &ControlPanel::getStateRequested);
    connect(m_btnSetTh, &QPushButton::clicked, this, [this]() {
        emit setThresholdRequested(m_thEdit->text().toInt());
    });

    setOnline(false);
}

void ControlPanel::setCurrentNode(int nodeId)
{
    m_nodeId = nodeId;
}

void ControlPanel::setMode(int mode)
{
    m_modeLabel->setText(mode == Proto::MODE_MANUAL
                             ? QStringLiteral("模式：手动")
                             : QStringLiteral("模式：自动"));
}

void ControlPanel::setThreshold(int th, int hys)
{
    if (m_thEdit->text().isEmpty()) {
        m_thEdit->setText(QString::number(th));
    }
    m_hintLabel->setText(QStringLiteral("当前阈值 %1，回差 %2（阈值必须大于回差）")
                             .arg(th).arg(hys));
}

void ControlPanel::setOnline(bool online)
{
    m_btnOn->setEnabled(online);
    m_btnOff->setEnabled(online);
    m_btnAuto->setEnabled(online);
    m_btnAlarm->setEnabled(online);
    m_btnState->setEnabled(online);
    m_btnSetTh->setEnabled(online);
    if (!online) {
        m_hintLabel->setText(QStringLiteral("节点离线，控制按钮已禁用"));
    }
}
