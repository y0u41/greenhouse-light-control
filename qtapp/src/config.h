#ifndef CONFIG_H
#define CONFIG_H

#include <QObject>
#include <QSettings>

// 配置中心：QSettings（组织 Greenhouse / 应用 LightControl）。
// 键名与默认值以 Tech_Design §配置项（QSettings）为准。
class Config : public QObject
{
    Q_OBJECT
public:
    static Config &instance();

    int     tcpPort() const;
    void    setTcpPort(int v);
    /* 监听地址：默认 0.0.0.0 —— 节点经局域网连入，必须监听 LAN 网卡。
     * 依据 Tech_Design §配置项（QSettings）`tcp/bindAddress`：文档已写明默认
     * 0.0.0.0，并说明"Qt 若只绑 127.0.0.1 将永远连不上"；此处实现与文档一致，
     * 无待回改项。需收紧时改为 PC 的 LAN IP，并同步防火墙放行 9092。 */
    QString bindAddress() const;
    void    setBindAddress(const QString &v);

    QString llmUrl() const;
    void    setLlmUrl(const QString &v);
    QString llmModel() const;
    void    setLlmModel(const QString &v);
    QString llmToken() const;                 // 为空时不发送 Authorization 头
    void    setLlmToken(const QString &v);
    int     llmTimeoutMs() const;
    void    setLlmTimeoutMs(int v);

    bool    ttsEnabled() const;
    void    setTtsEnabled(bool v);
    QString ttsVoice() const;                 // 运行时用 availableVoices() 校验回退
    void    setTtsVoice(const QString &v);

    QString asrLocale() const;
    void    setAsrLocale(const QString &v);

    int     chartMaxPoints() const;
    int     offlineTimeoutMs() const;         // 静默失效判定（3 × 上报周期）
    void    setOfflineTimeoutMs(int v);
    int     ackTimeoutMs() const;             // 指令 ACK 超时（command_ack_guard）
    void    setAckTimeoutMs(int v);

    /* 历史数据落盘文件（CSV）。默认 exe 同级 data/history.csv；
     * 置空字符串 = 关闭落盘。相对路径按 exe 所在目录解析。 */
    QString historyCsvPath() const;
    void    setHistoryCsvPath(const QString &v);

private:
    explicit Config(QObject *parent = nullptr);
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    QSettings m_s;
};

#endif // CONFIG_H
