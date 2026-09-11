#include "LoadMoreButton.h"

#include <QEvent>

LoadMoreButton::LoadMoreButton(QWidget* parent)
	: QPushButton(parent)
{
	setObjectName(QStringLiteral("loadMoreButton"));
	setCursor(Qt::PointingHandCursor);
	// 外观规则见 resources/styles/chat.qss（#chatScrollContent QPushButton#loadMoreButton）
	setText(tr("加载更多"));
}

void LoadMoreButton::changeEvent(QEvent* event)
{
	QPushButton::changeEvent(event);

	// 语言切换后按钮文案要跟着换（宿主也会在刷新时重设，这里保证切语言即时生效）
	if (event->type() == QEvent::LanguageChange)
		setText(tr("加载更多"));
}
