#include "DSHHub.h"
#include "ServerManager.h"
#include "ThemeManager.h"
#include "ChatInputWidget.h"
#include "Sidebar.h"

#include "DshApiClient.h"
#include "SessionPrefetcher.h"
#include "CodeHighlighter.h"
#include "SpinnerWidget.h"
#include "TopBar.h"
#include "Settings.h"
#include "PluginsManager.h"
#include "DshNamedPipeBridge.h"
#include "DllCaller.h"
#include "ExtensionManagerPopup.h"
#include "InteractionHandler.h"

#include "DshEventParser.h"
#include "AgentMessageUnit.h"
#include "MessageQuery.h"
#include "LoadMoreButton.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMoveEvent>

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFileDialog>
#include <QFrame>

#include <QHBoxLayout>

#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>

#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>

#include <QScrollBar>
#include <QScrollArea>

#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>

#include <QTimer>
#include <QUrl>
#include <QTcpSocket>
#include <QLocalSocket>
#include <QVBoxLayout>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

#include <functional>

namespace
{
	// 简单 std::function 任务，投递到 QThreadPool 执行（QtCore，无需 QtConcurrent）
	class FunctorTask : public QRunnable
	{
	public:
		explicit FunctorTask(std::function<void()> fn)
			: m_fn(std::move(fn))
		{
		}

		void run() override
		{
			if (m_fn)
				m_fn();
		}

	private:
		std::function<void()> m_fn;
	};
} // namespace

