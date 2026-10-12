#include "mainwindow.h"
#include "protocol_actions.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDateTime>
#include <QStatusBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_cfg(Config::instance())
    , m_model(new NodeModel(this))
    , m_guard(new CommandAckGuard(this, m_cfg.ackTimeoutMs()))
    , m_csvLogger(new CsvLogger(this))
    , m_server(new NodeServer(m_model, m_guard, m_csvLogger, this))
    , m_engine(new CommandEngine(m_server, m_model, this))
    , m_chart(new ChartWidget(this))
    , m_panel(new ControlPanel(this))
    , m_tts(new TtsService(this))
    , m_asr(new AsrService(this))
    , m_llmCtl(new LlmController(m_cfg, this))
{
    setupUi();
    wireSignals();

    m_server->setOfflineTimeoutMs(m_cfg.offlineTimeoutMs());
    m_chart->setMaxPoints(m_cfg.chartMaxPoints());

    /* 历史数据落盘：在节点开始接入**之前**打开，避免启动瞬间丢帧。
     * 失败（无权限/磁盘满/目录建不出来）只提示一次并降级，不阻断启动。 */
    m_csvLogger->open(m_cfg.historyCsvPath());

    /* 服务器启动失败必须显式报错（端口被占用等），不能静默降级 */
    QString err;
    if (!m_server->start(m_cfg.bindAddress(), (quint16)m_cfg.tcpPort(), &err)) {
        m_lblServerStatus->setText(tr("服务器：启动失败（%1）").arg(err));
        appendChat(tr("系统"), tr("NodeServer 启动失败：%1").arg(err));
    } else {
        m_lblServerStatus->setText(tr("服务器：监听 %1").arg(m_cfg.tcpPort()));
    }

    m_tts->configure(m_cfg.ttsEnabled(), m_cfg.ttsVoice());
    m_ttsBtn->setEnabled(m_tts->available());
    m_speakBtn->setEnabled(m_asr->available());
    if (!m_asr->available()) {
        appendChat(tr("系统"), tr("语音识别不可用（缺少 powershell/麦克风），可用文字输入"));
    }
}

MainWindow::~MainWindow()
{
}

void MainWindow::setupUi()
{
    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *main = new QHBoxLayout(central);

    /* ---------- 左列：节点 / 数据 / 曲线 ---------- */
    auto *left = new QVBoxLayout;

    auto *nodeRow = new QHBoxLayout;
    nodeRow->addWidget(new QLabel(tr("节点：")));
    m_nodeCombo = new QComboBox;
    nodeRow->addWidget(m_nodeCombo);
    nodeRow->addStretch();
    left->addLayout(nodeRow);

    auto *dataBox = new QGroupBox(tr("实时数据"));
    auto *data = new QGridLayout(dataBox);
    m_lblLight = new QLabel(tr("--"));
    m_lblAdc = new QLabel(tr("--"));
    m_lblTh = new QLabel(tr("--"));
    m_lblRelay = new QLabel(tr("--"));
    m_lblMode = new QLabel(tr("--"));
    m_lblOnline = new QLabel(tr("--"));
    data->addWidget(new QLabel(tr("光强 light：")), 0, 0);
    data->addWidget(m_lblLight, 0, 1);
    data->addWidget(new QLabel(tr("原始 adc：")), 0, 2);
    data->addWidget(m_lblAdc, 0, 3);
    data->addWidget(new QLabel(tr("阈值 th：")), 1, 0);
    data->addWidget(m_lblTh, 1, 1);
    data->addWidget(new QLabel(tr("继电器：")), 1, 2);
    data->addWidget(m_lblRelay, 1, 3);
    data->addWidget(new QLabel(tr("模式：")), 2, 0);
    data->addWidget(m_lblMode, 2, 1);
    data->addWidget(new QLabel(tr("状态：")), 2, 2);
    data->addWidget(m_lblOnline, 2, 3);
    left->addWidget(dataBox);

    left->addWidget(m_chart, 1);
    main->addLayout(left, 3);

    /* ---------- 右列：控制 / 语音 / 对话 ---------- */
    auto *right = new QVBoxLayout;
    m_panel->setMaximumWidth(360);
    right->addWidget(m_panel);

    auto *voiceRow = new QHBoxLayout;
    m_speakBtn = new QPushButton(tr("说话"));
    m_ttsBtn = new QPushButton(tr("播报：开"));
    m_ttsBtn->setCheckable(true);
    m_ttsBtn->setChecked(m_cfg.ttsEnabled());
    voiceRow->addWidget(m_speakBtn);
    voiceRow->addWidget(m_ttsBtn);
    voiceRow->addStretch();
    right->addLayout(voiceRow);

    m_chatView = new QTextEdit;
    m_chatView->setReadOnly(true);
    right->addWidget(m_chatView, 1);

    auto *inputRow = new QHBoxLayout;
    m_inputEdit = new QLineEdit;
    m_inputEdit->setPlaceholderText(tr("输入指令，如：光太强了，把遮阳帘放下来"));
    m_sendBtn = new QPushButton(tr("发送"));
    inputRow->addWidget(m_inputEdit);
    inputRow->addWidget(m_sendBtn);
    right->addLayout(inputRow);

    main->addLayout(right, 2);

    /* ---------- 状态栏 ---------- */
    m_lblServerStatus = new QLabel(tr("服务器：--"));
    m_lblLlmStatus = new QLabel(tr("大模型：--"));
    statusBar()->addWidget(m_lblServerStatus);
    statusBar()->addPermanentWidget(m_lblLlmStatus);

    resize(1120, 740);
    setWindowTitle(tr("智能温室感光控制系统"));
}

