// ------------------------------------------------------------------
// Main.cpp
// ------------------------------------------------------------------
// DSHHub 主窗口 UI 搭建（原构造函数主体，见 DSHHub::buildUi）。
// 外观由 Theme 启动时从 styles/*.qss 统一安装，控件只负责提供 objectName。
// ------------------------------------------------------------------

#include "DSHHub.h"

#include "ChatInputWidget.h"
#include "LoadMoreButton.h"
#include "ShadowPanel.h"
#include "Sidebar.h"
#include "SpinnerWidget.h"
#include "ThemeManager.h"
#include "TitleBar.h"
#include "TopBar.h"
#include "WindowFrame.h"
#include "MessageQuery.h"

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
	// 左侧会话列表宽度
	constexpr int kSidebarWidth = 240;
	// 会话列左右留白：消息与输入卡片共用同一份，两者左右边缘对齐
	constexpr int kColumnSideClearance = 16;
	// 窗口四周留白（不贴屏幕边缘）
	constexpr int kWindowMargin = 24;
	// 窗口宽度上限：大屏上到这里为止，不再无限拉长输入区
	constexpr int kPreferredWindowWidth = 1440;
	// 默认窗口高度上限
	constexpr int kPreferredWindowHeight = 880;
	// 会话列宽下限（保证底部控制行放得下 chip + 发送键）
	constexpr int kMinConversationColumnWidth = 320;
	// 自绘标题栏高度：窗口高度里要把它算进去，内容区才不会比改造前矮一截
	constexpr int kTitleBarHeight = TitleBar::kHeight;
	// 内容块四周的留白（下面 bodyRow 用）。
	// 它是"窗口内缘 -> 内容块"的距离；侧栏卡片离窗口左缘的距离 =
	// 这个值 + 侧栏阴影外壳的左留白，两者一起决定观感。
	constexpr int kContentSideInset = 9;
	// 侧栏阴影规格：比输入卡片那一档收窄（原版那套 12px 模糊的阴影如果照搬到
	// 贴着窗口边框的常驻侧栏上，留白会明显挤窄会话列）。
	// 数值单独写在这里，是为了能只调侧栏而不牵动消息气泡。
	const CardShadow::Spec kSidebarShadow = { 8, 2, 12 };
	// 会话列与侧栏阴影外壳之间的空隙。阴影本身已经带来一圈留白
	// （见 CardShadow::padding），这里只补 2px 让阴影右侧有落脚处。
	constexpr int kPanelGap = 2;
}