DSHHub::DSHHub(QWidget* parent, const QUrl& initialBaseUrl, QProcess* initialServerProcess)
	: QMainWindow(parent)
	, m_api(new DshApiClient(this))
{
	qInfo().noquote() << QStringLiteral("[DSH Hub] constructor started");

	// DLL/COM 工具调用线程池：请求在 Worker 上执行（GUI 不阻塞）；
	// DllCaller 内部按 DLL 串行、跨 DLL 并行。
	m_toolPool = new QThreadPool(this);
	m_toolPool->setMaxThreadCount(4);

	// 样式表按窗口安装（替代全局 qApp 表）：本窗口与后续加入的子控件统一应用当前主题
	Theme::applyToWindow(this);
	setWindowTitle(QStringLiteral("DSH Hub"));
	setAttribute(Qt::WA_DeleteOnClose);

	QSettings settings;
	m_defaultAgentPreset = settings.value(QStringLiteral("agent/defaultPreset")).toString();

	// 启动命名管道桥接服务，供 Node/DSh server 调用 DLL 工具
	m_pipeBridge = new DshNamedPipeBridge(this);
	connect(m_pipeBridge, &DshNamedPipeBridge::requestReceived,
		this, &DSHHub::handlePipeRequest);
	if (!m_pipeBridge->start()) {
		qWarning() << QStringLiteral("[DSH Pipe] failed to start:") << m_pipeBridge->errorString();
	}

	// 初始化 JSON5 DLL 调用器
	m_dllCaller = new DllCaller;
	const QString appDir = QCoreApplication::applicationDirPath();
	const QString serverProfilePath = appDir + QStringLiteral("/resources/server/harness/profiles/web");
	const QString extensionsRoot = serverProfilePath + QStringLiteral("/extensions");

	QString descriptorPath = qEnvironmentVariable("DSH_DLL_JSON5", QString());
	QString dllPath = qEnvironmentVariable("DSH_DLL", QString());

	// 显式指定了完整一对（调试用）：只加载这一份，跳过自动发现
	if (!descriptorPath.isEmpty() && !dllPath.isEmpty()) {
		if (QFile::exists(descriptorPath) && QFile::exists(dllPath)) {
			if (!m_dllCaller->loadDescriptor(descriptorPath)) {
				qWarning() << "[DllCaller] descriptor error:" << m_dllCaller->errorString();
			}
			else if (!m_dllCaller->loadLibrary(dllPath)) {
				qWarning() << "[DllCaller] library error:" << m_dllCaller->errorString();
			}
		}
	}
	else {
		// env 只给了描述符（调试）：目录下的 main.dll 作为库一并加载
		if (!descriptorPath.isEmpty() && QFile::exists(descriptorPath)) {
			if (dllPath.isEmpty())
				dllPath = QFileInfo(descriptorPath).absolutePath() + QStringLiteral("/main.dll");
			if (QFile::exists(dllPath)) {
				if (!m_dllCaller->loadDescriptor(descriptorPath)) {
					qWarning() << "[DllCaller] descriptor error:" << m_dllCaller->errorString();
				}
				else if (!m_dllCaller->loadLibrary(dllPath)) {
					qWarning() << "[DllCaller] library error:" << m_dllCaller->errorString();
				}
			}
		}

		// 自动发现：逐个加载全部已安装扩展（原先只加载扫描到的第一个；
		// 多扩展并存后改为全部加载，DllCaller 内部按扩展名去重）
		const QStringList scanRoots = {
			extensionsRoot,
			serverProfilePath + QStringLiteral("/node_modules")
		};
		for (const QString& scanRoot : scanRoots) {
			const QDir root(scanRoot);
			if (!root.exists())
				continue;
			const QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
			for (const QFileInfo& entry : entries) {
				const QString extDir = entry.absoluteFilePath();
				const QString candidateJson = extDir + QStringLiteral("/regulation.json5");
				const QString candidateDll = extDir + QStringLiteral("/main.dll");
				if (!QFile::exists(candidateJson) || !QFile::exists(candidateDll))
					continue;
				if (!m_dllCaller->loadDescriptor(candidateJson)) {
					qWarning() << "[DllCaller] auto-load descriptor failed:" << candidateJson
						<< m_dllCaller->errorString();
					continue;
				}
				if (!m_dllCaller->loadLibrary(candidateDll)) {
					qWarning() << "[DllCaller] auto-load library failed:" << candidateDll
						<< m_dllCaller->errorString();
				}
			}
		}
	}

	// 创建界面（外观由 Theme 启动时从 styles/*.qss 统一安装，控件只负责提供 objectName）
	auto* central = new QWidget(this);
	central->setObjectName(QStringLiteral("dshhubCentral"));
	central->setAttribute(Qt::WA_StyledBackground, true);
	auto* layout = new QVBoxLayout(central);

	m_scrollArea = new QScrollArea(central);
	m_scrollArea->setObjectName(QStringLiteral("chatScrollArea"));
	m_scrollArea->setFixedWidth(900);
	m_scrollArea->setFrameShape(QFrame::NoFrame);

	auto* scrollContent = new QWidget;
	scrollContent->setObjectName(QStringLiteral("chatScrollContent"));
	scrollContent->setAttribute(Qt::WA_StyledBackground, true);
	auto* scrollLayout = new QVBoxLayout(scrollContent);
	scrollLayout->setContentsMargins(10, 0, 0, 0);
	scrollLayout->setAlignment(Qt::AlignTop);

	m_messagesLayout = scrollLayout;
	m_messages = new MessageQuery;
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
	rightPanel->setFixedWidth(900);
	rightPanel->setAttribute(Qt::WA_StyledBackground, true);

	m_chatInput = new ChatInputWidget(rightPanel);

	// 底部“没有更多了”提示
	m_toastLabel = new QLabel(QStringLiteral("啊哦，没有更多了"), this);
	m_toastLabel->setObjectName(QStringLiteral("toastLabel"));
	m_toastLabel->setAlignment(Qt::AlignCenter);
	m_toastLabel->hide();

	auto* panelLayout = new QVBoxLayout(rightPanel);
	panelLayout->setContentsMargins(0, 0, 0, 0);
	panelLayout->setSpacing(0);

	// 左侧灰色会话列表
	m_sidebar = new Sidebar(central);
	m_sidebar->setFixedWidth(240);

	panelLayout->addWidget(m_scrollArea);

	auto* inputLayout = new QHBoxLayout;
	inputLayout->addWidget(m_chatInput, 1);

	inputLayout->setContentsMargins(10, 8, 10, 8);
	inputLayout->setSpacing(8);
	panelLayout->addLayout(inputLayout);

	// 对话顶部栏：宽度与对话栏一致，放在右侧对话栏上方
	auto* rightColumn = new QWidget(central);
	rightColumn->setFixedWidth(900);
	auto* rightColumnLayout = new QVBoxLayout(rightColumn);
	rightColumnLayout->setContentsMargins(0, 0, 0, 0);
	rightColumnLayout->setSpacing(8);

	m_topBar = new TopBar(rightColumn);
	auto* topBarRow = new QWidget(rightColumn);
	auto* topBarRowLayout = new QHBoxLayout(topBarRow);
	topBarRowLayout->setContentsMargins(10, 0, 0, 0);
	topBarRowLayout->setSpacing(0);
	topBarRowLayout->addWidget(m_topBar);
	topBarRowLayout->addStretch();

	rightColumnLayout->addWidget(topBarRow);
	rightColumnLayout->addWidget(rightPanel, 1);

	auto* bodyLayout = new QHBoxLayout;
	bodyLayout->setSpacing(0);

	bodyLayout->addWidget(m_sidebar);
	bodyLayout->addWidget(rightColumn);

	// 内容容器：整体居中
	auto* content = new QWidget(central);
	content->setFixedWidth(1140);
	auto* contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(0, 0, 0, 0);
	contentLayout->addLayout(bodyLayout, 1);

	layout->addWidget(content, 0, Qt::AlignHCenter);
	// 输入框是 rightPanel 的子控件，随右侧面板一起布局，无需加入主布局

	setCentralWidget(central);

	setMinimumWidth(1160);
	resize(1000, 700);

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

	m_initLabel = new QLabel(QStringLiteral("DSH Hub 正在初始化..."), initCard);
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

	// 初始化时从 Qt 资源中加载代码高亮规则
	{
		if (!CodeHighlighter::instance().loadFromFile(QStringLiteral(":/DSHHub/highlight_rules.json"))) {
			qWarning().noquote() << QStringLiteral("[DSH Hub] 未找到内置资源 highlight_rules.json，代码高亮不可用");
		}
	}

	// 流式渲染节流：避免每个 chunk 都全量重渲染导致卡顿
	m_streamTimer = new QTimer(this);
	m_streamTimer->setInterval(50);
	m_streamTimer->setSingleShot(true);
	connect(m_streamTimer, &QTimer::timeout, this, [this]() {
		AgentMessageUnit* target = m_messages ? m_messages->lastAgentUnitIfLast() : nullptr;
		if (!target)
			return;
		target->flushStream();
		if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
			QScrollBar* bar = m_scrollArea->verticalScrollBar();
			if (bar->value() >= bar->maximum() - 80)
				scrollToBottomNow();
		}
		});

	// 信号槽
	connect(m_chatInput, &ChatInputWidget::sendRequested, this, &DSHHub::onSendClicked);
	connect(m_chatInput, &ChatInputWidget::stopRequested, this, &DSHHub::onStopRequested);

	connect(m_sidebar, &Sidebar::newWorkspaceRequested,
		this, &DSHHub::onNewWorkspaceClicked);
	connect(m_sidebar, &Sidebar::createSessionInWorkspaceRequested,
		this, &DSHHub::onCreateSessionInWorkspace);
	connect(m_sidebar, &Sidebar::sessionSelected,
		this, &DSHHub::onSessionSelected);
	connect(m_sidebar, &Sidebar::deleteSessionRequested,
		this, &DSHHub::onDeleteSessionRequested);
	connect(m_sidebar, &Sidebar::clearRequested,
		this, &DSHHub::onClearConversationClicked);

	connect(m_api, &DshApiClient::connected, this, &DSHHub::handleConnected);
	connect(m_api, &DshApiClient::muxFrameReceived, this, &DSHHub::handleMuxFrame);
	connect(m_api, &DshApiClient::transportError, this, &DSHHub::handleTransportError);
	m_prefetcher = new SessionPrefetcher(this);
	connect(m_prefetcher, &SessionPrefetcher::historyFetched,
		this, &DSHHub::onHistoryPrefetched);
	// 侧边栏“设置”入口：Settings 是常驻“设置系统”，窗口开关由它自己管理，
	// 这里只做一次接线；实际创建放在构造函数尾部（ServerManager start 之后，
	// 那时 dshHome 才可用）。
	connect(m_sidebar, &Sidebar::settingsRequested,
		this, [this]() { if (m_settings) m_settings->openSettings(); });
	connect(m_sidebar, &Sidebar::pluginsRequested,
		this, [this]() { if (m_pluginsManager) m_pluginsManager->openPlugins(); });
	connect(m_sidebar, &Sidebar::themeToggleRequested,
		this, &DSHHub::toggleTheme);
	connect(m_sidebar, &Sidebar::extensionsRequested,
		this, &DSHHub::openExtensions);
	connect(m_sidebar, &Sidebar::initialSessionReady,
		this, &DSHHub::onInitialSessionReady);
	connect(m_sidebar, &Sidebar::sessionCreated,
		this, &DSHHub::onSessionCreated);
	connect(m_sidebar, &Sidebar::noSessionAvailable,
		this, &DSHHub::onNoSessionAvailable);
	connect(m_sidebar, &Sidebar::sessionListError,
		this, &DSHHub::onSessionListError);
	connect(m_sidebar, &Sidebar::sessionCreateError,
		this, &DSHHub::onSessionCreateError);

	m_historyLoader = new HistoryLoader(m_api, m_messages, m_messagesLayout, &m_history, m_scrollArea, this);
	connect(m_historyLoader, &HistoryLoader::loadMoreButtonVisibleChanged,
		this, &DSHHub::onHistoryLoadMoreButtonVisibleChanged);
	connect(m_historyLoader, &HistoryLoader::historyError,
		this, &DSHHub::onHistoryError);
	connect(m_historyLoader, &HistoryLoader::incrementalBuildReady,
		this, &DSHHub::onIncrementalBuildReady);
	connect(m_historyLoader, &HistoryLoader::noMoreHistory, this, [this]() {
		showNoMoreToast();
		if (m_loadMoreButton)
			m_loadMoreButton->hide();
		});
	connect(m_loadMoreButton, &QPushButton::clicked, m_historyLoader, &HistoryLoader::loadMore);

	// 服务端统一交给 ServerManager 管理
	m_serverManager = new ServerManager(this);
	connect(m_serverManager, &ServerManager::baseUrlReady, this, [this](const QUrl& url) {
		if (!m_api)
			return;
		m_api->setBaseUrl(url);
		if (m_pluginsManager)
			m_pluginsManager->setBaseUrl(url);
		m_api->openStreams();
		});
	connect(m_serverManager, &ServerManager::errorLine, this, [this](const QString& line) {
		if (m_messages) {
			m_messages->addSystemMessage(
				QStringLiteral("DSH 服务端: %1").arg(line),
				m_messagesLayout);
		}
		// 移除扩展后服务端启动失败时，自动清理 cordis.patch.yml 残留
		if (m_cleanupResidualsAfterServerError && m_extensionPopup) {
			m_cleanupResidualsAfterServerError = false;
			m_extensionPopup->cleanupResiduals();
		}
		if (!isInitializationComplete())
			finishInitialization();
		});
	connect(m_serverManager, &ServerManager::outputLine, this, [](const QString& line) {
		// 只记录服务端里可能与插件市场/扩展/服务本身相关的输出，避免刷爆日志；
		// 设置环境变量 DSH_HUB_SERVER_TRACE=1 可转储服务端全部 stdout。
		const QString lower = line.toLower();
		if (qEnvironmentVariableIsSet("DSH_HUB_SERVER_TRACE")
			|| lower.contains(QStringLiteral("dshmarket"))
			|| lower.contains(QStringLiteral("market"))
			|| lower.contains(QStringLiteral("install"))
			|| lower.contains(QStringLiteral("pnpm"))
			|| lower.contains(QStringLiteral("plugin"))
			|| lower.contains(QStringLiteral("registry"))
			|| lower.contains(QStringLiteral("snapshot"))
			|| lower.contains(QStringLiteral("error"))
			|| lower.contains(QStringLiteral("fail"))) {
			qInfo().noquote() << "[DSH Server]" << line;
		}
		});
	connect(m_serverManager, &ServerManager::finished, this, [this](int exitCode, QProcess::ExitStatus) {
		if (m_serverManager && m_serverManager->isRestarting())
			return;

		if (m_api && !m_api->isConnected()) {
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("DSH 服务端已退出，代码: %1").arg(exitCode),
					m_messagesLayout);
			}
			if (m_cleanupResidualsAfterServerError && m_extensionPopup) {
				m_cleanupResidualsAfterServerError = false;
				m_extensionPopup->cleanupResiduals();
			}
			if (exitCode != 0 && !isInitializationComplete())
				finishInitialization();
		}
		});

	m_serverManager->start(initialBaseUrl, initialServerProcess);

	// ------------------------------------------------------------------
	// 常驻“设置系统”：随主窗口存在，自管设置窗口的开关/遮罩/居中。
	// 放在 start() 之后创建，因为 Settings 构造需要 dshHome
	// （由 ServerManager::start 填充）。这里只做一次业务信号接线：
	// apiKey/Server 保存失败重启服务端、预设变更同步给当前会话。
	// ------------------------------------------------------------------
	m_settings = new Settings(m_serverManager->dshHome(), m_api, this);
	connect(m_settings, &Settings::apiKeyChanged, this, [this]() {
		if (m_serverManager)
			m_serverManager->restart();
		});
	connect(m_settings, &Settings::agentPresetChanged, this, [this](const QString& presetId) {
		m_defaultAgentPreset = presetId;
		if (!m_sessionId.isEmpty() && m_api) {
			QJsonObject payload;
			payload.insert(QStringLiteral("sessionId"), m_sessionId);
			payload.insert(QStringLiteral("agentPreset"), presetId);
			m_api->callMethod(QStringLiteral("agentPreset.select"), payload, {}, {});
		}
		});
	connect(m_settings, &Settings::serverSettingsSaved, this, [this]() {
		if (m_serverManager)
			m_serverManager->restart();
		});

	// ------------------------------------------------------------------
	// 常驻“插件系统”：随主窗口存在，自管插件窗口的开关/遮罩/居中。
	// baseUrl 会在 ServerManager::baseUrlReady 时经 setBaseUrl() 更新；
	// serverRestartRequested（插件内“重启服务”）只在此接线一次。
	// ------------------------------------------------------------------
	m_pluginsManager = new PluginsManager(m_api ? m_api->baseUrl() : QUrl(), this);
	connect(m_pluginsManager, &PluginsManager::serverRestartRequested,
		m_serverManager, &ServerManager::restart);
}

