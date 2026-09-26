// 自绘标题栏：左侧图标 + 右侧三个窗口按钮；点击只发意图信号，动作在 common/WindowFrame.cpp 落地。
// 按钮外观全走 QSS，本文件只建控件、换字形。

#include "ui/TitleBar.h"
#include "core/ConnectionManager.h"

#include "common/appearance/ThemeManager.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>

namespace
{
	// 顶部留 5px 给窗口缩放热区，避免点按钮变成拉边框
	constexpr int kButtonWidth = 46;
	constexpr int kButtonHeight = 32;
	constexpr int kButtonTopMargin = 5;
	constexpr int kButtonRightMargin = 6;

	constexpr int kLogoHeight = 20;

	constexpr int kLogoTextGap = 8;

	// 字形必须走 QStringLiteral：多字节 UTF-8 声明成 const char[] 再经 QLatin1String() 会渲染成乱码
	QString minimizeGlyph() { return QStringLiteral("−"); }
	QString maximizeGlyph() { return QStringLiteral("□"); }
	QString restoreGlyph() { return QStringLiteral("❐"); }
	QString closeGlyph() { return QStringLiteral("✕"); }

	QPushButton* makeWindowButton(const QString& objectName, const QString& glyph, const QString& tooltip,
		QWidget* parent)
	{
		auto* button = new QPushButton(glyph, parent);
		button->setObjectName(objectName);
		// 命中测试约定：带这个属性的控件不当作标题栏拖动区
		button->setProperty("dshWindowControl", true);
		button->setFixedSize(kButtonWidth, kButtonHeight);
		button->setCursor(Qt::ArrowCursor);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(tooltip);
		return button;
	}
}

TitleBar::TitleBar(QWidget* parent)
	: QWidget(parent)
{
	setObjectName(QStringLiteral("windowTitleBar"));
	setAttribute(Qt::WA_StyledBackground, true);
	setFixedHeight(kHeight);

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(14, kButtonTopMargin, kButtonRightMargin, 0);
	layout->setSpacing(0);

	m_logoLabel = new QLabel(this);
	m_logoLabel->setObjectName(QStringLiteral("windowTitleLogo"));
	m_logoLabel->setAttribute(Qt::WA_TranslucentBackground, true);
	m_logoLabel->setAlignment(Qt::AlignCenter);

	// 深色底用反色版（同目录 PNG 逐像素 RGB 取反）；切主题会重建窗口，构造期判断一次即可
	const QString logoResource = ThemeManager::instance().isDark()
		? QStringLiteral(":/DSHHub/DSH-Hub-Icon-Dark.png")
		: QStringLiteral(":/DSHHub/DSH-Hub-Icon.png");
	QPixmap logo(logoResource);
	if (!logo.isNull()) {
		logo = logo.scaledToHeight(kLogoHeight * 2, Qt::SmoothTransformation);
		logo.setDevicePixelRatio(2.0);
		m_logoLabel->setPixmap(logo);
		m_logoLabel->setFixedSize(logo.width() / 2, logo.height() / 2);
	}
	else {
		m_logoLabel->setText(QStringLiteral("DSH Hub"));
	}

	layout->addWidget(m_logoLabel, 0, Qt::AlignVCenter);

	// 产品名 + 版本号；各语言写法一致，不走 qtTrId
	auto* titleText = new QLabel(QStringLiteral("DSH Hub Alpha 1.5.1"), this);
	titleText->setObjectName(QStringLiteral("windowTitleText"));
	titleText->setAttribute(Qt::WA_TranslucentBackground, true);
	layout->addSpacing(kLogoTextGap);
	layout->addWidget(titleText, 0, Qt::AlignVCenter);

	layout->addStretch(1);

	m_minimizeButton = makeWindowButton(QStringLiteral("windowMinButton"),
		minimizeGlyph(), qtTrId("titlebar_minimize"), this);
	m_maximizeButton = makeWindowButton(QStringLiteral("windowMaxButton"),
		maximizeGlyph(), qtTrId("titlebar_maximize"), this);
	m_closeButton = makeWindowButton(QStringLiteral("windowCloseButton"),
		closeGlyph(), qtTrId("common_close"), this);
	layout->addWidget(m_minimizeButton, 0, Qt::AlignTop);
	layout->addWidget(m_maximizeButton, 0, Qt::AlignTop);
	layout->addWidget(m_closeButton, 0, Qt::AlignTop);

	// 按钮只发意图，动作由 common/WindowFrame 落地
	dshRegister("TitleBar.001", m_minimizeButton, qOverload<bool>(&QPushButton::clicked),
		this, &TitleBar::minimizeRequested);
	dshRegister("TitleBar.002", m_maximizeButton, qOverload<bool>(&QPushButton::clicked),
		this, &TitleBar::maximizeRestoreRequested);
	dshRegister("TitleBar.003", m_closeButton, qOverload<bool>(&QPushButton::clicked),
		this, &TitleBar::closeRequested);
}

void TitleBar::setMaximizedState(bool maximized)
{
	m_maximized = maximized;
	if (!m_maximizeButton)
		return;

	m_maximizeButton->setText(maximized ? restoreGlyph() : maximizeGlyph());
	m_maximizeButton->setToolTip(maximized ? qtTrId("titlebar_restore") : qtTrId("titlebar_maximize"));
}

void TitleBar::retranslateUi()
{
	if (m_minimizeButton)
		m_minimizeButton->setToolTip(qtTrId("titlebar_minimize"));
	if (m_closeButton)
		m_closeButton->setToolTip(qtTrId("common_close"));

	// 最大化按钮的提示与字形都跟状态有关，走同一个入口
	setMaximizedState(m_maximized);
}

void TitleBar::changeEvent(QEvent* event)
{
	QWidget::changeEvent(event);

	if (event->type() == QEvent::LanguageChange)
		retranslateUi();
}