void DSHHub::buildUi()
{
	// 创建界面（外观由 Theme 启动时从 styles/*.qss 统一安装，控件只负责提供 objectName）
	//
	// 尺寸策略：先按“可用桌面 - 四周留白”定窗口尺寸，再反推内容列宽。
	// 高 DPI 小屏（例如 2560x1600 @200% => 逻辑仅 1280x800）上，
	// 任何写死的宽/高都会让窗口超出屏幕，右边或下面被裁掉 —— 底部的
	// 输入卡片首当其冲，所以这里一律以可用桌面为上限。
	// 消息内容与输入卡片左右各留 16px，两者同宽对齐。
	const QRect available = screen()
		? screen()->availableGeometry()
		: QRect(0, 0, kPreferredWindowWidth, kPreferredWindowHeight);

	const int windowWidth = qMin(kPreferredWindowWidth, available.width() - 2 * kWindowMargin);
	const int windowHeight = qBound(480, kPreferredWindowHeight + kTitleBarHeight,
		available.height() - 2 * kWindowMargin);

	// 窗口内宽：这是"留给内容的总宽"，改造前会话列就是从这里扣掉侧栏本体得到的。
	const int windowInner = windowWidth - 2 * kWindowMargin;

	// 会话列宽**只从窗口内宽里扣侧栏本体**，不扣阴影留白 ——
	// 阴影留白属于"装饰"，让窗口自己的留白去承担；一旦从会话列里扣，
	// 输入卡片与消息列会整体变窄、并且离窗口左右边框更远（一眼就看得出来）。
	const int columnWidth = qMax(kMinConversationColumnWidth, windowInner - kSidebarWidth);

	// 侧栏外面套了阴影外壳（见 ShadowPanel），外壳比侧栏本体宽一圈，
	// 所以内容块 = 侧栏外壳 + 空隙 + 会话列，比 windowInner 宽出阴影那部分。
	// 多出来的宽度从窗口留白里出（下面 bodyRow 的边距只有十几像素，
	// 而窗口四周的 kWindowMargin 是 24px，够放）。
	const QMargins sidebarShadowPad = CardShadow::padding(kSidebarShadow);
	const int sidebarOuterWidth = kSidebarWidth + sidebarShadowPad.left() + sidebarShadowPad.right();

	// 但内容块本身不能超出 bodyRow 给得出的宽度，否则会被裁掉。
	// bodyRow 左右各留 kContentSideInset，即窗口内可用宽度 = windowWidth - 2*inset。
	const int contentWidth = qMin(sidebarOuterWidth + kPanelGap + columnWidth,
		windowWidth - 2 * kContentSideInset);

	auto* central = new QWidget(this);
	central->setObjectName(QStringLiteral("dshhubCentral"));
	central->setAttribute(Qt::WA_StyledBackground, true);
	auto* layout = new QVBoxLayout(central);
	// 客户区铺满整个窗口：圆角与 1px 描边都由 #dshhubCentral 自己画
	// （见 main-window.qss + common/WindowFrame），所以四周不留边距，
	// 自绘标题栏才能贴着窗口上沿、右沿放窗口按钮。
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	// 自绘标题栏（替代系统标题栏）：控件只管画与发意图，动作在 common/WindowFrame 落地。
	// 拖动、双击最大化、贴边吸附、右键系统菜单由系统按 HTCAPTION 处理。
	m_titleBar = new TitleBar(central);
	layout->addWidget(m_titleBar);

	connect(m_titleBar, &TitleBar::minimizeRequested,
		this, [this]() { WindowFrame::minimize(this); });
	connect(m_titleBar, &TitleBar::maximizeRestoreRequested,
		this, [this]() { WindowFrame::toggleMaximize(this); });
	connect(m_titleBar, &TitleBar::closeRequested,
		this, [this]() { WindowFrame::closeWindow(this); });

	m_scrollArea = new QScrollArea(central);
	m_scrollArea->setObjectName(QStringLiteral("chatScrollArea"));
	m_scrollArea->setFrameShape(QFrame::NoFrame);
	// 同上：滚动条早于 objectName 存在，需重新解析一次 #chatScrollArea 的滚动条规则
	Theme::repolishScrollArea(m_scrollArea);

	auto* scrollContent = new QWidget;
	scrollContent->setObjectName(QStringLiteral("chatScrollContent"));
	scrollContent->setAttribute(Qt::WA_StyledBackground, true);
	auto* scrollLayout = new QVBoxLayout(scrollContent);
	// 左侧 4px：消息外面套了阴影外壳（见 MessageQuery 的 kMessageShadow），
	// 外壳自带 6px 左留白，4 + 6 = 原来那 10px，消息的左边缘位置不变。
	scrollLayout->setContentsMargins(4, 0, 0, 0);
	scrollLayout->setAlignment(Qt::AlignTop);

	m_messagesLayout = scrollLayout;
	scrollContent->setLayout(scrollLayout);

	// 顶部“加载更多”按钮，默认隐藏
	m_loadMoreButton = new LoadMoreButton(scrollContent);
	m_loadMoreButton->hide();
	scrollLayout->insertWidget(0, m_loadMoreButton, 0, Qt::AlignHCenter);

	m_scrollArea->setWidget(scrollContent);
	m_scrollArea->setWidgetResizable(true);
	m_scrollArea->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	// 右侧面板与主窗口同色
	auto* rightPanel = new QWidget(central);
	rightPanel->setObjectName(QStringLiteral("chatPanel"));
	rightPanel->setFixedWidth(columnWidth);
	rightPanel->setAttribute(Qt::WA_StyledBackground, true);

	m_chatInput = new ChatInputWidget(rightPanel);

	// 底部“没有更多了”提示
	m_toastLabel = new QLabel(qtTrId("common_no_more_items"), this);
	m_toastLabel->setObjectName(QStringLiteral("toastLabel"));
	m_toastLabel->setAlignment(Qt::AlignCenter);
	m_toastLabel->hide();

	auto* panelLayout = new QVBoxLayout(rightPanel);
	panelLayout->setContentsMargins(0, 0, 0, 0);
	panelLayout->setSpacing(0);

	// 左侧灰色会话列表。
	// 外面套一层阴影外壳给它"悬浮"的观感：外壳自己透明，只在四周留白里画阴影，
	// #sidebar 的 QSS 背景/圆角/会话按钮样式一条都不用改（见 ShadowPanel.h）。
	m_sidebar = new Sidebar(central);
	m_sidebar->setFixedWidth(kSidebarWidth);
	auto* sidebarShadow = new ShadowPanel(QStringLiteral("shadow"), kSidebarShadow, central);
	sidebarShadow->setRadius(12); // 与 #sidebar 的 QSS 圆角一致，阴影形状才对得上
	sidebarShadow->setCard(m_sidebar);

	// 消息区左右各留 16px
	auto* scrollRow = new QHBoxLayout;
	scrollRow->setContentsMargins(kColumnSideClearance, 0, kColumnSideClearance, 0);
	scrollRow->setSpacing(0);
	scrollRow->addWidget(m_scrollArea, 1);

	panelLayout->addLayout(scrollRow, 1);

	// 输入卡片与消息内容左右对齐（同一份 16px 留白）。
	//
	// 卡片外面套了阴影外壳（ChatInputWidget::shadowSpec()），外壳本身要吃掉
	// 四周一圈留白，所以这里的边距 = 原边距 - 外壳留白：
	// 卡片左右边缘因此仍在原来的位置上，阴影落在被让出来的那圈里。
	//
	// 纵向：外壳的上下留白是新的高度来源，多出来的部分由消息区让出
	// （panelLayout 里消息区是 stretch=1，输入区按 sizeHint 贴底）。
	const QMargins inputShadowPad = CardShadow::padding(ChatInputWidget::shadowSpec());
	const int kInputTopClearance = 0;
	// 底部留白 = 侧栏阴影外壳的下留白。
	// 这样"卡片下方那行小灰字"的底边正好与侧栏卡片的底边齐平
	// （侧栏卡片底边 = 内容区底边 - 它的外壳下留白；小灰字底边 = 内容区底边 -
	//   这里的下留白。两者相等就平行了）。
	// 从 kSidebarShadow 推出来而不是写死数字：改侧栏阴影档位时对齐不会失效。
	const int kInputBottomClearance = CardShadow::padding(kSidebarShadow).bottom();

	auto* inputLayout = new QHBoxLayout;
	inputLayout->addWidget(m_chatInput, 1);

	inputLayout->setContentsMargins(
		qMax(0, kColumnSideClearance - inputShadowPad.left()), kInputTopClearance,
		qMax(0, kColumnSideClearance - inputShadowPad.right()), kInputBottomClearance);
	inputLayout->setSpacing(0);
	panelLayout->addLayout(inputLayout);

	// 对话顶部栏：宽度与消息内容一致，放在右侧对话栏上方
	auto* rightColumn = new QWidget(central);
	rightColumn->setFixedWidth(columnWidth);
	auto* rightColumnLayout = new QVBoxLayout(rightColumn);
	rightColumnLayout->setContentsMargins(0, 0, 0, 0);
	rightColumnLayout->setSpacing(8);

	m_topBar = new TopBar(rightColumn);
	// 顶栏宽度跟随会话列（与消息内容、输入卡片保持一致）
	m_topBar->setFixedWidth(qMax(320, columnWidth - 2 * kColumnSideClearance));
	auto* topBarRow = new QWidget(rightColumn);
	auto* topBarRowLayout = new QHBoxLayout(topBarRow);
	topBarRowLayout->setContentsMargins(kColumnSideClearance, 0, kColumnSideClearance, 0);
	topBarRowLayout->setSpacing(0);
	topBarRowLayout->addWidget(m_topBar);
	topBarRowLayout->addStretch();

	rightColumnLayout->addWidget(topBarRow);
	rightColumnLayout->addWidget(rightPanel, 1);

	auto* bodyLayout = new QHBoxLayout;
	bodyLayout->setSpacing(0);

	bodyLayout->addWidget(sidebarShadow);
	bodyLayout->addWidget(rightColumn);
	bodyLayout->setSpacing(kPanelGap);

	// 内容容器：整体居中（标题栏之下，左右/下侧留白由这一层给）
	auto* content = new QWidget(central);
	content->setFixedWidth(contentWidth);
	auto* contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(0, 0, 0, 0);
	contentLayout->addLayout(bodyLayout, 1);

	// 底部留白：输入卡片外面套了阴影外壳，外壳自带下留白（CardShadow::padding 的
	// bottom），所以这里归零 —— 两者的和才是卡片离窗口底边的距离，
	// 否则"留白 + 留白"会把输入卡片顶得老高。
	constexpr int kContentBottomInset = 0;
	auto* bodyRow = new QVBoxLayout;
	bodyRow->setContentsMargins(kContentSideInset, 0, kContentSideInset, kContentBottomInset);
	bodyRow->setSpacing(0);
	bodyRow->addWidget(content, 0, Qt::AlignHCenter);
	layout->addLayout(bodyRow, 1);
	// 输入框是 rightPanel 的子控件，随右侧面板一起布局，无需加入主布局

	setCentralWidget(central);

	setMinimumWidth(qMin(contentWidth + 2 * kWindowMargin, available.width()));
	resize(qMin(windowWidth, available.width()), windowHeight);

	// 初始化灰色蒙版 + 居中标签
	m_initOverlay = new QWidget(this);
	m_initOverlay->setObjectName(QStringLiteral("initOverlay"));
	m_initOverlay->setAttribute(Qt::WA_StyledBackground, true);
	auto* overlayLayout = new QVBoxLayout(m_initOverlay);

	// 现代化横版卡片：宽高比约 5:3
	auto* initCard = new QWidget(m_initOverlay);
	initCard->setObjectName(QStringLiteral("initCard"));
	initCard->setAttribute(Qt::WA_StyledBackground, true);
	initCard->setFixedSize(400, 240);

	auto* cardLayout = new QVBoxLayout(initCard);
	cardLayout->setContentsMargins(24, 20, 24, 20);
	cardLayout->setSpacing(8);

	// 卡片内顶部显示 Logo
	auto* cardLogo = new QLabel(initCard);
	cardLogo->setAlignment(Qt::AlignCenter);
	cardLogo->setAttribute(Qt::WA_TranslucentBackground);
	const QString cardLogoResource = Theme::isDark()
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

	// 下方：旋转条 + 初始化文字
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

	// 初始化卡片浮在蒙版上，是这一屏最高的一层：用 lv3 档阴影。
	// 卡片 400x240 的尺寸不变，多出来的只是外壳四周的阴影留白。
	auto* initPanel = new ShadowPanel(QStringLiteral("shadowFloat"),
		CardShadow::level3(), m_initOverlay);
	initPanel->setRadius(20); // 与 #initCard 的 QSS 圆角一致
	initPanel->setCard(initCard);

	overlayLayout->addWidget(initPanel, 0, Qt::AlignCenter);

	m_initOverlay->setGeometry(rect());
	m_initOverlay->raise();
	m_initOverlay->show();

	// 服务端 spawn 已提前到构造最前：若在搭 UI 期间服务端就绪/报错并已
	// 完成初始化（复用已有 server、启动即失败等），这里立即收掉遮罩，
	// 避免"初始化已完成但遮罩永远挂着"。
	if (m_initializationComplete) {
		m_initOverlay->hide();
		m_initOverlay->deleteLater();
		m_initOverlay = nullptr;
		m_initLabel = nullptr;
	}
}