void DSHHub::resizeEvent(QResizeEvent* event)
{
	QMainWindow::resizeEvent(event);

	if (m_initOverlay)
		m_initOverlay->setGeometry(rect());

	if (m_settings)
		m_settings->syncOverlayToHost();

	if (m_pluginsManager)
		m_pluginsManager->syncOverlayToHost();
	if (m_extensionOverlay)
		m_extensionOverlay->setGeometry(rect());

	// 宿主缩放后把打开的弹窗重新居中
	keepOpenPopupsCentered();
}

void DSHHub::moveEvent(QMoveEvent* event)
{
	QMainWindow::moveEvent(event);
	// 弹窗是宿主“拥有的”独立窗口（Windows 上不随宿主拖动），这里手动跟随
	keepOpenPopupsCentered();
}

void DSHHub::keepOpenPopupsCentered()
{
	const QPoint center = geometry().center();
	const auto recenter = [center](QWidget* popup) {
		if (popup && popup->isVisible())
			popup->move(center - popup->rect().center());
	};
	recenter(m_settings);
	recenter(m_pluginsManager);
	recenter(m_extensionPopup);
}

void DSHHub::finishInitialization()
{
	if (m_initializationComplete)
		return;

	m_initializationComplete = true;
	qInfo().noquote() << QStringLiteral("[DSH Hub] initialization complete");

	if (m_initOverlay) {
		m_initOverlay->hide();
		m_initOverlay->deleteLater();
		m_initOverlay = nullptr;
		m_initLabel = nullptr;
	}

	emit initializationComplete();
}

bool DSHHub::isInitializationComplete() const
{
	return m_initializationComplete;
}

