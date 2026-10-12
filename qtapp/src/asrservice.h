#ifndef ASRSERVICE_H
#define ASRSERVICE_H

#include <QObject>
#include <QProcess>
#include <QString>

// 语音识别（Windows）。
//
// 实现选择说明：MinGW 下直接链 SAPI/WinRT COM 头文件与导入库的可移植性差，
// 容易让整个工程编译失败。这里改用"PowerShell + System.Speech 子进程"方案：
//   - 不引入任何 COM 头/库，任何 MinGW 套件都能编译；
//   - 识别结果以 stdout 行输出，主线程读行即可；
//   - powershell 不存在或引擎缺失时发射 failed，界面把"说话"按钮置灰，
//     文字输入链路照常（满足 Tech_Design §语音链路降级）。
class AsrService : public QObject
{
    Q_OBJECT
public:
    explicit AsrService(QObject *parent = nullptr);
    ~AsrService() override;

    bool available() const;
    QString lastError() const;

public slots:
    void startListening();
    void stopListening();

signals:
    void textRecognized(const QString &text);
    void listeningChanged(bool listening);
    void failed(const QString &reason);

private:
    void launch();

    QProcess *m_proc = nullptr;
    QString m_locale = QStringLiteral("zh-CN");
    bool m_listening = false;
    QString m_error;
};

#endif // ASRSERVICE_H
