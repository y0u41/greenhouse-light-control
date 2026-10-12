#ifndef CSVLOGGER_H
#define CSVLOGGER_H

#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QString>

/* 历史数据落盘：把每个 report 帧 append 成 CSV 的一行。
 *
 * 对应 PRD「数据组织 · 数据存储」："历史数据：Qt 端 CSV 或 SQLite 落盘，
 * 便于绘制长期曲线" —— 本类就是这句声明的实现（此前只有内存里的
 * chart/maxPoints 个点，关掉程序就没了）。方案已拍板：每帧 append CSV，不做 SQLite。
 *
 * 设计约束（都是需求里点名的）：
 *  1) 只写"收到的事实"：入参是**原始帧**的 QJsonObject，不读 NodeModel。
 *     于是 ACK 守卫（command_ack_guard）屏蔽 UI 回写时，落盘照常 ——
 *     "不让旧 report 覆盖界面"与"数据不能丢"是两件事。
 *  2) 打开/写入失败**不得崩溃、不得阻塞、不得每帧刷屏**：
 *     首次失败发一条 logMessage + qWarning，之后静默跳过（本类停用）。
 *  3) 目录不存在自动创建；进度上限见 kFlushRows / kFlushIntervalMs 的取舍说明。
 *
 * 线程安全：本类**不加锁**，因为它只在 GUI（主）线程被使用 ——
 * NodeServer 由 MainWindow 以 parent=this 构造，QTcpSocket 也都在主线程，
 * readyRead → onReadyRead → handleFrame 是同线程直连调用（见 nodeserver.cpp）。
 * 若将来把 NodeServer moveToThread()，必须给本类补 QMutex（见 .cpp 末尾说明）。
 */
class CsvLogger : public QObject
{
    Q_OBJECT
public:
    explicit CsvLogger(QObject *parent = nullptr);
    ~CsvLogger() override;

    /* 打开（不存在则创建，含父目录）。filePath 为空 = 主动关闭落盘。
     * 返回 false 表示未启用落盘；此时程序必须照常运行。 */
    bool open(const QString &filePath);

    /* 追加一行 report 帧。frame 是**收到的原始 JSON**，recvMs 是服务端收到时刻。 */
    void appendReport(const QJsonObject &frame, qint64 recvMs);

    void flush();
    void close();

    bool    isOpen() const { return m_file.isOpen(); }
    QString filePath() const { return m_file.fileName(); }

    /* CSV 表头（与每行字段顺序同源，见 .cpp 的 kReportKeys） */
    static QString headerLine();

signals:
    void logMessage(const QString &msg);

private:
    bool writeRaw(const QByteArray &bytes);
    void failOnce(const QString &why);      // 记录一次并停用，之后静默跳过

    QFile        m_file;
    QElapsedTimer m_sinceFlush;
    int  m_rowsSinceFlush = 0;
    bool m_failed = false;                  // 已失败过：不再重复报错
};

#endif // CSVLOGGER_H
