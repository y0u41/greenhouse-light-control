#include "asrservice.h"

#include <QFile>
#include <QTextStream>
#include <QDir>

#ifdef Q_OS_WIN

static const char *kPsScript =
    "try { Add-Type -AssemblyName System.Speech } catch { exit 2 }\n"
    "$rec = $null\n"
    "try { $rec = New-Object System.Speech.Recognition.SpeechRecognitionEngine"
    " (New-Object System.Globalization.CultureInfo('%LOCALE%')) } catch { $rec = $null }\n"
    "if ($rec -eq $null) { $rec = New-Object System.Speech.Recognition.SpeechRecognitionEngine }\n"
    "$rec.LoadGrammar((New-Object System.Speech.Grammar.DictationGrammar))\n"
    "try { $rec.SetInputToDefaultAudioDevice() } catch { exit 3 }\n"
    "while ($true) {\n"
    "  $r = $rec.Recognize([TimeSpan]::FromSeconds(20))\n"
    "  if ($r -ne $null) { Write-Output $r.Text }\n"
    "}\n";

AsrService::AsrService(QObject *parent)
    : QObject(parent)
{
}

AsrService::~AsrService()
{
    stopListening();
}

bool AsrService::available() const
{
    return QFile::exists(QStringLiteral("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"));
}

QString AsrService::lastError() const
{
    return m_error;
}

void AsrService::launch()
{
    if (m_proc != nullptr) {
        return;
    }

    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    m_proc->setReadChannel(QProcess::StandardOutput);

    const QString script = QString::fromLatin1(kPsScript)
                               .replace(QStringLiteral("%LOCALE%"), m_locale);

    connect(m_proc, &QProcess::readyReadStandardOutput, this, [this]() {
        QTextStream in(m_proc->readAllStandardOutput());
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (!line.isEmpty()) {
                emit textRecognized(line);
            }
        }
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        m_error = QStringLiteral("识别进程错误：%1").arg((int)e);
        emit failed(m_error);
        stopListening();
    });
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        if (st == QProcess::NormalExit && code == 2) {
            m_error = QStringLiteral("系统缺少 System.Speech（.NET 语音组件）");
            emit failed(m_error);
        } else if (st == QProcess::NormalExit && code == 3) {
            m_error = QStringLiteral("未找到可用音频输入设备（麦克风）");
            emit failed(m_error);
        }
        stopListening();
    });

    m_proc->start(QStringLiteral("powershell"),
                  {QStringLiteral("-NoProfile"),
                   QStringLiteral("-NonInteractive"),
                   QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                   QStringLiteral("-Command"), script});

    if (!m_proc->waitForStarted(3000)) {
        m_error = QStringLiteral("无法启动识别进程：%1").arg(m_proc->errorString());
        emit failed(m_error);
        stopListening();
        return;
    }

    m_listening = true;
    emit listeningChanged(true);
}

void AsrService::startListening()
{
    if (m_listening) {
        return;
    }
    if (!available()) {
        m_error = QStringLiteral("未找到 powershell.exe");
        emit failed(m_error);
        return;
    }
    launch();
}

void AsrService::stopListening()
{
    if (m_proc != nullptr) {
        m_proc->disconnect(this);
        if (m_proc->state() != QProcess::NotRunning) {
            m_proc->kill();
            m_proc->waitForFinished(1000);
        }
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    if (m_listening) {
        m_listening = false;
        emit listeningChanged(false);
    }
}

#else  /* 非 Windows：编译桩 */

AsrService::AsrService(QObject *parent)
    : QObject(parent)
{
}

AsrService::~AsrService() = default;

bool AsrService::available() const
{
    return false;
}

QString AsrService::lastError() const
{
    return m_error;
}

void AsrService::startListening()
{
    emit failed(QStringLiteral("当前平台不支持语音识别"));
}

void AsrService::stopListening()
{
}

#endif /* Q_OS_WIN */