QUrl DSHHub::baseUrl() const
{
	return m_api ? m_api->baseUrl() : QUrl();
}

void DSHHub::toggleTheme()
{
	Theme::switchTheme(this);
}

// openPlugins 已迁出：插件窗口的开关/遮罩/居中由常驻的
// PluginsManager（openPlugins()/closePlugins()）自管，见构造函数。

QProcess* DSHHub::takeServerProcess()
{
	return m_serverManager ? m_serverManager->takeProcess() : nullptr;
}

void DSHHub::adoptServerProcess(QProcess* process)
{
	if (m_serverManager)
		m_serverManager->adoptProcess(process);
}

DSHHub::~DSHHub()
{
	// 先停掉工具调用线程池，避免 Worker 仍在用 m_dllCaller / 排队任务引用 this
	if (m_toolPool) {
		m_toolPool->clear();
		m_toolPool->waitForDone();
		delete m_toolPool;
		m_toolPool = nullptr;
	}

	if (m_api)
		disconnect(m_api, nullptr, this, nullptr);

	delete m_dllCaller;
	delete m_messages;
	m_messages = nullptr;

	m_cacheManager.clearAll();
}

void DSHHub::onSendClicked()
{
	const QString text = m_chatInput->text().trimmed();
	if (text.isEmpty())
		return;

	if (m_sessionId.isEmpty()) {
		m_messages->addSystemMessage(QStringLiteral("还没有可用会话，正在自动创建..."),
			m_messagesLayout);
		createSessionAndSend(text);
		return;
	}

	sendPrompt(text);
}

