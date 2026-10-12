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
     * 注：Tech_Design 旧文写"绑定 127.0.0.1"，与"节点 CIPSTART 目标为 PC 局域网 IP"矛盾；
     *     此处按功能优先取 LAN 绑定（不等于暴露公网），待用户确认后回改文档。 */
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

private:
    explicit Config(QObject *parent = nullptr);
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    QSettings m_s;
};

#endif // CONFIG_H
