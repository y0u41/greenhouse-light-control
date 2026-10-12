#include "chartwidget.h"

#include <QDateTime>
#include <QPainter>

ChartWidget::ChartWidget(QWidget *parent)
    : QChartView(parent)
{
    m_chart = new QChart();
    m_chart->setTitle(QStringLiteral("光强实时曲线（light 0–4095）"));
    m_chart->legend()->setAlignment(Qt::AlignBottom);
    setChart(m_chart);
    setRenderHint(QPainter::Antialiasing);

    m_axisX = new QDateTimeAxis;
    m_axisX->setTitleText(QStringLiteral("时间"));
    m_axisX->setFormat(QStringLiteral("hh:mm:ss"));
    m_chart->addAxis(m_axisX, Qt::AlignBottom);

    m_axisY = new QValueAxis;
    m_axisY->setTitleText(QStringLiteral("light"));
    m_axisY->setRange(0, 4095);          /* 固定量程，避免自动缩放掩盖"越限"事实 */
    m_chart->addAxis(m_axisY, Qt::AlignLeft);
}

void ChartWidget::setMaxPoints(int n)
{
    m_maxPoints = (n > 10) ? n : 10;
}

void ChartWidget::updateNode(int nodeId, int light, const QDateTime &ts)
{
    QLineSeries *s = m_series.value(nodeId, nullptr);
    if (s == nullptr) {
        s = new QLineSeries;
        s->setName(QStringLiteral("节点 %1").arg(nodeId));
        m_chart->addSeries(s);
        s->attachAxis(m_axisX);
        s->attachAxis(m_axisY);
        m_series.insert(nodeId, s);
    }

    s->append(ts.toMSecsSinceEpoch(), light);
    while (s->count() > m_maxPoints) {
        s->remove(0);                    /* 只保留最近 N 点，防止内存无界增长 */
    }

    /* X 轴跟随最新点，窗口宽度固定为 N 个上报周期（约 N 秒） */
    const qint64 spanMs = (qint64)m_maxPoints * 1000;
    const qint64 end = ts.toMSecsSinceEpoch();
    m_axisX->setRange(QDateTime::fromMSecsSinceEpoch(end - spanMs),
                      QDateTime::fromMSecsSinceEpoch(end));
}

void ChartWidget::setNodeVisible(int nodeId, bool visible)
{
    if (QLineSeries *s = m_series.value(nodeId, nullptr)) {
        s->setVisible(visible);
    }
}

void ChartWidget::clearAll()
{
    for (QLineSeries *s : m_series) {
        s->clear();
    }
}