void MainWindow::wireSignals()
{
    /* 节点数据 */
    connect(m_server, &NodeServer::nodeDataChanged, this, &MainWindow::onNodeDataChanged);
    connect(m_server, &NodeServer::nodeAckReceived, this, &MainWindow::onNodeAckReceived);
    connect(m_server, &NodeServer::commandFailed, this, &MainWindow::onCommandFailed);
    connect(m_server, &NodeServer::nodeOffline, this, &MainWindow::onNodeOffline);
    connect(m_server, &NodeServer::nodeSilent, this, &MainWindow::onNodeSilent);
    connect(m_server, &NodeServer::logMessage, this, &MainWindow::onLogMessage);

    /* 落盘状态（成功时的文件路径 / 失败时的原因）只发一两条，
     * 同时进状态栏与对话区：对话区留痕，便于事后确认数据到底写哪去了。 */
    connect(m_csvLogger, &CsvLogger::logMessage, this, [this](const QString &msg) {
        onLogMessage(msg);
        appendChat(tr("系统"), msg);
    });

    connect(m_nodeCombo, &QComboBox::currentIndexChanged,
            this, &MainWindow::onCurrentNodeChanged);

    /* 控制面板 → 统一指令引擎 */
    connect(m_panel, &ControlPanel::relayOnRequested, this, &MainWindow::onRelayOn);
    connect(m_panel, &ControlPanel::relayOffRequested, this, &MainWindow::onRelayOff);
    connect(m_panel, &ControlPanel::autoRequested, this, &MainWindow::onAuto);
    connect(m_panel, &ControlPanel::setThresholdRequested, this, &MainWindow::onSetThreshold);
    connect(m_panel, &ControlPanel::alarmTestRequested, this, &MainWindow::onAlarmTest);
    connect(m_panel, &ControlPanel::getStateRequested, this, &MainWindow::onGetState);

    /* 文字 / 语音 / 播报 */
    connect(m_sendBtn, &QPushButton::clicked, this, &MainWindow::onSendText);
    connect(m_inputEdit, &QLineEdit::returnPressed, this, &MainWindow::onSendText);
    connect(m_speakBtn, &QPushButton::clicked, this, &MainWindow::onSpeakClicked);
    connect(m_ttsBtn, &QPushButton::toggled, this, &MainWindow::onTtsToggled);

    connect(m_asr, &AsrService::textRecognized, this, &MainWindow::onAsrText);
    connect(m_asr, &AsrService::failed, this, &MainWindow::onAsrFailed);
    connect(m_asr, &AsrService::listeningChanged, this, &MainWindow::onAsrListeningChanged);

    /* 大模型链路：控制器负责线程 / 探测 / 模型列表，UI 只接收结果与可用性 */
    connect(m_llmCtl, &LlmController::llmFinished, this, &MainWindow::onLlmFinished);
    connect(m_llmCtl, &LlmController::llmFailed, this, &MainWindow::onLlmFailed);
    connect(m_llmCtl, &LlmController::modelsChanged, this, &MainWindow::onModelsReady);
    connect(m_llmCtl, &LlmController::llmAvailabilityChanged,
            this, &MainWindow::onLlmAvailabilityChanged);
}