void DSHHub::onStopRequested()  
{
	if (!m_streaming)
		return;

	if (!m_api || m_sessionId.isEmpty()) {
		// 没有可用会话时也把本地流式状态复位，避免按钮卡在中止态
		m_streaming = false;
		updateStreamingUi();
		return;
	}

	QJsonObject payload;
	payload.insert(QStringLiteral("sessionId"), m_sessionId);

	m_api->callMethod(
		QStringLiteral("session.cancel"),
		payload,
		[this](const QJsonObject&) {
			m_streaming = false;
			if (m_streamTimer)
				m_streamTimer->stop();
			if (m_messages && m_messages->lastAgentUnitIfLast())
				m_messages->lastAgentUnitIfLast()->flushStream();
			updateStreamingUi();
		},
		[this](const DshApiClient::RpcError& error) {
			// cancel 失败也恢复按钮状态，避免 UI 一直卡在中止态
			m_streaming = false;
			if (m_streamTimer)
				m_streamTimer->stop();
			if (m_messages && m_messages->lastAgentUnitIfLast())
				m_messages->lastAgentUnitIfLast()->flushStream();
			updateStreamingUi();
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("中止失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});

}

void DSHHub::updateStreamingUi()
{
	if (m_chatInput)
		m_chatInput->setStreaming(m_streaming);
}

void DSHHub::clearInteractionPanels()
{
	for (QWidget* panel : m_interactionPanels) {
		if (!panel)
			continue;

		if (m_messagesLayout)
			m_messagesLayout->removeWidget(panel);
		panel->hide();
		panel->deleteLater();
	}
	m_interactionPanels.clear();
}

void DSHHub::onNewWorkspaceClicked()
{
	const QString path = QFileDialog::getExistingDirectory(
		this,
		QStringLiteral("选择要加入工作区的目录"));

	if (path.isEmpty())
		return;

	QJsonObject payload;
	payload.insert(QStringLiteral("path"), path);

	m_api->callMethod(
		QStringLiteral("workspace.create"),
		payload,
		[this](const QJsonObject&) {
			if (m_sidebar && m_api && m_prefetcher)
				m_sidebar->refreshSessions(m_api, m_prefetcher);
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("新建工作区失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});

}

void DSHHub::onCreateSessionInWorkspace(const QString& workspaceId)
{
	QJsonObject payload;
	if (!workspaceId.isEmpty())
		payload.insert(QStringLiteral("workspaceId"), workspaceId);
	if (!m_defaultAgentPreset.isEmpty())
		payload.insert(QStringLiteral("agentPreset"), m_defaultAgentPreset);

	m_api->callMethod(
		QStringLiteral("session.create"),
		payload,
		[this, workspaceId](const QJsonObject& value) {
			const QString newSessionId = value.value(QStringLiteral("sessionId")).toString();
			if (newSessionId.isEmpty())
				return;

			cacheCurrentMessages();
			m_messages = new MessageQuery;
			m_sessionId = newSessionId;
			if (m_topBar)
				m_topBar->setTitle(QStringLiteral("未命名会话"));
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSessionToWorkspace(newSessionId, QStringLiteral("未命名会话"), workspaceId);
				m_sidebar->workspaceList()->setCurrentSession(newSessionId);
			}

			m_history.reset();
			m_usingPrefetched = false;
			if (m_loadMoreButton)
				m_loadMoreButton->hide();

			if (m_historyLoader) {
				m_historyLoader->setMessages(m_messages);
				m_historyLoader->setUsingPrefetched(false);
				m_historyLoader->load(newSessionId);
			}
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("新建会话失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});

}

void DSHHub::sendPrompt(const QString& text)
{
	if (m_sessionId.isEmpty() || !m_messages)
		return;

	m_streaming = false;
	if (m_streamTimer)
		m_streamTimer->stop();
	updateStreamingUi();
	m_messages->addUserMessage(text, m_messagesLayout);

	QJsonObject payload;
	payload.insert(QStringLiteral("sessionId"), m_sessionId);
	payload.insert(QStringLiteral("mode"), QStringLiteral("queue"));

	QJsonArray content;
	QJsonObject textPart;
	textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
	textPart.insert(QStringLiteral("text"), text);
	content.append(textPart);
	payload.insert(QStringLiteral("content"), content);

	m_api->callMethod(
		QStringLiteral("session.prompt"),
		payload,
		[this](const QJsonObject&) {
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("发送失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});

	m_chatInput->clear();
}

void DSHHub::createSessionAndSend(const QString& text)
{
	if (!m_api)
		return;

	QJsonObject payload;
	if (!m_defaultAgentPreset.isEmpty())
		payload.insert(QStringLiteral("agentPreset"), m_defaultAgentPreset);
	m_api->callMethod(
		QStringLiteral("session.create"),
		payload,
		[this, text](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			if (m_historyLoader)
				m_historyLoader->cancelBuild();
			cacheCurrentMessages();
			m_messages = new MessageQuery;
			m_sessionId = sid;
			m_history.reset();

			if (m_topBar)
				m_topBar->setTitle(QStringLiteral("未命名会话"));
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSession(sid, QStringLiteral("未命名会话"));
				m_sidebar->workspaceList()->setCurrentSession(sid);
			}
			if (m_loadMoreButton)
				m_loadMoreButton->hide();

			m_usingPrefetched = false;
			if (m_historyLoader) {
				m_historyLoader->setMessages(m_messages);
				m_historyLoader->setUsingPrefetched(false);
			}

			sendPrompt(text);
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("创建会话失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});

}

void DSHHub::cacheCurrentMessages()
{
	m_cacheManager.cacheOrDiscardCurrentSession(m_sessionId, m_messages, m_messagesLayout);
	m_messages = nullptr;
}

bool DSHHub::tryRestoreCachedMessages(const QString& sessionId)
{
	const bool partial = m_cacheManager.isPartialCache(sessionId);
	m_messages = m_cacheManager.restoreCachedSession(sessionId, m_messagesLayout);
	if (!m_messages)
		return false;

	if (m_loadMoreButton)
		m_loadMoreButton->setVisible(!m_messages->messages.empty());

	scrollToBottomNow();

	// If this is a partial cache built from prefetched history, continue loading full history.
	if (partial) {
		m_history.reset();
		if (m_historyLoader) {
			m_historyLoader->setMessages(m_messages);
			m_historyLoader->load(sessionId);
		}
	}

	return true;

}

void DSHHub::swapToMessageQuery(MessageQuery* query)
{
	clearInteractionPanels();
	if (!m_scrollArea)
		return;

	m_scrollArea->setUpdatesEnabled(false);

	if (m_messages) {
		m_messages->clear();
		delete m_messages;
		m_messages = nullptr;
	}

	m_messages = query;
	m_messages->attachToLayout(m_messagesLayout);

	// 在刷新前同步滚到底部，避免先显示顶部再闪烁
	scrollToBottomNow();
}

void DSHHub::scrollToBottomNow()
{
	if (!m_scrollArea || !m_scrollArea->widget())
		return;

	// 避免流式输出时频繁触发多个滚动任务
	if (m_scrollToBottomScheduled)
		return;
	m_scrollToBottomScheduled = true;

	// 在布局完成前一直禁用刷新，等滚动到底部后再一次性显示，
	// 避免先看到顶部再闪到底部。
	m_scrollArea->setUpdatesEnabled(false);

	// 强制触发一次布局，让 scrollbar maximum 尽快有效
	if (m_scrollArea->widget()->layout())
		m_scrollArea->widget()->layout()->activate();

	// 等 Qt 完成本轮布局/事件处理后，再真正滚动并恢复刷新
	QTimer::singleShot(0, this, [this]() {
		if (!m_scrollArea)
			return;

		// 再次强制布局/调整尺寸，确保 scrollbar maximum 已经更新
		QWidget* content = m_scrollArea->widget();
		if (content) {
			if (content->layout())
				content->layout()->activate();
			content->adjustSize();
		}

		if (m_scrollArea->verticalScrollBar())
			m_scrollArea->verticalScrollBar()->setValue(
				m_scrollArea->verticalScrollBar()->maximum());

		m_scrollToBottomScheduled = false;
		m_scrollArea->setUpdatesEnabled(true);
		m_scrollArea->viewport()->update();
		});

}

void DSHHub::onHistoryPrefetched(const QString& sessionId, const QJsonArray& events)
{
	if (sessionId.isEmpty() || events.isEmpty())
		return;

	// If the prefetched history belongs to the currently opened session, show it immediately.
	if (sessionId == m_sessionId) {
		if (!m_messages || !m_messages->messages.empty())
			return;
		m_messages->appendEvents(m_messagesLayout, events);
		m_history.setEventCount(events.size());
		m_usingPrefetched = true;
		scrollToBottomNow();
		if (m_historyLoader)
			m_historyLoader->setUsingPrefetched(true);
		return;
	}

	if (m_cacheManager.hasCachedMessages(sessionId))
		return;

	m_cacheManager.storePrefetchedHistory(sessionId, events);

	if (!m_prebuildQueue.contains(sessionId))
		m_prebuildQueue.append(sessionId);
	processPrebuildQueue();

}

void DSHHub::processPrebuildQueue()
{
	if (m_prebuilding)
		return;

	m_prebuilding = true;

	while (!m_prebuildQueue.isEmpty()) {
		const QString sessionId = m_prebuildQueue.takeFirst();

		if (sessionId == m_sessionId || m_cacheManager.hasCachedMessages(sessionId))
			continue;

		const QJsonArray events = m_cacheManager.takePrefetchedHistory(sessionId);
		if (events.isEmpty())
			continue;

		// 只预构建少量预取消息，控件树很小，避免初始化时卡顿
		MessageQuery* query = MessageQuery::fromEvents(events);
		m_cacheManager.cacheSessionMessages(sessionId, query);
		m_cacheManager.markPartialCache(sessionId);
		break;
	}

	m_prebuilding = false;

	if (!m_prebuildQueue.isEmpty())
		QTimer::singleShot(0, this, &DSHHub::processPrebuildQueue);

}

void DSHHub::onSessionSelected(const QString& sessionId)
{
	if (sessionId.isEmpty())
		return;

	// 切换会话时先取消上一个会话尚未完成的增量构建，避免旧消息覆盖新会话
	if (m_historyLoader)
		m_historyLoader->cancelBuild();
	// 切换会话时停止旧会话的流式渲染状态，避免旧输出继续污染新会话
	m_streaming = false;
	if (m_streamTimer)
		m_streamTimer->stop();
	updateStreamingUi();
	clearInteractionPanels();
	cacheCurrentMessages();

	m_sessionId = sessionId;

	if (m_sidebar)
		m_sidebar->workspaceList()->setCurrentSession(sessionId);

	if (m_topBar && m_sidebar)
		m_topBar->setTitle(m_sidebar->workspaceList()->titleForSession(sessionId));

	if (tryRestoreCachedMessages(sessionId))
		return;

	m_messages = new MessageQuery;
	m_history.setLimit(20);
	m_history.setHasMore(false);

	const QJsonArray prefetched = m_cacheManager.takePrefetchedHistory(sessionId);
	m_usingPrefetched = !prefetched.isEmpty();
	if (!prefetched.isEmpty()) {
		m_history.setEventCount(prefetched.size());
		m_messages->appendEvents(m_messagesLayout, prefetched);
		scrollToBottomNow();
	}
	else {
		m_history.setEventCount(0);
	}

	if (m_loadMoreButton)
		m_loadMoreButton->hide();

	if (m_historyLoader) {
		m_historyLoader->setMessages(m_messages);
		m_historyLoader->setUsingPrefetched(m_usingPrefetched);
		m_historyLoader->load(sessionId);
	}

}

void DSHHub::onDeleteSessionRequested(const QString& sessionId)
{
	if (sessionId.isEmpty() || !m_api)
		return;

	const auto ret = QMessageBox::question(
		this,
		QStringLiteral("删除会话"),
		QStringLiteral("确定要删除这个会话吗？此操作无法撤销。"),
		QMessageBox::Yes | QMessageBox::No,
		QMessageBox::No);
	if (ret != QMessageBox::Yes)
		return;

	QJsonObject payload;
	payload.insert(QStringLiteral("sessionId"), sessionId);

	m_api->callMethod(
		QStringLiteral("workspace.archiveSession"),
		payload,
		[this, sessionId](const QJsonObject&) {
			qInfo().noquote() << "[DSH Hub] session archived:" << sessionId;

			// archiveSession 只更新归档状态，这里同时删除本地会话文件，避免重启后残留
			if (m_serverManager) {
				const QString sessionsRoot = m_serverManager->dshHome() + QStringLiteral("/sessions");
				QDir sessionsDir(sessionsRoot);
				if (sessionsDir.exists()) {
					const QStringList workspaceDirs = sessionsDir.entryList(
						QDir::Dirs | QDir::NoDotAndDotDot);
					for (const QString& workspaceDir : workspaceDirs) {
						QDir candidate(sessionsDir.filePath(workspaceDir));
						if (candidate.exists(sessionId)) {
							QDir sessionDir(candidate.filePath(sessionId));
							if (sessionDir.removeRecursively()) {
								qInfo().noquote() << "[DSH Hub] session files removed:"
									<< sessionDir.absolutePath();
							}
							else {
								qWarning().noquote() << "[DSH Hub] failed to remove session files:"
									<< sessionDir.absolutePath();
							}
						}
					}
				}
			}

			if (sessionId == m_sessionId) {
				// 删除当前会话时也停止流式渲染
				m_streaming = false;
				if (m_streamTimer)
					m_streamTimer->stop();
				updateStreamingUi();
				clearInteractionPanels();
				// 当前会话被删除时，先丢弃当前消息容器，避免继续持有已归档会话
				m_cacheManager.cacheOrDiscardCurrentSession(QString(), m_messages, m_messagesLayout);
				m_messages = new MessageQuery;
				m_sessionId.clear();
				m_history.reset();
				if (m_loadMoreButton)
					m_loadMoreButton->hide();
				if (m_topBar)
					m_topBar->setTitle(QStringLiteral("New session"));
			}

			if (m_sidebar && m_api && m_prefetcher)
				m_sidebar->refreshSessions(m_api, m_prefetcher);
		},
		[this](const DshApiClient::RpcError& error) {
			qWarning().noquote() << "[DSH Hub] delete session failed:"
				<< error.code << error.message;
			if (m_messages) {
				m_messages->addSystemMessage(
					QStringLiteral("删除会话失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
			}
		});

}

void DSHHub::onClearConversationClicked()
{
	m_sidebar->clearAllSessions(
		m_serverManager->dshHome(),
		[this]() {
			clearInteractionPanels();
			if (m_messages)
				m_messages->clear();
			m_cacheManager.clearAll();
			m_sessionId.clear();

			if (m_messages) {
				if (AgentMessageUnit* agent = m_messages->lastAgentUnit())
					agent->clearStreamSegments();
			}
		},
		[this]() {
			callSessionCreate();
		});

}

void DSHHub::callSessionCreate()
{
	if (!m_api)
		return;

	QJsonObject payload;
	if (!m_defaultAgentPreset.isEmpty())
		payload.insert(QStringLiteral("agentPreset"), m_defaultAgentPreset);
	m_api->callMethod(
		QStringLiteral("session.create"),
		payload,
		[this](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			cacheCurrentMessages();
			m_messages = new MessageQuery;
			m_sessionId = sid;
			m_history.reset();

			if (m_topBar)
				m_topBar->setTitle(QStringLiteral("未命名会话"));
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSession(sid, QStringLiteral("未命名会话"));
				m_sidebar->workspaceList()->setCurrentSession(sid);
			}
			if (m_loadMoreButton)
				m_loadMoreButton->hide();

			m_usingPrefetched = false;
			if (m_historyLoader) {
				m_historyLoader->setMessages(m_messages);
				m_historyLoader->setUsingPrefetched(false);
				m_historyLoader->load(sid);
			}
		},
		[this](const DshApiClient::RpcError& error) {
			finishInitialization();
			if (m_messages)
				m_messages->addSystemMessage(
					QStringLiteral("创建会话失败: %1 %2").arg(error.code, error.message),
					m_messagesLayout);
		});

}

void DSHHub::handleConnected()
{
	qInfo().noquote() << QStringLiteral("[DSH Hub] connected to DSH");
	if (m_sidebar && m_api && m_prefetcher)
		m_sidebar->refreshSessions(m_api, m_prefetcher);
}

void DSHHub::onInitialSessionReady(const QString& sessionId, const QString& title)
{
	Q_UNUSED(title)
		onSessionSelected(sessionId);
}

void DSHHub::onSessionCreated(const QString& sessionId, const QString& workspaceId)
{
	// Cache the current session before switching to the new session.
	cacheCurrentMessages();
	m_messages = new MessageQuery;
	m_sessionId = sessionId;
	if (!m_defaultAgentPreset.isEmpty() && m_api) {
		QJsonObject presetPayload;
		presetPayload.insert(QStringLiteral("sessionId"), sessionId);
		presetPayload.insert(QStringLiteral("agentPreset"), m_defaultAgentPreset);
		m_api->callMethod(QStringLiteral("agentPreset.select"), presetPayload, {}, {});
	}
	m_history.reset();

	if (m_sidebar)
		m_sidebar->addCreatedSession(sessionId, workspaceId);
	if (m_sidebar)
		m_sidebar->workspaceList()->setCurrentSession(sessionId);
	if (m_topBar)
		m_topBar->setTitle(QStringLiteral("New session"));
	if (m_loadMoreButton)
		m_loadMoreButton->hide();

	if (m_historyLoader) {
		m_historyLoader->setMessages(m_messages);
		m_historyLoader->load(sessionId);
	}
}

void DSHHub::onNoSessionAvailable()
{
	if (m_sidebar && m_api)
		m_sidebar->createSession(m_api);

}

void DSHHub::onSessionListError(const QString& code, const QString& message)
{
	if (m_messages)
		m_messages->addSystemMessage(QStringLiteral("Session list error: %1 %2").arg(code, message), m_messagesLayout);
	finishInitialization();

}

void DSHHub::onSessionCreateError(const QString& code, const QString& message)
{
	if (m_messages)
		m_messages->addSystemMessage(QStringLiteral("Session create error: %1 %2").arg(code, message), m_messagesLayout);
	finishInitialization();

}

void DSHHub::onHistoryLoadMoreButtonVisibleChanged(bool visible)
{
	if (m_loadMoreButton)
		m_loadMoreButton->setVisible(visible);
}

void DSHHub::onHistoryError(const QString& code, const QString& message)
{
	if (m_messages)
		m_messages->addSystemMessage(QStringLiteral("History error: %1 %2").arg(code, message), m_messagesLayout);

}

void DSHHub::onIncrementalBuildReady(MessageQuery* query)
{
	if (!query)
		return;
	swapToMessageQuery(query);
	finishInitialization();
}

void DSHHub::openExtensions()
{
	if (m_extensionPopup)
		return;

	// 遮罩常驻复用：只在首次创建，关闭后仅隐藏，避免每次开关重建全窗半透明控件
	if (!m_extensionOverlay) {
		m_extensionOverlay = new QWidget(this);
		m_extensionOverlay->setObjectName(QStringLiteral("extensionOverlay"));
		m_extensionOverlay->setAttribute(Qt::WA_StyledBackground, true);
	}
	m_extensionOverlay->setGeometry(rect());
	m_extensionOverlay->raise();
	m_extensionOverlay->show();
	// 强制先让遮罩画出来（否则与下方弹窗同一帧才呈现，观感像弹窗先出、遮罩延迟）
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

	m_extensionPopup = new ExtensionManagerPopup(m_serverManager->dshHome() + QStringLiteral("/profiles/web"), this);
	m_extensionPopup->move(geometry().center() - m_extensionPopup->rect().center());
	m_extensionPopup->show();

	connect(m_extensionPopup, &ExtensionManagerPopup::serverRestartRequested,
		m_serverManager, &ServerManager::restart);
	connect(m_extensionPopup, &ExtensionManagerPopup::extensionInstalled,
		this, [this](const QString& jsonPath, const QString& dllPath) {
			if (m_dllCaller && m_dllCaller->loadDescriptor(jsonPath) && m_dllCaller->loadLibrary(dllPath)) {
				qInfo().noquote() << "[DSH DllCaller] ready tools=" << m_dllCaller->tools().join(',');
			}
			else if (m_dllCaller) {
				qWarning().noquote() << "[DSH DllCaller] load failed:" << m_dllCaller->errorString();
			}
		});
	connect(m_extensionPopup, &ExtensionManagerPopup::extensionRemoving,
		this, [this](const QString& name) {
			// 移除扩展前先卸载该扩展的 DLL，释放文件占用（其它扩展不受影响）
			if (m_dllCaller && !m_dllCaller->removeExtension(name)) {
				qWarning() << "[DSH DllCaller] removeExtension failed:" << name
					<< m_dllCaller->errorString();
			}
			// 如果移除后服务端因残留配置启动失败，自动清理一次
			m_cleanupResidualsAfterServerError = true;
		});
	connect(m_extensionPopup, &PopupWindow::closed, this, [this]() {
		// 遮罩常驻复用：只隐藏，不销毁
		if (m_extensionOverlay)
			m_extensionOverlay->hide();
		if (m_extensionPopup) {
			m_extensionPopup->deleteLater();
			m_extensionPopup = nullptr;
		}
		});
}

void DSHHub::handlePipeRequest(int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket)
{
	if (!m_dllCaller || !m_pipeBridge || !m_toolPool)
		return;

	// DLL/COM 调用挪到 Worker 线程执行：GUI 线程不再被长任务（如 ffmpeg
	// 转码、COM 调用）卡住；不同扩展/不同 DLL 的工具可并行。QLocalSocket
	// 只在 GUI 线程读写，Worker 完成后把响应投递回 GUI 线程发送。
	// 同一连接上多个请求的响应可能乱序返回，Node 端按请求 id 配对，无碍。
	struct PipeJob
	{
		int id = 0;
		QPointer<QLocalSocket> socket;
		QString tool;
		QJsonObject args;
	};

	auto* job = new PipeJob;
	job->id = id;
	job->socket = socket;
	job->tool = tool;
	job->args = args;

	auto* task = new FunctorTask([this, job]() {
		QJsonObject result;
		QString error;
		const bool ok = m_dllCaller->callTool(job->tool, job->args, result, &error);

		const int jobId = job->id;
		const QPointer<QLocalSocket> client = job->socket;
		const bool success = ok;
		const QJsonObject payload = result;
		const QString errText = error;
		delete job;

		QMetaObject::invokeMethod(this, [this, jobId, client, success, payload, errText]() {
			if (!m_pipeBridge || client.isNull())
				return; // 客户端已断开，丢弃响应
			if (success)
				m_pipeBridge->sendResponse(client.data(), jobId, true, payload);
			else
				m_pipeBridge->sendResponse(client.data(), jobId, false, QJsonObject(), errText);
			}, Qt::QueuedConnection);
		});

	m_toolPool->start(task);
}

void DSHHub::handleMuxFrame(const QJsonObject& frame)
{
	const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
	const QString type = payload.value(QStringLiteral("type")).toString();
	const QString frameSessionId = payload.value(QStringLiteral("sessionId")).toString();

	if (type == QStringLiteral("session/event")) {
		const QJsonObject event = payload.value(QStringLiteral("event")).toObject();
		// 防止在 A 会话输出时切到 B 会话，A 的流式内容错误地显示到 B 里；
		// 同时把 A 标记为 partial cache，切回 A 时会重新拉取历史，不丢失后台输出
		if (!frameSessionId.isEmpty() && frameSessionId != m_sessionId) {
			m_cacheManager.markPartialCache(frameSessionId);
			return;
		}
		const QString eventType = event.value(QStringLiteral("type")).toString();

		if (eventType == QStringLiteral("assistant/message")) {
			const QString thinking = extractThinking(event);

			const QString reply = extractReply(event);

			if (m_streaming) {
				// 已经通过 assistant/chunk 流式显示过，最终消息不再重复追加
				m_streaming = false;
				if (m_streamTimer)
					m_streamTimer->stop();
				updateStreamingUi();
				if (AgentMessageUnit* target = m_messages->lastAgentUnitIfLast())
					target->flushStream();
			}
			else {
				// 如果没有流式 chunk，也要先把已积压的工具调用/结果渲染出来
				if (AgentMessageUnit* target = m_messages->lastAgentUnitIfLast())
					target->flushStream();

				if (!thinking.isEmpty() || !reply.isEmpty()) {
					// 如果上一条仍然是 Agent 消息，就继续追加到同一个气泡里，保持连续
					AgentMessageUnit* target = m_messages->lastAgentUnitIfLast();
					if (target) {
						if (!thinking.isEmpty()) {
							target->appendThinking(thinking);
						}
						if (!reply.isEmpty())
							target->appendMarkdownWithCodeShadow(reply);
					}
					else {
						// 否则创建新的 AgentMessageUnit
						m_messages->addAgentMessage(reply, m_messagesLayout, thinking);
					}
				}
			}
			// 一次对话完成后，刷新会话标题（如果服务端已经生成了标题）
			if (m_sidebar && m_api)
				m_sidebar->workspaceList()->refreshTitles(m_api);
		}
		else if (eventType == QStringLiteral("assistant/chunk")) {
			const QString chunk = extractEventText(event);

			{
				const QString chunkType = extractChunkType(event);

				AgentMessageUnit* streamTarget = m_messages->lastAgentUnitIfLast();
				if (!streamTarget)
					streamTarget = m_messages->addAgentMessage(QString(), m_messagesLayout);

				if (chunkType == QStringLiteral("reasoning-delta") && !chunk.isEmpty()) {
					streamTarget->appendStreamChunk(StreamSegment::Thinking, chunk);
				}
				else if (chunkType == QStringLiteral("text-delta") && !chunk.isEmpty()) {
					streamTarget->appendStreamChunk(StreamSegment::Reply, chunk);
				}
				// 节流 50ms 批量全量重渲染一次，保证 Markdown/HTML 即时显示
				if (m_streamTimer && !m_streamTimer->isActive())
					m_streamTimer->start();

				m_streaming = true;
				updateStreamingUi();
			}
		}
		else if (eventType == QStringLiteral("text-chunks") || eventType == QStringLiteral("reasoning-chunks")) {
			const QJsonObject eventData = event.value(QStringLiteral("data")).toObject();
			const QJsonArray texts = eventData.value(QStringLiteral("texts")).toArray();
			if (!m_messages || texts.isEmpty())
				return;
			AgentMessageUnit* target = m_messages->lastAgentUnitIfLast();
			if (!target)
				target = m_messages->addAgentMessage(QString(), m_messagesLayout);
			const bool thinking = eventType == QStringLiteral("reasoning-chunks");
			for (const auto& value : texts) {
				const QString chunk = value.toString();
				if (chunk.isEmpty())
					continue;
				target->appendStreamChunk(thinking ? StreamSegment::Thinking : StreamSegment::Reply, chunk);
			}
			if (m_streamTimer && !m_streamTimer->isActive())
				m_streamTimer->start();
			m_streaming = true;
			updateStreamingUi();
		}
		else if (eventType == QStringLiteral("user/message")) {
			// 用户消息已在 onSendClicked 中创建 UserMessageUnit，这里避免重复显示。
		}
		else if (eventType == QStringLiteral("tool/call")) {
			const ToolCallInfo tool = extractToolCall(event);
			if (tool.valid && m_messages) {
				AgentMessageUnit* target = m_messages->lastAgentUnitIfLast();
				if (!target)
					target = m_messages->addAgentMessage(QString(), m_messagesLayout);
				const QString html = QStringLiteral("<pre>%1</pre>")
					.arg(QString::fromUtf8(
						QJsonDocument(tool.arguments).toJson(QJsonDocument::Indented))
						.toHtmlEscaped());
				target->appendStreamChunk(StreamSegment::ToolCall, html, tool.name);
				// 工具调用即使没有 assistant/chunk 也要能显示出来
				if (m_streamTimer && !m_streamTimer->isActive())
					m_streamTimer->start();
			}
		}
		else if (eventType == QStringLiteral("tool/result")) {
			const ToolResultInfo result = extractToolResult(event);
			if (result.valid && m_messages) {
				AgentMessageUnit* target = m_messages->lastAgentUnitIfLast();
				if (!target)
					target = m_messages->addAgentMessage(QString(), m_messagesLayout);
				const QString html = QStringLiteral("<pre>%1</pre>").arg(result.message.toHtmlEscaped());
				target->appendStreamChunk(StreamSegment::ToolResult, html);
				// 工具结果即使没有 assistant/chunk 也要能显示出来
				if (m_streamTimer && !m_streamTimer->isActive())
					m_streamTimer->start();
			}
		}
	}
	else if (type == QStringLiteral("question/requested")) {
		QWidget* panel = InteractionHandler::handleQuestion(frame, m_api, m_messagesLayout);
		if (panel) {
			m_interactionPanels.append(panel);
			scrollToBottomNow();
			connect(panel, &QObject::destroyed, this, [this, panel]() {
				m_interactionPanels.removeAll(panel);
				});
		}
		else if (m_messages) {
			m_messages->addSystemMessage(QStringLiteral("收到提问请求，但无法创建内联面板。"), m_messagesLayout);
		}
	}
	else if (type == QStringLiteral("approval/requested")) {
		QWidget* panel = InteractionHandler::handleApproval(frame, m_api, m_messagesLayout);
		if (panel) {
			m_interactionPanels.append(panel);
			scrollToBottomNow();
			connect(panel, &QObject::destroyed, this, [this, panel]() {
				m_interactionPanels.removeAll(panel);
				});
		}
		else if (m_messages) {
			m_messages->addSystemMessage(QStringLiteral("收到审批请求，但无法创建内联面板。"), m_messagesLayout);
		}
	}

}

void DSHHub::handleTransportError(const QString& context, const QString& message)
{
	// 服务端主动重启时，旧 WebSocket 断开是预期行为，不当作错误刷到聊天区
	if (m_serverManager && m_serverManager->isRestarting())
		return;

	m_messages->addSystemMessage(
		QStringLiteral("传输错误 [%1]: %2").arg(context, message),
		m_messagesLayout);

}

void DSHHub::showNoMoreToast()
{
	if (!m_toastLabel)
		return;

	QWidget* parent = qobject_cast<QWidget*>(m_toastLabel->parent());
	if (!parent)
		parent = this;

	m_toastLabel->setText(QStringLiteral("啊哦，没有更多了"));
	m_toastLabel->adjustSize();
	m_toastLabel->setGeometry(
		(parent->width() - m_toastLabel->width() - 32) / 2,
		parent->height() - m_toastLabel->height() - 24,
		m_toastLabel->width() + 32,
		m_toastLabel->height());
	m_toastLabel->show();
	m_toastLabel->raise();

	auto* effect = new QGraphicsOpacityEffect(m_toastLabel);
	m_toastLabel->setGraphicsEffect(effect);

	auto* fadeIn = new QPropertyAnimation(effect, "opacity", m_toastLabel);
	fadeIn->setDuration(180);
	fadeIn->setStartValue(0.0);
	fadeIn->setEndValue(1.0);
	connect(fadeIn, &QPropertyAnimation::finished, this, [this]() {
		QTimer::singleShot(1200, this, [this]() {
			if (!m_toastLabel)
				return;

			auto* effect = qobject_cast<QGraphicsOpacityEffect*>(m_toastLabel->graphicsEffect());
			if (!effect)
				return;

			auto* fadeOut = new QPropertyAnimation(effect, "opacity", m_toastLabel);
			fadeOut->setDuration(300);
			fadeOut->setStartValue(1.0);
			fadeOut->setEndValue(0.0);
			connect(fadeOut, &QPropertyAnimation::finished, m_toastLabel, [this]() {
				if (m_toastLabel)
					m_toastLabel->hide();
				});
			fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
			});
		});
	fadeIn->start(QAbstractAnimation::DeleteWhenStopped);
}
