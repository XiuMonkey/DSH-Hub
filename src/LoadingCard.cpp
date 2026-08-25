#include "LoadingCard.h"
#include "ThemeManager.h"

#include <QTimer>

LoadingCard::LoadingCard(QWidget *parent)
    : QLabel(QStringLiteral("正在加载会话"), parent)
{
    setAlignment(Qt::AlignCenter);
    setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + QStringLiteral("#57606A") + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + QStringLiteral("#E1E4E8") + QStringLiteral(";") + QStringLiteral("  border-radius: 16px;") + QStringLiteral("  padding: 20px 36px;") + QStringLiteral("  font-size: 18px;") + QStringLiteral("  font-weight: 600;") + QStringLiteral("}"));

    m_timer = new QTimer(this);
    m_timer->setInterval(350);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        m_dots = (m_dots + 1) % 4;
        setText(QStringLiteral("正在加载会话") + QString(m_dots, QLatin1Char('.')));
    });
    m_timer->start();
}