/* ===================== 节点数据 ===================== */

void MainWindow::onNodeDataChanged(int id)
{
    if (const NodeState *st = m_model->peek(id)) {
        m_chart->updateNode(id, st->light, QDateTime::currentDateTime());
    }
    refreshNodeCombo();
    if (id == m_currentNode) {
        refreshDataPanel();
    }
}

void MainWindow::onNodeAckReceived(int id, const QString &action, const QString &result)
{
    /* ★ 必须区分 ok / err ★
     * 固件对"参数非法"（set_th 越界 / <= hys）与"写盘失败"（24C02 不在位）
     * 都会明确回 "err"，并保证**指令未生效**（固件侧已回滚）。
     * 原实现不区分 result，一律 speakState() 播报并当成功 ——
     * 于是"指令被拒绝"在界面上表现为"执行成功"，比不回报还糟。 */
    const bool ok = result.compare(QStringLiteral("err"), Qt::CaseInsensitive) != 0;

    if (ok) {
        appendChat(tr("系统"), tr("节点 %1 回执：%2 → %3").arg(id).arg(action).arg(result));
        speakState(id);
    } else {
        appendChat(tr("系统"), tr("节点 %1 指令 %2 被拒绝（%3），状态未改变")
                       .arg(id).arg(action).arg(result));
        /* 失败时不播报状态：避免"没执行却报成功"误导操作者 */
    }
    refreshDataPanel();
}

void MainWindow::onCommandFailed(int nodeId, const QString &action, const QString &reason)
{
    appendChat(tr("系统"), tr("节点 %1 指令 %2 失败：%3").arg(nodeId).arg(action).arg(reason));
}

void MainWindow::onNodeOffline(int id)
{
    appendChat(tr("系统"), tr("节点 %1 离线").arg(id));
    if (id == m_currentNode) {
        m_panel->setOnline(false);      /* 只影响当前查看的节点，别把别人的按钮一起禁掉 */
    }
    refreshNodeCombo();
    refreshDataPanel();                 /* 状态标签立即反映离线（nodeChanged 未接到 UI） */
}

void MainWindow::onNodeSilent(int id)
{
    appendChat(tr("系统"), tr("节点 %1 链路异常：socket 未断但超时未上报").arg(id));
    refreshNodeCombo();
    if (id == m_currentNode) {
        refreshDataPanel();             /* socket 仍活着，控制按钮不禁用 */
    }
}

void MainWindow::onLogMessage(const QString &msg)
{
    statusBar()->showMessage(msg, 4000);
}

void MainWindow::onCurrentNodeChanged(int index)
{
    const int id = m_nodeCombo->itemData(index).toInt();
    if (id > 0) {
        m_currentNode = id;
    }
    m_panel->setCurrentNode(m_currentNode);
    refreshDataPanel();
}

void MainWindow::refreshNodeCombo()
{
    const QList<int> ids = m_model->nodeIds();
    const int cur = m_currentNode;

    m_nodeCombo->blockSignals(true);
    m_nodeCombo->clear();
    for (int id : ids) {
        QString label = tr("节点 %1").arg(id);
        if (const NodeState *st = m_model->peek(id)) {
            if (!st->online) {
                label += tr("（离线）");
            } else if (st->silent) {
                label += tr("（链路异常）");
            }
        }
        m_nodeCombo->addItem(label, id);
    }
    const int idx = m_nodeCombo->findData(cur);
    if (idx >= 0) {
        m_nodeCombo->setCurrentIndex(idx);
    }
    m_nodeCombo->blockSignals(false);
}

