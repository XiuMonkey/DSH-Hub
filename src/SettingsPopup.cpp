#include "SettingsPopup.h"

#include <QLabel>

SettingsPopup::SettingsPopup(QWidget *parent)
    : PopupWindow(parent)
{
    setTitle(QStringLiteral("设置"));

    auto *placeholder = new QLabel(QStringLiteral("设置功能开发中..."), this);
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  background: transparent;"
        "  color: #6B7280;"
        "  font-size: 14px;"
        "}"
    ));
    setContent(placeholder);

    resize(480, 360);
}
