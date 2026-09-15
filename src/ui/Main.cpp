// ------------------------------------------------------------------
// Main.cpp
// ------------------------------------------------------------------
// DSHHub 主窗口 UI 搭建（原构造函数主体，见 DSHHub::buildUi）。
// 外观由 Theme 启动时从 styles/*.qss 统一安装，控件只负责提供 objectName。
// ------------------------------------------------------------------

#include "DSHHub.h"

#include "ChatInputWidget.h"
#include "LoadMoreButton.h"
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

	// 内容居中于窗口内，同样各留 kWindowMargin
	const int contentWidth = qMax(kSidebarWidth + kMinConversationColumnWidth,
		windowWidth - 2 * kWindowMargin);
	const int columnWidth = qMax(kMinConversationColumnWidth, contentWidth - kSidebarWidth);

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
	scrollLayout->setContentsMargins(10, 0, 0, 0);
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
	m_toastLabel = new QLabel(tr("啊哦，没有更多了"), this);
	m_toastLabel->setObjectName(QStringLiteral("toastLabel"));
	m_toastLabel->setAlignment(Qt::AlignCenter);
	m_toastLabel->hide();

	auto* panelLayout = new QVBoxLayout(rightPanel);
	panelLayout->setContentsMargins(0, 0, 0, 0);
	panelLayout->setSpacing(0);

	// 左侧灰色会话列表
	m_sidebar = new Sidebar(central);
	m_sidebar->setFixedWidth(kSidebarWidth);

	// 消息区左右各留 16px
	auto* scrollRow = new QHBoxLayout;
	scrollRow->setContentsMargins(kColumnSideClearance, 0, kColumnSideClearance, 0);
	scrollRow->setSpacing(0);
	scrollRow->addWidget(m_scrollArea, 1);

	panelLayout->addLayout(scrollRow, 1);

	// 输入卡片与消息内容左右对齐（同一份 16px 留白）。
	//
	// 上下留白里那 14px 是给卡片下方"会话统计小灰字"的
	// （行高固定 = ChatInputWidget 的 SessionStatsLine::kHeight）：
	//   改造前： 8 + 卡片 + 8
	//   现在：   2 + 卡片 + 14(小灰字) + 0
	// 总高不变（消息区大小不受影响）。
	//
	// 输入区是"贴着面板底边"摆的，所以底部留白决定整块的高度位置：
	// 底部 2 -> 0 让"卡片 + 小灰字"整块下沉 2px，窗口底边留白
	// （下面 bodyRow 的 kContentBottomInset 9 -> 7）再下沉 2px，合计 4px。
	constexpr int kInputTopClearance = 2;
	constexpr int kInputBottomClearance = 0;
	static_assert(kInputTopClearance + SessionStatsLine::kHeight + kInputBottomClearance == 8 + 8,
		"输入区总高必须与改造前一致：改 SessionStatsLine::kHeight 时要同步这三个数");

	auto* inputLayout = new QHBoxLayout;
	inputLayout->addWidget(m_chatInput, 1);

	inputLayout->setContentsMargins(kColumnSideClearance, kInputTopClearance,
		kColumnSideClearance, kInputBottomClearance);
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

	bodyLayout->addWidget(m_sidebar);
	bodyLayout->addWidget(rightColumn);

	// 内容容器：整体居中（标题栏之下，左右/下侧留白由这一层给）
	auto* content = new QWidget(central);
	content->setFixedWidth(contentWidth);
	auto* contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(0, 0, 0, 0);
	contentLayout->addLayout(bodyLayout, 1);

	// 底部留白 7（原 9）：从这里再挪 2px 出去，配合输入区底部留白 2 -> 0，
	// 让"输入卡片 + 小灰字"整块相对窗口底边下沉 4px
	constexpr int kContentBottomInset = 7;
	auto* bodyRow = new QVBoxLayout;
	bodyRow->setContentsMargins(9, 0, 9, kContentBottomInset);
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

	m_initLabel = new QLabel(tr("DSH Hub 正在初始化..."), initCard);
	m_initLabel->setObjectName(QStringLiteral("initLabel"));
	m_initLabel->setAlignment(Qt::AlignCenter);
	rowLayout->addWidget(m_initLabel, 0, Qt::AlignVCenter);
	rowLayout->addStretch(1);

	cardLayout->addLayout(rowLayout);
	cardLayout->addStretch(1);

	overlayLayout->addWidget(initCard, 0, Qt::AlignCenter);

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