#include "presentation/widgets/sky_banner.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPolygon>
#include <QRandomGenerator>

namespace gmst {

SkyBanner::SkyBanner(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(58);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void SkyBanner::setHeroMode(bool enabled)
{
    if (m_heroMode == enabled) return;
    m_heroMode = enabled;
    update();
}

void SkyBanner::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    QLinearGradient sky(0, 0, 0, height());
    if (m_heroMode) {
        sky.setColorAt(0.0, QColor(QStringLiteral("#5aa7ef")));
        sky.setColorAt(0.46, QColor(QStringLiteral("#a8d8ef")));
        sky.setColorAt(0.48, QColor(QStringLiteral("#477fb4")));
        sky.setColorAt(0.72, QColor(QStringLiteral("#4f8b58")));
        sky.setColorAt(1.0, QColor(QStringLiteral("#273c48")));
    } else {
        sky.setColorAt(0.0, QColor(QStringLiteral("#75b8f3")));
        sky.setColorAt(0.45, QColor(QStringLiteral("#c0e5f5")));
        sky.setColorAt(0.56, QColor(QStringLiteral("#4b99c9")));
        sky.setColorAt(0.7, QColor(QStringLiteral("#477748")));
        sky.setColorAt(1.0, QColor(QStringLiteral("#355237")));
    }
    painter.fillRect(rect(), sky);

    const int period = m_heroMode ? 190 : 145;
    const int base = height();
    for (int x = -period; x < width() + period; x += period) {
        QPolygon ridge;
        ridge << QPoint(x, base) << QPoint(x + period * 0.54, base * 0.32)
              << QPoint(x + period * 0.64, base * 0.32) << QPoint(x + period * 0.1, base);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#35709c")));
        painter.drawPolygon(ridge);

        QPolygon greenRidge;
        greenRidge << QPoint(x + period * 0.18, base) << QPoint(x + period * 0.73, base * 0.25)
                   << QPoint(x + period * 0.84, base * 0.25) << QPoint(x + period * 0.29, base);
        painter.setBrush(QColor(QStringLiteral("#559143")));
        painter.drawPolygon(greenRidge);
    }

    painter.setPen(QColor(255, 255, 255, 230));
    QFont cloudFont(QStringLiteral("Segoe UI Symbol"), m_heroMode ? 22 : 19);
    painter.setFont(cloudFont);
    const QString cloud = QStringLiteral("☁   ✦       ☁       ✧       ☁");
    painter.drawText(QRect(12, 0, width() - 20, m_heroMode ? 35 : 30), Qt::AlignLeft | Qt::AlignVCenter, cloud);
    painter.setPen(QColor(QStringLiteral("#6385c4")));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(1, 1, -2, -2));
}

} // namespace gmst
