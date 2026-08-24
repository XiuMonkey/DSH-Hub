#include "LoadMoreButton.h"

LoadMoreButton::LoadMoreButton(QWidget *parent)
    : QPushButton(QStringLiteral("加载更多"), parent)
{
    setObjectName(QStringLiteral("loadMoreButton"));
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral(
        "QPushButton#loadMoreButton {"
        "  border: none;"
        "  background: transparent;"
        "  color: #4C8BF5;"
        "  border: 1px solid #D0D7DE;"
        "  border-radius: 15px;"
        "  background: #FFFFFF;"
        "  padding: 6px 18px;"
        "  font-size: 13px;"
        "}"
        "QPushButton#loadMoreButton:hover {"
        "  color: #3A7AE0;"
        "}"
    ));
}
