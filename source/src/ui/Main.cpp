// DSHHub 主窗口 UI 搭建（见 DSHHub::buildUi）：外观由 Theme 从 styles/*.qss 统一安装，控件只提供 objectName。

#include "core/DSHHub.h"
#include "core/ConnectionManager.h"
#include "ui/ChatInputWidget.h"
#include "ui/LayoutUtils.h"
#include "ui/LoadMoreButton.h"
#include "ui/ShadowPanel.h"
#include "ui/Sidebar.h"
#include "ui/SpinnerWidget.h"
#include "common/appearance/ThemeManager.h"
#include "ui/TitleBar.h"
#include "ui/TopBar.h"
#include "common/appearance/WindowFrame.h"

#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
	constexpr int kSidebarWidth = 240;
	// 消息与输入卡片共用同一份留白
	constexpr int kColumnSideClearance = 16;
	constexpr int kWindowMargin = 24;
	constexpr int kPreferredWindowWidth = 1440;
	constexpr int kPreferredWindowHeight = 880;
	constexpr int kMinConversationColumnWidth = 320;
	// 要算进窗口高度，否则内容区会矮一截
	constexpr int kTitleBarHeight = TitleBar::kHeight;
	constexpr int kContentSideInset = 9;
	// 侧栏阴影比输入卡片那一档收窄：原版 12px 模糊照搬到贴着窗口边框的常驻侧栏上，
	// 留白会明显挤窄会话列
	const CardShadow::Spec kSidebarShadow = { 8, 2, 12 };
	// 阴影本身已带来一圈留白（见 CardShadow::padding），这里只补 2px
	constexpr int kPanelGap = 2;

	// 与侧栏同一档，单独一份只为能只调顶栏
	const CardShadow::Spec kTopBarShadow = { 8, 2, 12 };
	// 现由阴影外壳的下留白承担，外壳外的布局间距归零
	constexpr int kTopBarShadowGap = 8;
}

