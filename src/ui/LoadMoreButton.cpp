#include "LoadMoreButton.h"

LoadMoreButton::LoadMoreButton(QWidget* parent)
	: QPushButton(QStringLiteral("加载更多"), parent)
{
	setObjectName(QStringLiteral("loadMoreButton"));
	setCursor(Qt::PointingHandCursor);
	// 外观规则见 resources/styles/chat.qss（#chatScrollContent QPushButton#loadMoreButton）
}