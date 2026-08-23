#include "SessionButton.h"

#include <QFontMetrics>
#include <QResizeEvent>
#include <QSizePolicy>

SessionButton::SessionButton(const QString &sessionId,
                             const QString &title,
                             QWidget *parent)
    : QPushButton(parent)
    , m_sessionId(sessionId)
    , m_fullTitle(title.isEmpty() ? sessionId : title)
{
    setObjectName(QStringLiteral("sessionButton"));
    setCheckable(true);
    setAutoExclusive(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(32);
    setCursor(Qt::PointingHandCursor);

    connect(this, &QPushButton::clicked, this, &SessionButton::handleClicked);

    updateElidedText();
}

QString SessionButton::sessionId() const
{
    return m_sessionId;
}

QString SessionButton::fullTitle() const
{
    return m_fullTitle;
}

void SessionButton::setSessionTitle(const QString &title)
{
    m_fullTitle = title.isEmpty() ? m_sessionId : title;
    updateElidedText();
}

void SessionButton::setSelected(bool selected)
{
    setChecked(selected);
}

void SessionButton::resizeEvent(QResizeEvent *event)
{
    QPushButton::resizeEvent(event);
    updateElidedText();
}

void SessionButton::handleClicked()
{
    emit sessionClicked(m_sessionId);
}

void SessionButton::updateElidedText()
{
    const int availableWidth = width() - 20;
    if (availableWidth <= 0) {
        setText(m_fullTitle);
        return;
    }

    const QFontMetrics fm(font());
    setText(fm.elidedText(m_fullTitle, Qt::ElideRight, availableWidth));
}
