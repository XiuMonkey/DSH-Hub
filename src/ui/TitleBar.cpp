// ------------------------------------------------------------------
// TitleBar.cpp
// ------------------------------------------------------------------
// 自绘标题栏：左侧应用图标 + 右侧三个窗口按钮，没有任何窗口状态逻辑 ——
// 按钮点击只发意图信号，动作在 common/WindowFrame.cpp 落地。
//
// 三个按钮是普通 QPushButton：形状/底色/悬停/按下/字形颜色全部由 QSS 决定
// （resources/styles/main-window.qss 的 #windowMinButton / #windowMaxButton /
// #windowCloseButton 规则），本文件只负责建控件、换字形（最大化 ↔ 还原）。
// ------------------------------------------------------------------

#include "TitleBar.h"

#include "ThemeManager.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>

namespace
{
	// 窗口按钮尺寸：对齐 Windows 11 标题栏按钮（46x32）。
	// 顶部留 5px 空白，把最上面那条留给窗口缩放热区
	// （common/WindowFrame.cpp 的边缘判定范围），避免点按钮变成拉边框。
	constexpr int kButtonWidth = 46;
	constexpr int kButtonHeight = 32;
	constexpr int kButtonTopMargin = 5;
	// 三个按钮整体离窗口右缘的距离（不贴边）
	constexpr int kButtonRightMargin = 6;

	// 标题栏左侧图标高度（原图 139x138，按 2 倍取样后缩放，任意 DPI 下都清晰）
	constexpr int kLogoHeight = 20;

	// 按钮字形：用文字表达，颜色与悬停交给 QSS。字体在 QSS 里指定为
	// "Segoe UI Symbol"（−/□/❐/✕ 都有字形），缺失时回退主题字体。
	//
	// 必须走 QStringLiteral：这些字形是多字节 UTF-8，若声明成 const char[] 再用
	// QLatin1String() 取，会被按“一字节一字符”解释，直接渲染成乱码。
	QString minimizeGlyph() { return QStringLiteral("−"); }  // U+2212 MINUS SIGN
	QString maximizeGlyph() { return QStringLiteral("□"); }  // U+25A1 WHITE SQUARE
	QString restoreGlyph() { return QStringLiteral("❐"); }   // U+2750 SHADOWED WHITE SQUARE
	QString closeGlyph() { return QStringLiteral("✕"); }     // U+2715 MULTIPLICATION X

	QPushButton* makeWindowButton(const QString& objectName, const QString& glyph,
		const QString& tooltip, QWidget* parent)
	{
		auto* button = new QPushButton(glyph, parent);
		button->setObjectName(objectName);
		// 命中测试约定：带这个属性的控件不当作标题栏拖动区（见 common/WindowFrame.cpp）
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

	// 左侧：应用图标（原图 139x138，按 2 倍目标高度缩放后标 DPR=2）
	m_logoLabel = new QLabel(this);
	m_logoLabel->setObjectName(QStringLiteral("windowTitleLogo"));
	m_logoLabel->setAttribute(Qt::WA_TranslucentBackground, true);
	m_logoLabel->setAlignment(Qt::AlignCenter);

	QPixmap logo(QStringLiteral(":/DSHHub/DSH-Hub-Icon.png"));
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
	layout->addStretch(1);

	// 右侧：最小化 / 最大化（还原）/ 关闭（外观见 main-window.qss）
	m_minimizeButton = makeWindowButton(QStringLiteral("windowMinButton"),
		minimizeGlyph(), tr("最小化"), this);
	m_maximizeButton = makeWindowButton(QStringLiteral("windowMaxButton"),
		maximizeGlyph(), tr("最大化"), this);
	m_closeButton = makeWindowButton(QStringLiteral("windowCloseButton"),
		closeGlyph(), tr("关闭"), this);
	layout->addWidget(m_minimizeButton, 0, Qt::AlignTop);
	layout->addWidget(m_maximizeButton, 0, Qt::AlignTop);
	layout->addWidget(m_closeButton, 0, Qt::AlignTop);

	// 按钮只发意图，动作由 common/WindowFrame 落地（接线在 DSHHub 构造函数）
	connect(m_minimizeButton, &QPushButton::clicked,
		this, &TitleBar::minimizeRequested);
	connect(m_maximizeButton, &QPushButton::clicked,
		this, &TitleBar::maximizeRestoreRequested);
	connect(m_closeButton, &QPushButton::clicked,
		this, &TitleBar::closeRequested);
}

void TitleBar::setMaximizedState(bool maximized)
{
	m_maximized = maximized;

	if (!m_maximizeButton)
		return;

	m_maximizeButton->setText(maximized ? restoreGlyph() : maximizeGlyph());
	m_maximizeButton->setToolTip(maximized ? tr("还原") : tr("最大化"));
}

void TitleBar::retranslateUi()
{
	if (m_minimizeButton)
		m_minimizeButton->setToolTip(tr("最小化"));
	if (m_closeButton)
		m_closeButton->setToolTip(tr("关闭"));

	// 最大化按钮的提示与字形都跟状态有关，交给同一个入口重设
	setMaximizedState(m_maximized);
}

void TitleBar::changeEvent(QEvent* event)
{
	QWidget::changeEvent(event);

	if (event->type() == QEvent::LanguageChange)
		retranslateUi();
}