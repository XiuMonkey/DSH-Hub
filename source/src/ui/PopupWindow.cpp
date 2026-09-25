#include "ui/PopupWindow.h"
#include "ui/LayoutUtils.h"
#include "core/ConnectionManager.h"
#include "ui/ShadowPanel.h"
#include "common/appearance/ThemeManager.h"
#include "common/appearance/WindowFrame.h"

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

	auto* body = new QWidget(this);
	body->setObjectName(QStringLiteral("popupBody"));
	body->setAttribute(Qt::WA_StyledBackground, true);

	// 弹窗外套最高一档阴影外壳（lv3）；外壳透明，只在四周留白里画阴影
	auto* bodyPanel = new ShadowPanel(QStringLiteral("shadowFloat"), CardShadow::level3(), this);
	bodyPanel->setRadius(16);
	bodyPanel->setCard(body);

	auto* outerLayout = new QVBoxLayout(this);
	outerLayout->setContentsMargins(0, 0, 0, 0);
	outerLayout->addWidget(bodyPanel);

	m_mainLayout = new QVBoxLayout(body);
	m_mainLayout->setContentsMargins(20, 16, 20, 20);
	m_mainLayout->setSpacing(12);

	auto* headerLayout = new QHBoxLayout;
	headerLayout->setSpacing(8);
	headerLayout->setContentsMargins(0, -4, -4, 0);

	m_titleLabel = new QLabel(body);
	m_titleLabel->setObjectName(QStringLiteral("popupTitle"));

	m_closeButton = new QPushButton(QStringLiteral("✕"), body);
	m_closeButton->setObjectName(QStringLiteral("popupCloseButton"));
	m_closeButton->setFixedSize(28, 28);
	m_closeButton->setCursor(Qt::PointingHandCursor);
	// 弹窗有多个实例，index 带上自身地址
	dshRegister(QStringLiteral("PopupWindow.close.%1").arg(reinterpret_cast<quintptr>(this)), m_closeButton,
		qOverload<bool>(&QPushButton::clicked), this, &QWidget::close);

	headerLayout->addWidget(m_titleLabel, 1);
	headerLayout->addWidget(m_closeButton, 0, Qt::AlignTop);

	m_mainLayout->addLayout(headerLayout);

	m_contentLayout = new QVBoxLayout;
	m_contentLayout->setContentsMargins(0, 0, 0, 0);
	m_contentLayout->setSpacing(0);
	m_mainLayout->addLayout(m_contentLayout, 1);

	ThemeManager::instance().applyToWindow(this);

	retranslateUi();
}

void PopupWindow::setTitle(const QString& title)
{
	m_title = title;
	retranslateUi();
}

void PopupWindow::retranslateUi()
{
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

	LayoutUtils::clearLayout(m_contentLayout, LayoutUtils::ClearMode::DeferOnly);

	m_contentLayout->addWidget(content);
}

void PopupWindow::openHosted()
{
	if (!m_popupHost || isVisible())
		return;

	// 铺遮罩 + 居中 + 显示必须在同一帧里发出去（见 WindowFrame::showOverlayWithPopup）
	WindowFrame::showOverlayWithPopup(m_popupHost, this, this);

	// 数据异步加载刻意放在 show() 之后：refreshOnOpen() 往往最耗时，放前面会"卡一下才出现"
	refreshOnOpen();
}

void PopupWindow::closeHosted()
{
	// 先收遮罩、再隐藏自己：反过来观感是"窗口没了、遮罩还留一拍"
	WindowFrame::hideOverlay(m_popupHost, this);
	hide();
}

void PopupWindow::syncOverlayToHost()
{
	WindowFrame::syncOverlay(m_popupHost);
}

void PopupWindow::closeEvent(QCloseEvent* event)
{
	emit closed();
	QWidget::closeEvent(event);
}
