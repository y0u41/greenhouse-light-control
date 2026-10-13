#include "csvlogger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>

namespace {

/* report 帧的字段顺序（PRD 协议章节：adc / light / th / hys / relay / buzz / mode）。
 * 表头与每行都从这张表生成 —— 只有一处事实源，杜绝"改了表头忘了改行"的错位。
 * node 与 timestamp 是行首两列，单独处理。 */
const char *const kReportKeys[] = {"adc", "light", "th", "hys", "relay", "buzz", "mode"};
constexpr int kReportKeyCount = int(sizeof(kReportKeys) / sizeof(kReportKeys[0]));

/* flush 取舍（1 Hz/节点上报）：
 *  - 写：QFile::write 直接走底层 write()，**进程内没有用户态缓冲**，所以
 *    TerminateProcess / 任务管理器结束进程不会丢已写出的行；flush() 只是把
 *    数据从 OS 页缓存压到设备（防断电/蓝屏）。
 *  - 因此策略定为"每 8 行 或 每 1 s 压一次 + 关闭时必压"：
 *    最坏情况（突然断电）丢 ≤1 s 的数据，代价是 1 Hz × 节点数 次 flush/s，
 *    对每帧 1 行（约 70 B）的写入量而言可忽略；既不"每帧刷"也不"一直不刷"。 */
constexpr int    kFlushRows       = 8;
constexpr qint64 kFlushIntervalMs = 1000;

/* 行尾用 CRLF：Windows 上双击直接进 Excel/WPS；不依赖 QIODevice::Text，
 * 让字节内容与平台无关（测试里可逐字节断言）。 */
const char *const kEol = "\r\n";

QString csvField(const QString &v)
{
    const bool needQuote = v.contains(QLatin1Char(',')) || v.contains(QLatin1Char('"'))
                           || v.contains(QLatin1Char('\n')) || v.contains(QLatin1Char('\r'));
    if (!needQuote) {
        return v;
    }
    QString out = v;
    out.replace(QLatin1Char('"'), QLatin1String("\"\""));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

} // namespace

CsvLogger::CsvLogger(QObject *parent)
    : QObject(parent)
{
}

CsvLogger::~CsvLogger()
{
    close();                 // 退出前必须落盘（QFile::flush + close）
}

QString CsvLogger::headerLine()
{
    QString h = QStringLiteral("timestamp,node");
    for (int i = 0; i < kReportKeyCount; ++i) {
        h += QLatin1Char(',');
        h += QLatin1String(kReportKeys[i]);
    }
    return h;
}

bool CsvLogger::open(const QString &filePath)
{
    close();
    m_failed = false;

    if (filePath.trimmed().isEmpty()) {
        emit logMessage(QStringLiteral("历史数据落盘：已关闭（storage/historyCsv 为空）"));
        return false;
    }

    /* 相对路径按 exe 所在目录解析：Windows 下双击启动时 CWD 不可预期，
     * 若按 CWD 解析会出现"文件写到别处去了"。 */
    QString path = filePath;
    if (QFileInfo(path).isRelative()) {
        path = QDir(QCoreApplication::applicationDirPath()).filePath(path);
    }

    const QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        m_file.setFileName(path);
        failOnce(QStringLiteral("无法创建目录 %1").arg(QDir::toNativeSeparators(dir.absolutePath())));
        return false;
    }

    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        failOnce(QStringLiteral("无法打开文件：%1").arg(m_file.errorString()));
        return false;
    }

    m_rowsSinceFlush = 0;
    m_sinceFlush.start();

    /* 首次创建（文件为空/刚建）写表头，且立刻落盘：
     * 这样"跑起来了但一行都没有"时文件里也看得见表头与列含义。 */
    if (m_file.size() == 0) {
        const QByteArray head = (headerLine() + QLatin1String(kEol)).toUtf8();
        if (!writeRaw(head)) {
            return false;
        }
        flush();
    }

    emit logMessage(QStringLiteral("历史数据落盘：%1")
                        .arg(QDir::toNativeSeparators(m_file.fileName())));
    return true;
}

