#include "LoadMoreButton.h"
#include "ThemeManager.h"

LoadMoreButton::LoadMoreButton(QWidget *parent)
    : QPushButton(QStringLiteral("加载更多"), parent)
{
    setObjectName(QStringLiteral("loadMoreButton"));
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral("QPushButton#loadMoreButton {") + QStringLiteral("  border: none;") + QStringLiteral("  background: transparent;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + QStringLiteral("#D0D7DE") + QStringLiteral(";") + QStringLiteral("  border-radius: 15px;") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  padding: 6px 18px;") + QStringLiteral("  font-size: 13px;") + QStringLiteral("}") + QStringLiteral("QPushButton#loadMoreButton:hover {") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("accentHover")) + QStringLiteral(";") + QStringLiteral("}"));
}
