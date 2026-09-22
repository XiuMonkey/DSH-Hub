#include "ui/PopupWindow.h"
#include "ui/LayoutUtils.h"
#include "ui/ShadowPanel.h"
#include "common/appearance/ThemeManager.h"

#include <QCloseEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPushButton>
#include <QVBoxLayout>

PopupWindow::PopupWindow(QWidget* parent)
	: QWidget(parent)
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
	setAttribute(Qt::WA_TranslucentBackground);
	setAttribute(Qt::WA_StyledBackground, true);

	// 白色圆角主体
	auto* body = new QWidget(this);
	body->setObjectName(QStringLiteral("popupBody"));
	body->setAttribute(Qt::WA_StyledBackground, true);

	// 弹窗外面套阴影外壳：弹窗是"浮起来的"一层，用最高一档阴影（lv3）。
	// 外壳透明，只在四周留白里画阴影 —— 原来的 1px 边距（outerLayout）撑不下，
	// 所以整窗尺寸会随留白一起变大，主体本身尺寸不变。
	auto* bodyPanel = new ShadowPanel(QStringLiteral("shadowFloat"),
		CardShadow::level3(), this);
	bodyPanel->setRadius(16); // 与 #popupBody 的 QSS 圆角一致
	bodyPanel->setCard(body);

	auto* outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);
	outerLayout->addWidget(bodyPanel);

	m_mainLayout = new QVBoxLayout(body);
	m_mainLayout->setContentsMargins(20, 16, 20, 20);
	m_mainLayout->setSpacing(12);

	// 标题栏 + 关闭按钮
	auto* headerLayout = new QHBoxLayout;
	headerLayout->setSpacing(8);
	headerLayout->setContentsMargins(0, -4, -4, 0); // 关闭按钮向上、向右各靠近 4px

	m_titleLabel = new QLabel(body);
	m_titleLabel->setObjectName(QStringLiteral("popupTitle"));

	m_closeButton = new QPushButton(QStringLiteral("✕"), body);
	m_closeButton->setObjectName(QStringLiteral("popupCloseButton"));
	m_closeButton->setFixedSize(28, 28);
	m_closeButton->setCursor(Qt::PointingHandCursor);
	connect(m_closeButton, &QPushButton::clicked, this, &QWidget::close);

	headerLayout->addWidget(m_titleLabel, 1);
	headerLayout->addWidget(m_closeButton, 0, Qt::AlignTop);

	m_mainLayout->addLayout(headerLayout);

	// 内容区
	m_contentLayout = new QVBoxLayout;
	m_contentLayout->setContentsMargins(0, 0, 0, 0);
	m_contentLayout->setSpacing(0);
	m_mainLayout->addLayout(m_contentLayout, 1);

	// 窗口级样式：本弹窗与后续 setContent 加入的内容统一应用当前主题样式表
	ThemeManager::instance().applyToWindow(this);

	// 标题留空 -> 显示可翻译的默认名（子类可用 setTitle 覆盖）
	retranslateUi();
}

void PopupWindow::setTitle(const QString& title)
{
	m_title = title;
	retranslateUi();
}

void PopupWindow::retranslateUi()
{
	// 标题栏左侧文案 + 关闭按钮的无障碍名/提示都随语言走
	if (m_titleLabel)
		m_titleLabel->setText(m_title.isEmpty() ? qtTrId("popup_title") : m_title);
	if (m_closeButton)
		m_closeButton->setToolTip(qtTrId("common_close"));
}

void PopupWindow::changeEvent(QEvent* event)
{
	QWidget::changeEvent(event);

	if (event->type() == QEvent::LanguageChange)
		retranslateUi();
}

void PopupWindow::setContent(QWidget* content)
{
	if (!content)
		return;

	// 移除旧内容
	LayoutUtils::clearLayout(m_contentLayout, LayoutUtils::ClearMode::DeferOnly);

	m_contentLayout->addWidget(content);
}

void PopupWindow::closeEvent(QCloseEvent* event)
{
	emit closed();
	QWidget::closeEvent(event);
}