void DSHHub::buildUi()
{
	// 高 DPI 小屏（2560x1600 @200% => 逻辑仅 1280x800）上任何写死的宽/高都会让窗口超出屏幕、
	// 输入卡片首当其冲被裁掉，所以一律以可用桌面为上限
	const QRect available = screen()
		? screen()->availableGeometry()
		: QRect(0, 0, kPreferredWindowWidth, kPreferredWindowHeight);

	const int windowWidth = qMin(kPreferredWindowWidth, available.width() - 2 * kWindowMargin);
	const int windowHeight = qBound(480, kPreferredWindowHeight + kTitleBarHeight,
		available.height() - 2 * kWindowMargin);

	const int windowInner = windowWidth - 2 * kWindowMargin;

	// 会话列宽只从窗口内宽里扣侧栏本体、不扣阴影留白，否则输入卡片与消息列会整体变窄
	const int columnWidth = qMax(kMinConversationColumnWidth, windowInner - kSidebarWidth);

	// 侧栏有阴影外壳，内容块比 windowInner 宽出阴影那部分；多出的宽度从窗口留白里出
	const QMargins sidebarShadowPad = CardShadow::padding(kSidebarShadow);
	const int sidebarOuterWidth = kSidebarWidth + sidebarShadowPad.left() + sidebarShadowPad.right();

	// 不能超出 bodyRow 给得出的宽度，否则会被裁掉
	const int contentWidth = qMin(sidebarOuterWidth + kPanelGap + columnWidth,
		windowWidth - 2 * kContentSideInset);

	auto* central = new QWidget(this);
	central->setObjectName(QStringLiteral("dshhubCentral"));
	central->setAttribute(Qt::WA_StyledBackground, true);
	auto* layout = new QVBoxLayout(central);
	// 客户区铺满整个窗口：圆角与 1px 描边都由 #dshhubCentral 自己画，四周不留边距
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	// 自绘标题栏：控件只管画与发意图，拖动/双击/吸附由系统按 HTCAPTION 处理
	m_titleBar = new TitleBar(central);
	layout->addWidget(m_titleBar);

	dshRegister("Main.001", m_titleBar, &TitleBar::minimizeRequested, this,
		[this]() { WindowFrame::minimize(this); });
	dshRegister("Main.002", m_titleBar, &TitleBar::maximizeRestoreRequested, this,
		[this]() { WindowFrame::toggleMaximize(this); });
	dshRegister("Main.003", m_titleBar, &TitleBar::closeRequested, this,
		[this]() { WindowFrame::closeWindow(this); });

	m_scrollArea = LayoutUtils::makeThemedScrollArea(central, QStringLiteral("chatScrollArea"));

	auto* scrollContent = new QWidget;
	scrollContent->setObjectName(QStringLiteral("chatScrollContent"));
	scrollContent->setAttribute(Qt::WA_StyledBackground, true);
	auto* scrollLayout = new QVBoxLayout(scrollContent);
	// 4px + 消息阴影外壳自带的 6px 左留白 = 原来那 10px
	scrollLayout->setContentsMargins(4, 0, 0, 0);
	scrollLayout->setAlignment(Qt::AlignTop);

	m_messagesLayout = scrollLayout;
	scrollContent->setLayout(scrollLayout);

	m_loadMoreButton = new LoadMoreButton(scrollContent);
	m_loadMoreButton->hide();
	scrollLayout->insertWidget(0, m_loadMoreButton, 0, Qt::AlignHCenter);

	m_scrollArea->setWidget(scrollContent);
	m_scrollArea->setAlignment(Qt::AlignTop | Qt::AlignLeft);

	auto* rightPanel = new QWidget(central);
	rightPanel->setObjectName(QStringLiteral("chatPanel"));
	rightPanel->setFixedWidth(columnWidth);
	rightPanel->setAttribute(Qt::WA_StyledBackground, true);

	m_chatInput = new ChatInputWidget(rightPanel);

	m_toastLabel = new QLabel(qtTrId("common_no_more_items"), this);
	m_toastLabel->setObjectName(QStringLiteral("toastLabel"));
	m_toastLabel->setAlignment(Qt::AlignCenter);
	m_toastLabel->hide();

	auto* panelLayout = new QVBoxLayout(rightPanel);
	panelLayout->setContentsMargins(0, 0, 0, 0);
	panelLayout->setSpacing(0);

	// 外壳自己透明，只在四周留白里画阴影，#sidebar 的 QSS 一条都不用改（见 ShadowPanel.h）
	m_sidebar = new Sidebar(central);
	m_sidebar->setFixedWidth(kSidebarWidth);
	auto* sidebarShadow = new ShadowPanel(QStringLiteral("shadow"), kSidebarShadow, central);
	sidebarShadow->setRadius(12); // 与 #sidebar 的 QSS 圆角一致，阴影形状才对得上
	sidebarShadow->setCard(m_sidebar);
	// 上留白置 0：外壳默认上留白 = blur - dy 会把卡片上沿压低一截
	QMargins sidebarCardPad = sidebarShadowPad;
	sidebarCardPad.setTop(0);
	sidebarShadow->setPadding(sidebarCardPad);

	auto* scrollRow = new QHBoxLayout;
	scrollRow->setContentsMargins(kColumnSideClearance, 0, kColumnSideClearance, 0);
	scrollRow->setSpacing(0);
	scrollRow->addWidget(m_scrollArea, 1);

	panelLayout->addLayout(scrollRow, 1);

	// 边距 = 原边距 - 外壳留白，卡片左右边缘仍在原位置；纵向多出的高度由消息区让出
	const QMargins inputShadowPad = CardShadow::padding(ChatInputWidget::shadowSpec());
	const int kInputTopClearance = 0;
	// 底部留白取侧栏阴影外壳的下留白，改档位时对齐不会失效
	const int kInputBottomClearance = CardShadow::padding(kSidebarShadow).bottom();

	auto* inputLayout = new QHBoxLayout;
	inputLayout->addWidget(m_chatInput, 1);

	inputLayout->setContentsMargins(
		qMax(0, kColumnSideClearance - inputShadowPad.left()), kInputTopClearance,
		qMax(0, kColumnSideClearance - inputShadowPad.right()), kInputBottomClearance);
	inputLayout->setSpacing(0);
	panelLayout->addLayout(inputLayout);

	auto* rightColumn = new QWidget(central);
	rightColumn->setFixedWidth(columnWidth);
	auto* rightColumnLayout = new QVBoxLayout(rightColumn);
	rightColumnLayout->setContentsMargins(0, 0, 0, 0);
	// 间距归零：两者的和才是「顶栏卡片 → 会话列」的距离
	rightColumnLayout->setSpacing(0);

	m_topBar = new TopBar(rightColumn);
	m_topBar->setFixedWidth(qMax(320, columnWidth - 2 * kColumnSideClearance));

	// 档位与侧栏相同（两块面板挨着，不一致会看出来）
	const QMargins topBarShadowPad = CardShadow::padding(kTopBarShadow);
	auto* topBarShadow = new ShadowPanel(QStringLiteral("shadow"), kTopBarShadow, rightColumn);
	topBarShadow->setRadius(kTopBarShadow.radius);	// 与 #topBar 的 QSS 圆角一致，阴影形状才对得上
	topBarShadow->setCard(m_topBar);
	// 上留白置 0 让卡片上沿与侧栏齐平；上留白为 0 意味着卡片上方的阴影不会被画出来
	topBarShadow->setPadding(QMargins(topBarShadowPad.left(), 0, topBarShadowPad.right(),
		kTopBarShadowGap));

	auto* topBarRow = new QWidget(rightColumn);
	auto* topBarRowLayout = new QHBoxLayout(topBarRow);
	// 边距扣掉外壳的阴影留白，卡片左右边缘仍在原来的 16px 上
	topBarRowLayout->setContentsMargins(
		kColumnSideClearance - topBarShadowPad.left(), 0,
		kColumnSideClearance - topBarShadowPad.right(), 0);
	topBarRowLayout->setSpacing(0);
	topBarRowLayout->addWidget(topBarShadow);
	topBarRowLayout->addStretch();

	rightColumnLayout->addWidget(topBarRow);
	rightColumnLayout->addWidget(rightPanel, 1);

	auto* bodyLayout = new QHBoxLayout;
	bodyLayout->setSpacing(0);

	bodyLayout->addWidget(sidebarShadow);
	bodyLayout->addWidget(rightColumn);
	bodyLayout->setSpacing(kPanelGap);

	auto* content = new QWidget(central);
	content->setFixedWidth(contentWidth);
	auto* contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(0, 0, 0, 0);
	contentLayout->addLayout(bodyLayout, 1);

	// 外壳自带下留白，两者之和才是卡片离窗口底边的距离
	constexpr int kContentBottomInset = 0;
	auto* bodyRow = new QVBoxLayout;
	bodyRow->setContentsMargins(kContentSideInset, 0, kContentSideInset, kContentBottomInset);
	bodyRow->setSpacing(0);
	bodyRow->addWidget(content, 0, Qt::AlignHCenter);
	layout->addLayout(bodyRow, 1);

	setCentralWidget(central);

	setMinimumWidth(qMin(contentWidth + 2 * kWindowMargin, available.width()));
	resize(qMin(windowWidth, available.width()), windowHeight);

	m_initOverlay = new QWidget(this);
	m_initOverlay->setObjectName(QStringLiteral("initOverlay"));
	m_initOverlay->setAttribute(Qt::WA_StyledBackground, true);
	auto* overlayLayout = new QVBoxLayout(m_initOverlay);

	auto* initCard = new QWidget(m_initOverlay);
	initCard->setObjectName(QStringLiteral("initCard"));
	initCard->setAttribute(Qt::WA_StyledBackground, true);
	initCard->setFixedSize(400, 240);

	auto* cardLayout = new QVBoxLayout(initCard);
	cardLayout->setContentsMargins(24, 20, 24, 20);
	cardLayout->setSpacing(8);

	auto* cardLogo = new QLabel(initCard);
	cardLogo->setAlignment(Qt::AlignCenter);
	cardLogo->setAttribute(Qt::WA_TranslucentBackground);
	const QString cardLogoResource = ThemeManager::instance().isDark()
		? QStringLiteral(":/DSHHub/DSH-Hub-Logo-Tiny-Dark@2x.png")
		: QStringLiteral(":/DSHHub/DSH-Hub-Logo-Tiny@2x.png");
	QPixmap cardLogoPix(cardLogoResource);
	if (!cardLogoPix.isNull()) {
		cardLogoPix.setDevicePixelRatio(2.0);
		cardLogo->setPixmap(cardLogoPix);
	}
	else {
		cardLogo->setText(QStringLiteral("DSH Hub"));
	}
	cardLayout->addWidget(cardLogo);

	auto* rowLayout = new QHBoxLayout;
	rowLayout->setSpacing(16);

	auto* spinner = new SpinnerWidget(initCard);
	spinner->setFixedSize(40, 40);
	spinner->start();

	rowLayout->addStretch(1);
	rowLayout->addWidget(spinner, 0, Qt::AlignVCenter);

	m_initLabel = new QLabel(qtTrId("app_initializing"), initCard);
	m_initLabel->setObjectName(QStringLiteral("initLabel"));
	m_initLabel->setAlignment(Qt::AlignCenter);
	rowLayout->addWidget(m_initLabel, 0, Qt::AlignVCenter);
	rowLayout->addStretch(1);

	cardLayout->addLayout(rowLayout);
	cardLayout->addStretch(1);

	// 初始化卡片是这一屏最高的一层，用 lv3 档阴影
	auto* initPanel = new ShadowPanel(QStringLiteral("shadowFloat"),
		CardShadow::level3(), m_initOverlay);
	initPanel->setRadius(20); // 与 #initCard 的 QSS 圆角一致
	initPanel->setCard(initCard);

	overlayLayout->addWidget(initPanel, 0, Qt::AlignCenter);

	m_initOverlay->setGeometry(rect());
	m_initOverlay->raise();
	m_initOverlay->show();

	// 若搭 UI 期间服务端已完成初始化（复用已有 server、启动即失败等），这里立即收掉遮罩，
	// 避免"初始化已完成但遮罩永远挂着"
	if (m_initializationComplete) {
		m_initOverlay->hide();
		m_initOverlay->deleteLater();
		m_initOverlay = nullptr;
		m_initLabel = nullptr;
	}
}
