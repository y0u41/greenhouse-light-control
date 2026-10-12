#ifndef CHARTWIDGET_H
#define CHARTWIDGET_H

#include <QChartView>
#include <QDateTimeAxis>
#include <QValueAxis>
#include <QLineSeries>
#include <QDateTime>
#include <QHash>

/* Qt 6 起 QtCharts 的类位于 QT_BEGIN_NAMESPACE（常规构建即全局命名空间），
 * 且 QT_CHARTS_USE_NAMESPACE 宏已被移除 —— 这里不能再写它，直接用 QChart/QLineSeries。 */

// 实时光强曲线：每节点一条线，保留最近 N 点（默认 300），Y 轴固定 0–4095（light 量程）。
class ChartWidget : public QChartView
{
    Q_OBJECT
public:
    explicit ChartWidget(QWidget *parent = nullptr);

    void setMaxPoints(int n);
    void updateNode(int nodeId, int light, const QDateTime &ts);
    void setNodeVisible(int nodeId, bool visible);
    void clearAll();

private:
    QChart *m_chart;
    QDateTimeAxis *m_axisX;
    QValueAxis *m_axisY;
    QHash<int, QLineSeries *> m_series;
    int m_maxPoints = 300;
};

#endif // CHARTWIDGET_H