void MainWindow::refreshDataPanel()
{
    const NodeState *st = m_model->peek(m_currentNode);
    if (st == nullptr) {
        m_lblLight->setText(tr("--"));
        m_lblAdc->setText(tr("--"));
        m_lblTh->setText(tr("--"));
        m_lblRelay->setText(tr("--"));
        m_lblMode->setText(tr("--"));
        m_lblOnline->setText(tr("无节点"));
        m_panel->setOnline(false);
        return;
    }

    m_lblLight->setText(QString::number(st->light));
    m_lblAdc->setText(QString::number(st->adc));
    m_lblTh->setText(QStringLiteral("%1（回差 %2）").arg(st->th).arg(st->hys));
    m_lblRelay->setText(st->relay ? tr("吸合") : tr("断开"));
    m_lblMode->setText(st->mode == Proto::MODE_MANUAL ? tr("手动") : tr("自动"));

    if (!st->online) {
        m_lblOnline->setText(tr("离线"));
    } else if (st->silent) {
        m_lblOnline->setText(tr("链路异常"));
    } else {
        m_lblOnline->setText(tr("在线"));
    }

    m_panel->setOnline(st->online);
    m_panel->setMode(st->mode);
    m_panel->setThreshold(st->th, st->hys);
}

/* ===================== 控制指令 ===================== */

int MainWindow::currentHys() const
{
    if (const NodeState *st = m_model->peek(m_currentNode)) {
        if (st->hys > 0) {
            return st->hys;
        }
    }
    return Proto::HYS_DEFAULT;   /* 与固件 HYS_DEFAULT 同源（protocol_actions.h） */
}

void MainWindow::onRelayOn()
{
    QString why;
    if (!m_engine->submit(m_currentNode, QLatin1String(Proto::ACTION_RELAY_ON), -1, &why)) {
        appendChat(tr("系统"), tr("指令被拦截：%1").arg(why));
    }
}

void MainWindow::onRelayOff()
{
    QString why;
    if (!m_engine->submit(m_currentNode, QLatin1String(Proto::ACTION_RELAY_OFF), -1, &why)) {
        appendChat(tr("系统"), tr("指令被拦截：%1").arg(why));
    }
}

void MainWindow::onAuto()
{
    QString why;
    if (!m_engine->submit(m_currentNode, QLatin1String(Proto::ACTION_AUTO), -1, &why)) {
        appendChat(tr("系统"), tr("指令被拦截：%1").arg(why));
    }
}

void MainWindow::onSetThreshold(int value)
{
    QString why;
    if (!m_engine->submit(m_currentNode, QLatin1String(Proto::ACTION_SET_TH), value, &why)) {
        appendChat(tr("系统"), tr("阈值设置被拦截：%1").arg(why));
    }
}

void MainWindow::onAlarmTest()
{
    QString why;
    if (!m_engine->submit(m_currentNode, QLatin1String(Proto::ACTION_ALARM_TEST), -1, &why)) {
        appendChat(tr("系统"), tr("报警测试被拦截：%1").arg(why));
    }
}

void MainWindow::onGetState()
{
    QString why;
    if (!m_engine->submit(m_currentNode, QLatin1String(Proto::ACTION_GET_STATE), -1, &why)) {
        appendChat(tr("系统"), tr("查询被拦截：%1").arg(why));
    }
}

/* ===================== 文字 / 语音 / 大模型 ===================== */

void MainWindow::onSendText()
{
    const QString text = m_inputEdit->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    appendChat(tr("用户"), text);
    m_inputEdit->clear();
    dispatchUserText(text);
}

void MainWindow::dispatchUserText(const QString &text)
{
    if (!m_llmCtl->isAvailable()) {
        /* 大模型不可用：给出明确提示，但监控与控制链路不受影响 */
        appendChat(tr("系统"), tr("大模型当前不可用，请用按钮控制或先启动 llama-server"));
        return;
    }
    m_llmCtl->ask(text, stateJsonFor(m_currentNode));
}

void MainWindow::onLlmAvailabilityChanged(bool ok)
{
    m_lblLlmStatus->setText(ok ? tr("大模型：可用") : tr("大模型：不可用"));
    /* 文字/语音输入只服务于大模型链路，不可用时置灰（PRD：智能交互降级，监控与控制照常） */
    m_inputEdit->setEnabled(ok);
    m_sendBtn->setEnabled(ok);
}

