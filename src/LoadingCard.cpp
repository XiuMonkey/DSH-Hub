#include "LoadingCard.h"

#include <QTimer>

LoadingCard::LoadingCard(QWidget *parent)
    : QLabel(QStringLiteral("正在加载会话"), parent)
{
    setAlignment(Qt::AlignCenter);
    setStyleSheet(QStringLiteral(
        "QLabel {"
        "  background: #FFFFFF;"
        "  color: #57606A;"
        "  border: 1px solid #E1E4E8;"
        "  border-radius: 16px;"
        "  padding: 20px 36px;"
        "  font-size: 18px;"
        "  font-weight: 600;"
        "}"
    ));

    m_timer = new QTimer(this);
    m_timer->setInterval(350);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        m_dots = (m_dots + 1) % 4;
        setText(QStringLiteral("正在加载会话") + QString(m_dots, QLatin1Char('.')));
    });
    m_timer->start();
}
