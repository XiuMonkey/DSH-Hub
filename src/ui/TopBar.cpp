#include "TopBar.h"

#include <QHBoxLayout>
#include <QLabel>

TopBar::TopBar(QWidget* parent)
	: QWidget(parent)
{
	setObjectName(QStringLiteral("topBar"));
	setAttribute(Qt::WA_StyledBackground, true);
	setFixedHeight(48);
	setFixedWidth(890); // 留出 10px 左边距，与 Agent 消息左边缘对齐

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(16, 0, 16, 0);
	layout->setSpacing(0);

	m_titleLabel = new QLabel(QStringLiteral("未命名会话"), this);
	m_titleLabel->setObjectName(QStringLiteral("topBarTitle"));
	layout->addWidget(m_titleLabel);
	layout->addStretch();
}

void TopBar::setTitle(const QString& title)
{
	if (m_titleLabel)
		m_titleLabel->setText(title.isEmpty() ? QStringLiteral("未命名会话") : title);
}