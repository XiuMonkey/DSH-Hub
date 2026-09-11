#include "TopBar.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>

TopBar::TopBar(QWidget* parent)
	: QWidget(parent)
{
	setObjectName(QStringLiteral("topBar"));
	setAttribute(Qt::WA_StyledBackground, true);
	setFixedHeight(48);
	// 宽度与消息内容一致（见 Main.cpp 的宽度约定：1152 - 左右各 16px 留白）
	setFixedWidth(1120);

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(16, 0, 16, 0);
	layout->setSpacing(0);

	m_titleLabel = new QLabel(this);
	m_titleLabel->setObjectName(QStringLiteral("topBarTitle"));
	layout->addWidget(m_titleLabel);
	layout->addStretch();

	setTitle(QString());
}

void TopBar::setTitle(const QString& title)
{
	m_title = title;
	retranslateUi();
}

void TopBar::retranslateUi()
{
	if (m_titleLabel)
		m_titleLabel->setText(m_title.isEmpty() ? tr("未命名会话") : m_title);
}

void TopBar::changeEvent(QEvent* event)
{
	QWidget::changeEvent(event);

	if (event->type() == QEvent::LanguageChange)
		retranslateUi();
}