void MainWindow::onLlmFinished(const QString &content)
{
    appendChat(tr("大模型"), content);

    const int b = content.indexOf(QLatin1Char('{'));
    const int e = content.lastIndexOf(QLatin1Char('}'));
    if (b < 0 || e <= b) {
        appendChat(tr("系统"), tr("大模型未返回 JSON 指令，已忽略"));
        return;
    }

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(
        content.mid(b, e - b + 1).toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        appendChat(tr("系统"), tr("大模型返回的 JSON 无法解析，已忽略"));
        return;
    }

    const QJsonObject cmd = doc.object();
    QString why;
    if (!CommandEngine::validate(cmd, currentHys(), &why)) {
        appendChat(tr("系统"), tr("指令被拦截：%1").arg(why));
        return;
    }

    const QString action = cmd.value(QStringLiteral("action")).toString();
    const int value = cmd.contains(QStringLiteral("value"))
                          ? cmd.value(QStringLiteral("value")).toInt() : -1;
    if (!m_engine->submit(m_currentNode, action, value, &why)) {
        appendChat(tr("系统"), tr("指令未下发：%1").arg(why));
    }
}

void MainWindow::onLlmFailed(const QString &reason)
{
    appendChat(tr("系统"), reason);
    /* 具体原因只进对话区；状态栏由 onLlmAvailabilityChanged 统一维护，
     * 避免被周期探测覆盖后语义不一致 */
    m_llmCtl->setUnavailable();
}

void MainWindow::onModelsReady(const QStringList &models)
{
    if (models.isEmpty()) {
        appendChat(tr("系统"), tr("未获取到模型列表，请确认 llama-server 已启动"));
    }
    /* "配置的模型不在服务端列表中 → 切换并重新 configure" 已由 LlmController 处理 */
}

void MainWindow::onSpeakClicked()
{
    if (m_speakBtn->text() == tr("说话")) {
        m_asr->startListening();
    } else {
        m_asr->stopListening();
    }
}

void MainWindow::onAsrText(const QString &text)
{
    appendChat(tr("语音"), text);
    dispatchUserText(text);
}

void MainWindow::onAsrFailed(const QString &reason)
{
    appendChat(tr("系统"), tr("语音识别：%1").arg(reason));
    m_speakBtn->setText(tr("说话"));
}

void MainWindow::onAsrListeningChanged(bool listening)
{
    m_speakBtn->setText(listening ? tr("停止") : tr("说话"));
}

void MainWindow::onTtsToggled(bool on)
{
    m_cfg.setTtsEnabled(on);
    m_ttsBtn->setText(on ? tr("播报：开") : tr("播报：关"));
    m_tts->configure(on, m_cfg.ttsVoice());
}

/* ===================== 其他 ===================== */

void MainWindow::appendChat(const QString &who, const QString &text)
{
    m_chatView->append(QStringLiteral("[%1] %2：%3")
                           .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")))
                           .arg(who, text));
}

QString MainWindow::stateJsonFor(int nodeId) const
{
    const NodeState *st = m_model->peek(nodeId);
    if (st == nullptr) {
        return tr("节点 %1 暂无数据").arg(nodeId);
    }
    return tr("节点%1 光强 %2（0 最暗 / 4095 最强），阈值 %3，回差 %4，继电器 %5，模式 %6")
        .arg(st->nodeId).arg(st->light).arg(st->th).arg(st->hys)
        .arg(st->relay ? tr("吸合") : tr("断开"))
        .arg(st->mode == Proto::MODE_MANUAL ? tr("手动") : tr("自动"));
}

void MainWindow::speakState(int nodeId)
{
    if (!m_cfg.ttsEnabled() || !m_tts->available()) {
        return;
    }
    const NodeState *st = m_model->peek(nodeId);
    if (st == nullptr) {
        return;
    }
    m_tts->speak(tr("节点 %1 当前光强 %2，阈值 %3，继电器 %4，%5 模式")
                     .arg(nodeId).arg(st->light).arg(st->th)
                     .arg(st->relay ? tr("吸合") : tr("断开"))
                     .arg(st->mode == Proto::MODE_MANUAL ? tr("手动") : tr("自动")));
}