void CsvLogger::appendReport(const QJsonObject &frame, qint64 recvMs)
{
    if (!m_file.isOpen()) {
        return;              // 未启用 / 已失败：静默跳过，绝不每帧刷屏
    }

    /* ISO8601（毫秒 + **显式时区偏移**）：人可读、可排序，且与 report 周期（1 s）
     * 相比毫秒分辨率足够区分同秒到达的多节点帧。
     * 偏移必须显式带上：Qt 只在 spec 为 OffsetFromUTC/UTC 时才输出偏移后缀，
     * 直接对 LocalTime 调 toString(ISODateWithMs) 会得到不带时区的裸时间
     * （换台机器或改了时区，历史文件就解释不了了）。 */
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(recvMs);
    QString line = csvField(dt.toOffsetFromUtc(dt.offsetFromUtc()).toString(Qt::ISODateWithMs));
    line += QLatin1Char(',');
    line += QString::number(frame.value(QStringLiteral("node")).toInt(-1));

    for (int i = 0; i < kReportKeyCount; ++i) {
        const QString key = QLatin1String(kReportKeys[i]);
        line += QLatin1Char(',');
        /* 缺字段或非数值字段写空列，**不**用 0 冒充：0 是合法取值
         * （relay=0 断开、mode=0 自动），用 0 填会把"没收到/格式错"
         * 伪造成"收到了一个 0"。QJsonValue::toInt() 对字符串/布尔返回默认值 0，
         * 必须先 isDouble() 判定再取值。 */
        const QJsonValue v = frame.value(key);
        if (v.isDouble()) {
            line += csvField(QString::number(v.toInt()));
        }
    }
    line += QLatin1String(kEol);

    writeRaw(line.toUtf8());
}

void CsvLogger::flush()
{
    if (!m_file.isOpen()) {
        return;
    }
    if (!m_file.flush()) {
        failOnce(QStringLiteral("flush 失败：%1").arg(m_file.errorString()));
        return;
    }
    m_rowsSinceFlush = 0;
    m_sinceFlush.restart();
}

void CsvLogger::close()
{
    if (m_file.isOpen()) {
        flush();
        m_file.close();
    }
}

bool CsvLogger::writeRaw(const QByteArray &bytes)
{
    if (m_file.write(bytes) != bytes.size()) {
        failOnce(QStringLiteral("写入失败：%1").arg(m_file.errorString()));
        return false;
    }
    m_rowsSinceFlush += 1;
    if (m_rowsSinceFlush >= kFlushRows
        || (m_sinceFlush.isValid() && m_sinceFlush.elapsed() >= kFlushIntervalMs)) {
        flush();
    }
    return true;
}

void CsvLogger::failOnce(const QString &why)
{
    const QString path = m_file.fileName().isEmpty()
                             ? QStringLiteral("<未指定>")
                             : QDir::toNativeSeparators(m_file.fileName());
    if (m_file.isOpen()) {
        m_file.close();
    }
    if (m_failed) {
        return;              // 已经报过一次：后续静默
    }
    m_failed = true;
    const QString msg = QStringLiteral("历史数据落盘已停用（%1）：%2；功能降级，程序照常运行")
                            .arg(path, why);
    qWarning().noquote() << msg;      // 控制台/调试输出留痕
    emit logMessage(msg);            // UI 只提示这一次
}

/* 线程安全：本类**故意不加锁**。
 * 证据（不是假设）：MainWindow 以 parent=this 构造 NodeServer（GUI 线程），
 * QTcpSocket 由 QTcpServer::nextPendingConnection() 在监听线程（=GUI 线程）产生，
 * readyRead → onReadyRead → handleFrame 是同一线程的直接调用，全工程没有对
 * NodeServer 调用过 moveToThread()。因此 appendReport / flush / close 全部发生在
 * 主线程，不存在并发访问。若将来把 NodeServer 移到工作线程，必须在 open/appendReport/
 * flush/close 四处加同一把 QMutex（并在 open/close 前后保持顺序一致）。 */
