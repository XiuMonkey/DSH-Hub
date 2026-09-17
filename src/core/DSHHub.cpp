#include "DSHHub.h"
#include "ServerManager.h"
#include "SessionCommands.h"
#include "ThemeManager.h"
#include "ToolRequestDispatcher.h"
#include "ChatInputWidget.h"
#include "Sidebar.h"
#include "Logger.h"

#include "DshApiClient.h"
#include "MessageHost.h"
#include "SessionPrefetcher.h"
#include "CodeHighlighter.h"
#include "TopBar.h"
#include "TitleBar.h"
#include "WindowFrame.h"
#include "Settings.h"
#include "PluginsManager.h"
#include "DshNamedPipeBridge.h"
#include "DllCaller.h"
#include "ExtensionManagerPopup.h"
#include "InteractionHandler.h"

#include "AgentMessageUnit.h"

// 注意：current() 返回 MessageQuery*，这里调用它的成员函数（lastAgentUnit 等）
// 所以需要完整类型，不能只靠前置声明
#include "MessageQuery.h"

#include <QCoreApplication>
#include <QMoveEvent>
#include <QShowEvent>

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFileDialog>

#include <QJsonArray>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>

#include <QPushButton>
#include <QResizeEvent>

#include <QScrollBar>
#include <QScrollArea>

#include <QProcess>

#include <QUrl>
#include <QUrlQuery>
#include <QLocalSocket>
#include <QThreadPool>

#include "SettingsStore.h"

DSHHub::DSHHub(QWidget* parent, const QUrl& initialBaseUrl, QProcess* initialServerProcess)
	: QMainWindow(parent)
	, m_api(new DshApiClient(this))
{
	qInfo().noquote() << QStringLiteral("[DSH Hub] constructor started");
	TimingLogger::mark(QStringLiteral("DSHHub ctor enter"));

	// 服务spawn 提前到构造函数最前：Node 进程启动.9~1.6s）与下面
	// 的扩展加载 / UI 构建 / 首帧真正并行，缩短初始化墙钟时间。
	// ServerManager::start 会同步填dshHome，因此之后创建的
	// Settings/PluginsManager 仍可正常使用它
	m_serverManager = new ServerManager(this);
	connect(m_serverManager, &ServerManager::baseUrlReady, this, [this](const QUrl& url) {
		TimingLogger::mark(QStringLiteral("server baseUrl ready -> open WS streams"));
		if (!m_api)
			return;
		m_api->setBaseUrl(url);
		if (m_pluginsManager) {
			// 注意传"干净"的 baseUrl：ServerManager 给出来的这条带启动令牌
			// （…/?token=…），插件市场拿它拼接口地址会把 path 弄丢（见
			// PluginMarketClient::endpointUrl 的说明）。
			m_pluginsManager->setBaseUrl(m_api->baseUrl());
		}
		m_api->openStreams();
		});
	connect(m_serverManager, &ServerManager::errorLine, this, [this](const QString& line) {
		if (m_messageHost && m_messageHost->current())
			m_messageHost->addSystemMessage(tr("DSH 服务端: %1").arg(line));
		// 移除扩展后服务端启动失败时，自动清理 cordis.patch.yml 残留
		if (m_cleanupResidualsAfterServerError && m_extensionPopup) {
			m_cleanupResidualsAfterServerError = false;
			m_extensionPopup->cleanupResiduals();
		}
		if (!isInitializationComplete())
			finishInitialization();
		});
	connect(m_serverManager, &ServerManager::outputLine, this, [](const QString& line) {
		// 只记录服务端里可能与插件市场/扩展/服务本身相关的输出，避免刷爆日志
		// 设置环境变量 DSH_HUB_SERVER_TRACE=1 可转储服务端全部 stdout
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
			if (m_messageHost && m_messageHost->current())
				m_messageHost->addSystemMessage(tr("DSH 服务端已退出，代码: %1").arg(exitCode));
			if (m_cleanupResidualsAfterServerError && m_extensionPopup) {
				m_cleanupResidualsAfterServerError = false;
				m_extensionPopup->cleanupResiduals();
			}
			if (exitCode != 0 && !isInitializationComplete())
				finishInitialization();
		}
		});

	m_serverManager->start(initialBaseUrl, initialServerProcess);

	// DLL/COM 工具调用线程池：请求Worker 上执行（GUI 不阻塞）
	// DllCaller 内部DLL 串行、跨 DLL 并行
	m_toolPool = new QThreadPool(this);
	m_toolPool->setMaxThreadCount(4);

	// 无边框窗口：系统标题栏与边框全部由自绘替—标题栏见 TitleBar
	// 圆角 + 1px 描边#dshhubCentral QSS 画（main-window.qss），
	// 窗口本体透明，圆角之外什么都不画
	// 必须在原生窗口创建之前设置，否则会触发窗口重建
	setObjectName(QStringLiteral("dshHubWindow"));
	setWindowFlag(Qt::FramelessWindowHint, true);
	setAttribute(Qt::WA_TranslucentBackground, true);

	// 样式表按窗口安装（替代全局 qApp 表）：本窗口与后续加入的子控件统一应用当前主题
	Theme::applyToWindow(this);
	setWindowTitle(QStringLiteral("DSH Hub"));
	setAttribute(Qt::WA_DeleteOnClose);

	m_defaultAgentPreset = SettingsStore::defaultAgentPresetId();

	// 启动命名管道桥接服务，供 Node/DSh server 调用 DLL 工具
	m_pipeBridge = new DshNamedPipeBridge(this);
	connect(m_pipeBridge, &DshNamedPipeBridge::requestReceived,
		this, &DSHHub::handlePipeRequest);
	if (!m_pipeBridge->start()) {
		qWarning() << QStringLiteral("[DSH Pipe] failed to start:") << m_pipeBridge->errorString();
	}

	// 初始JSON5 DLL 调用
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
		// env 只给了描述符（调试）：目录下main.dll 作为库一并加
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
		// 多扩展并存后改为全部加载，DllCaller 内部按扩展名去重
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

	TimingLogger::mark(QStringLiteral("extension DLLs loaded"));

	buildUi();
	// 初始化时Qt 资源中加载代码高亮规
	{
		if (!CodeHighlighter::instance().loadFromFile(QStringLiteral(":/DSHHub/highlight_rules.json"))) {
			qWarning().noquote() << QStringLiteral("[DSH Hub] 未找到内置资源 highlight_rules.json，代码高亮不可用");
		}
	}

	// 信号
	// （发中止入口与流式节流定时器流式 + 输入搬进MessageHost，在那里接线
	connect(m_chatInput, &ChatInputWidget::modelChanged, this,
		[](const QString& provider, const QString& model) {
			qInfo().noquote() << QStringLiteral("[DSH Hub] model ->")
				<< QStringLiteral("%1/%2").arg(provider, model);
		});
	connect(m_chatInput, &ChatInputWidget::thinkingDepthChanged, this, [](const QString& levelId) {
		qInfo().noquote() << QStringLiteral("[DSH Hub] thinking depth ->")
			<< (levelId.isEmpty() ? tr("(默认)") : levelId);
		});

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
	// 0.1.5：历史由 session/follow 快照播种，工作区workspace/follow 驱动
	connect(m_api, &DshApiClient::sessionSnapshotReady, this, &DSHHub::handleSessionSnapshot);
	// 小灰字：快照里那份"全量折叠"的投影做种子 + session/control 的实时推送做更新。
	// 两条都是服务端现算，客户端不读任何缓存（列表行那条读的是投影缓存检查点，已弃用）。
	connect(m_api, &DshApiClient::sessionProjectionsReady,
		this, &DSHHub::handleSessionProjections);
	connect(m_api, &DshApiClient::sessionProjectionsBaselineReady,
		this, &DSHHub::handleSessionControlBaseline);
	connect(m_api, &DshApiClient::sessionProjectionChanged,
		this, &DSHHub::handleSessionProjectionChanged);
	connect(m_api, &DshApiClient::workspaceSnapshotReady, this, &DSHHub::handleWorkspaceSnapshot);
	connect(m_api, &DshApiClient::workspaceUpserted, this, &DSHHub::handleWorkspaceUpserted);
	connect(m_api, &DshApiClient::workspaceRemoved, this, &DSHHub::handleWorkspaceRemoved);
	connect(m_api, &DshApiClient::workspaceReordered, this, &DSHHub::handleWorkspaceReordered);
	connect(m_api, &DshApiClient::workspaceArchiveChanged, this, &DSHHub::handleWorkspaceArchiveChanged);
	connect(m_api, &DshApiClient::transportError, this, &DSHHub::handleTransportError);
	// 侧边栏“设置”入口：Settings 是常驻“设置系统”，窗口开关由它自己管理，
	// 这里只做一次接线；实际创建放在构造函数尾部（ServerManager start 之后
	// 那时 dshHome 才可用）
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
	// 会话列表刷新（含启动、增删会话）对可见会话排一轮首屏预
	connect(m_sidebar, &Sidebar::sessionsRefreshed,
		this, &DSHHub::onSessionsRefreshed);
	connect(m_sidebar, &Sidebar::sessionCreated,
		this, &DSHHub::onSessionCreated);
	connect(m_sidebar, &Sidebar::noSessionAvailable,
		this, &DSHHub::onNoSessionAvailable);
	connect(m_sidebar, &Sidebar::sessionListError,
		this, &DSHHub::onSessionListError);
	connect(m_sidebar, &Sidebar::sessionCreateError,
		this, &DSHHub::onSessionCreateError);

	// 消息区宿主：拥有当前列表、HistoryLoader、预构建队列与消息区的 UI 反馈。
	// 控件由 buildUi() 搭好传进来，这里只做接线。
	//
	// parent 故意传 nullptr：MessageHost 持有 MessageQuery，而 MessageQuery::clear()
	// 会 delete 各气泡容器（它们的父对象是窗口里的滚动区）。若把它挂成本窗口的子
	// QObject，Qt 的销毁顺序是"先 ~QWidget 删掉整棵控件树，再 ~QObject 删子对象"，
	// 那时容器已是野指针，clear() 会二次释放（实测崩在 MessageQuery::clear）。
	// 因此它由 ~DSHHub 在函数体里显式删除——那时控件树还活着，与搬家前的顺序一致。
	m_messageHost = new MessageHost(m_api, &m_cacheManager, m_scrollArea, m_messagesLayout,
		m_loadMoreButton, m_toastLabel, m_chatInput, nullptr);
	// 没有会话时点发送：建会话要动侧边栏/会话列表，归 DSHHub；建好后由它调 sendPrompt 回来
	connect(m_messageHost, &MessageHost::sendWithoutSession,
		this, &DSHHub::createSessionAndSend);
	// 首屏内容真正上屏 收掉启动遮罩（finishInitialization 幂等
	connect(m_messageHost, &MessageHost::contentReady, this, &DSHHub::finishInitialization);
	// 整列表被整体替换（缓存恢首屏构建完成）→ 先收掉内联交互面板，
	// 再在刷新前同步滚到底，避免先显示顶部再闪烁
	connect(m_messageHost, &MessageHost::contentReplaced, this, [this]() {
		clearInteractionPanels();
		m_messageHost->scrollToBottomNow();
		});

	// 首屏预取：session/list 回来后并发发一session/page，结果入库供"立即点亮"
	m_prefetcher = new SessionPrefetcher(this);
	m_prefetcher->setApi(m_api);
	connect(m_prefetcher, &SessionPrefetcher::historyFetched, this,
		[this](const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore) {
			if (!m_messageHost)
				return;
			// 命中当前会话且界面还空着 已用它点亮；这时才记"见过的最seq"
			if (m_messageHost->onPrefetched(sessionId, events, throughSeq, hasMore)
				== MessageHost::PrefetchOutcome::Painted) {
				m_sessionLastSeq = qMax(m_sessionLastSeq, throughSeq);
			}
		});
	connect(m_prefetcher, &SessionPrefetcher::prefetchFailed,
		this, [](const QString& sessionId, const QString& code, const QString& message) {
			qWarning().noquote() << "[DSH Hub] prefetch failed sessionId=" << sessionId
				<< "code=" << code << "message=" << message;
		});

	// ------------------------------------------------------------------
	// 常驻“设置系统”：随主窗口存在，自管设置窗口的开关/遮罩/居中。
	// 放在 start() 之后创建，因Settings 构造需dshHome
	// （由 ServerManager::start 填充）。这里只做一次业务信号接线：
	// 预设变更同步给当前会话、新增模型后刷新选择器
	// ------------------------------------------------------------------
	m_settings = new Settings(m_api, this);
	connect(m_settings, &Settings::agentPresetChanged, this, [this](const QString& presetId) {
		m_defaultAgentPreset = presetId;
		if (!m_sessionId.isEmpty() && m_api) {
			m_api->callMethod(QStringLiteral("agentPresets/select"),
				SessionCommands::agentPresetSelect(m_sessionId, presetId), {}, {});
		}
		});
	connect(m_settings, &Settings::serverSettingsSaved, this, [this]() {
		if (m_serverManager)
			m_serverManager->restart();
		});
	// 设置里新增了模型：服务端 settings 已热生效，输入框底的模型选择
	// 需要重新拉一次会话目录才能看到新模型
	connect(m_settings, &Settings::modelAdded, this, [this](const QString& provider, const QString& modelId) {
		qInfo().noquote() << QStringLiteral("[DSH Hub] model added ->")
			<< QStringLiteral("%1/%2").arg(provider, modelId);
		if (m_chatInput)
			m_chatInput->refreshModelCatalog();
		});

	// ------------------------------------------------------------------
	// 常驻“插件系统”：随主窗口存在，自管插件窗口的开关/遮罩/居中。
	// baseUrl 会在 ServerManager::baseUrlReady 时经 setBaseUrl() 更新
	// serverRestartRequested（插件内“重启服务”）只在此接线一次
	// ------------------------------------------------------------------
	m_pluginsManager = new PluginsManager(m_api ? m_api->baseUrl() : QUrl(), this);
	connect(m_pluginsManager, &PluginsManager::serverRestartRequested,
		m_serverManager, &ServerManager::restart);

	// ------------------------------------------------------------------
	// 工具栏右侧的“工具过滤”：baseUrl 用回调现取（启动/重启都会变），
	// 会话 id 由 setSessionId() 在切会话时推给 TopBar。
	// ------------------------------------------------------------------
	if (m_topBar) {
		m_topBar->setBaseUrlProvider([this]() { return m_api ? m_api->baseUrl() : QUrl(); });
	}

	TimingLogger::mark(QStringLiteral("DSHHub ctor done (server spawned / UI ready)"));
}

void DSHHub::resizeEvent(QResizeEvent* event)
{
	QMainWindow::resizeEvent(event);

	// 窗口尺寸变了就重算一次边框状态（含跨显示DPI 变化时的工作区补偿）
	syncWindowFrameStyle();

	if (m_initOverlay)
		m_initOverlay->setGeometry(rect());

	if (m_messageHost)
		m_messageHost->syncLoadingGeometry();

	if (m_settings)
		m_settings->syncOverlayToHost();

	if (m_pluginsManager)
		m_pluginsManager->syncOverlayToHost();
	WindowFrame::syncOverlay(this);   // 扩展管理弹窗的遮罩

	// 宿主缩放后把打开的弹窗重新居
	keepOpenPopupsCentered();
}

void DSHHub::moveEvent(QMoveEvent* event)
{
	QMainWindow::moveEvent(event);
	// 弹窗是宿主“拥有的”独立窗口（Windows 上不随宿主拖动），这里手动跟
	keepOpenPopupsCentered();
}

// ------------------------------------------------------------------
// 无边框窗口（自绘圆角边框 + 自绘标题栏）
// ------------------------------------------------------------------

void DSHHub::showEvent(QShowEvent* event)
{
	QMainWindow::showEvent(event);
	// 原生窗口到这一刻才真正存在，补样式位只能在这里做（逻辑common/WindowFrame
	WindowFrame::applyNativeStyle(this);
	syncWindowFrameStyle();
}

void DSHHub::changeEvent(QEvent* event)
{
	QMainWindow::changeEvent(event);
	if (event->type() == QEvent::WindowStateChange)
		syncWindowFrameStyle();
}

// ------------------------------------------------------------------
// 边框的“界面收尾”：判定与顺序都common/WindowFrame::applyFrameStyle
// 这里只把结果转给标题栏——按钮图标属于控件，common 层不该认TitleBar
// 画的部分resources/styles/main-window.qss #dshhubCentral
// ------------------------------------------------------------------
void DSHHub::syncWindowFrameStyle()
{
	const bool edgeToEdge = WindowFrame::applyFrameStyle(this, centralWidget());
	// 标题栏中间的按钮在“最大化/还原”之间换图标
	if (m_titleBar)
		m_titleBar->setMaximizedState(edgeToEdge);
}

bool DSHHub::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
	// 无边框窗口的原生消息判定全在 common/WindowFrame（含 WM_NCCALCSIZE 让客户区
	// 铺满窗口、WM_NCHITTEST 判缩放热区与标题栏拖动区）；这里只是转交
	if (WindowFrame::handleNativeMessage(this, m_titleBar, eventType, message, result))
		return true;

	return QMainWindow::nativeEvent(eventType, message, result);
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
	if (m_topBar)
		m_topBar->syncOverlayToHost();   // 铺满遮罩 + 让"工具过滤"窗口跟着居中
}

void DSHHub::finishInitialization()
{
	if (m_initializationComplete)
		return;

	m_initializationComplete = true;
	qInfo().noquote() << QStringLiteral("[DSH Hub] initialization complete");
	TimingLogger::mark(QStringLiteral("initialization complete (overlay hidden)"));

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

QUrl DSHHub::authenticatedBaseUrl() const
{
	QUrl url = baseUrl();
	const QString token = m_api ? m_api->launchToken() : QString();
	if (!url.isEmpty() && !token.isEmpty()) {
		// 0.1.5：新窗口必须带上启动令牌，才能换到 /api 的认证 cookie
		QUrlQuery query(url);
		query.addQueryItem(QStringLiteral("token"), token);
		url.setQuery(query);
	}
	return url;
}

void DSHHub::toggleTheme()
{
	Theme::switchTheme(this);
}

// openPlugins 已迁出：插件窗口的开关/遮罩/居中由常驻的
// PluginsManager（openPlugins()/closePlugins()）自管，见构造函数

QProcess* DSHHub::takeServerProcess()
{
	return m_serverManager ? m_serverManager->takeProcess() : nullptr;
}

DSHHub::~DSHHub()
{
	// 先停掉工具调用线程池，避Worker 仍在m_dllCaller / 排队任务引用 this
	if (m_toolPool) {
		m_toolPool->clear();
		m_toolPool->waitForDone();
		delete m_toolPool;
		m_toolPool = nullptr;
	}

	if (m_api)
		disconnect(m_api, nullptr, this, nullptr);

	// 消息列表与 HistoryLoader 归 MessageHost 所有。必须在这里（控件树仍活着时）显式删除：
	// 它没有父对象，靠 Qt 的父子链会在 ~QWidget 之后才析构，那时气泡容器已被删掉，
	// MessageQuery::clear() 会二次释放（见构造函数里的说明）。
	delete m_messageHost;
	m_messageHost = nullptr;

	delete m_dllCaller;

	m_cacheManager.clearAll();
}


void DSHHub::syncComposerSession()
{
	// 输入区底部的“模型 / 思考深度”控件按当前会话的模型目录刷新；
	// 会话为空（尚未创已被删除）时控件自行隐藏
	if (m_chatInput)
		m_chatInput->setModelSession(m_api, m_sessionId);

	// 0.1.5：modelCatalog 只给部署默认值，会话自己的选择session/list 
	// projections.values.modelSelection 里（已随会话行进SessionCatalog），
	// 这里把它推给 chip，避chip 显示成默认模型
	if (m_chatInput && m_sidebar && !m_sessionId.isEmpty()) {
		QString provider;
		QString model;
		QString effort;
		if (m_sidebar->workspaceList()->catalog().modelSelectionFor(m_sessionId, &provider, &model, &effort))
			m_chatInput->applySessionModelSelection(provider, model, effort);
		else
			m_chatInput->applySessionModelSelection(QString(), QString(), QString());
	}

	// 换会话：这块合并态要整体重来（两块投影 + asOfSeq），否则旧会话的数字会留在
	// 新会话上；控件那条高度照旧占着，只是先不画字。
	// 重新取数不在这里做：换会话时会重开 session/follow（见 onSessionSelected /
	// switchToFreshSession），快照里那份现算投影会自己送上门。
	m_sessionStatsBlock = QJsonObject();
	m_tokenUsageBlock = QJsonObject();
	m_statsAsOfSeq = -1;
	if (m_chatInput)
		m_chatInput->clearSessionStats();
}

namespace
{
	/**
	 * 从 session/list 一行的 projections.values 里取出输入区小灰字要的两块投影。
	 *
	 * 字段名来自 @deepseek-ai/dsh-session-stats 与 @deepseek-ai/dsh-token-meter 的
	 * projection.d.ts；服务端保证 key 一定在（还没有事件时各字段都是 0），
	 * 所以这里做宽松解析即可 —— 解析不出来就是 0，控件自己决定"0 就是不显示"。
	 */
	SessionUsageStats parseSessionUsage(const QJsonObject& projectionValues)
	{
		SessionUsageStats stats;

		const QJsonObject sessionStats =
			projectionValues.value(QStringLiteral("sessionStats")).toObject();
		stats.turns = sessionStats.value(QStringLiteral("turns")).toInt();
		stats.steps = sessionStats.value(QStringLiteral("steps")).toInt();
		// 毫秒数是累加值，可能很大：一律走 double 再取整（JSON 里本来就是 double）
		stats.llmMs = static_cast<qint64>(sessionStats.value(QStringLiteral("llmMs")).toDouble());
		stats.toolMs = static_cast<qint64>(sessionStats.value(QStringLiteral("toolMs")).toDouble());
		stats.ttftMs = static_cast<qint64>(sessionStats.value(QStringLiteral("ttftMs")).toDouble());
		stats.ttftSteps = sessionStats.value(QStringLiteral("ttftSteps")).toInt();
		stats.decodeMs = static_cast<qint64>(sessionStats.value(QStringLiteral("decodeMs")).toDouble());
		stats.decodeTokens =
			static_cast<qint64>(sessionStats.value(QStringLiteral("decodeTokens")).toDouble());

		const QJsonObject tokenUsage =
			projectionValues.value(QStringLiteral("tokenUsage")).toObject();
		if (!tokenUsage.isEmpty()) {
			stats.hasUsage = true;
			stats.uncachedInputTokens =
				static_cast<qint64>(tokenUsage.value(QStringLiteral("uncachedInputTokens")).toDouble());
			stats.cacheReadTokens =
				static_cast<qint64>(tokenUsage.value(QStringLiteral("cacheReadTokens")).toDouble());
			stats.cacheWriteTokens =
				static_cast<qint64>(tokenUsage.value(QStringLiteral("cacheWriteTokens")).toDouble());
			stats.outputTokens =
				static_cast<qint64>(tokenUsage.value(QStringLiteral("outputTokens")).toDouble());
		}

		return stats;
	}
}

/**
 * 输入卡片下方那行小灰字（会话统计）的数据入口说明。
 *
 * 两块投影都来自服务端现算，客户端**不读任何缓存**：
 *   sessionStats  轮/步 + LLM/工具/首 token/解码耗时（@deepseek-ai/dsh-session-stats）
 *   tokenUsage    输入（未缓存/缓存读/缓存写）与输出 token（@deepseek-ai/dsh-token-meter）
 *
 * 三个来源，都是服务端当场算的：
 *   1. session/follow 快照里的 projections —— 全量折叠整条日志，打开会话就有（冷会话也准）
 *   2. session/control 的 baseline          —— 活会话的现算快照（未附着的会话不在里面）
 *   3. session/control 的 projection 帧     —— 投影一变化就推，做到"每步实时跳"
 * 早先用过 session/list 行，那条读的是投影缓存检查点（冷会话会很旧），已弃用。
 *
 * 显示规则与官方 Web 端 composer 下方的 StatsLine 对齐（见该包 StatsLine.d.ts），
 * 拼行在 SessionStatsLine::formatStats。这些都暂时搁在这个 god class 里：
 * 将来拆的话，取数该进一个 SessionStatsService，"什么时候刷"该回输入区自己的控制器。
 */

void DSHHub::clearInteractionPanels()
{
	for (QWidget* panel : m_interactionPanels) {
		if (!panel)
			continue;

		if (m_messageHost->layout())
			m_messageHost->layout()->removeWidget(panel);
		panel->hide();
		panel->deleteLater();
	}
	m_interactionPanels.clear();
}

void DSHHub::onNewWorkspaceClicked()
{
	const QString path = QFileDialog::getExistingDirectory(
		this,
		tr("选择要加入工作区的目录"));

	if (path.isEmpty())
		return;

	m_api->callMethod(
		QStringLiteral("workspace/create"),
		SessionCommands::workspaceCreate(path),
		[this](const QJsonObject&) {
			if (m_sidebar && m_api)
				m_sidebar->refreshSessions(m_api);
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messageHost->current()) {
				m_messageHost->addSystemMessage(tr("新建工作区失败: %1 %2").arg(error.code, error.message));
			}
		});
}

void DSHHub::onCreateSessionInWorkspace(const QString& workspaceId)
{
	m_api->callMethod(
		QStringLiteral("session/create"),
		SessionCommands::sessionCreate(workspaceId, m_defaultAgentPreset),
		[this, workspaceId](const QJsonObject& value) {
			const QString newSessionId = value.value(QStringLiteral("sessionId")).toString();
			if (newSessionId.isEmpty())
				return;

			switchToFreshSession(newSessionId, tr("未命名会话"), /*loadHistory=*/true);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSessionToWorkspace(newSessionId, tr("未命名会话"), workspaceId);
				m_sidebar->workspaceList()->setCurrentSession(newSessionId);
			}
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messageHost->current()) {
				m_messageHost->addSystemMessage(tr("新建会话失败: %1 %2").arg(error.code, error.message));
			}
		});
}

void DSHHub::createSessionAndSend(const QString& text)
{
	if (!m_api)
		return;

	m_api->callMethod(
		QStringLiteral("session/create"),
		SessionCommands::sessionCreate(QString(), m_defaultAgentPreset),
		[this, text](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			// load 历史：等首条 prompt mux 事件即可；adoptSession 
			// loader 绑定新会话，之后"加载更多"不会误用旧会id
			switchToFreshSession(sid, tr("未命名会话"), /*loadHistory=*/false);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSession(sid, tr("未命名会话"));
				m_sidebar->workspaceList()->setCurrentSession(sid);
			}

			m_messageHost->sendPrompt(text);
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messageHost->current()) {
				m_messageHost->addSystemMessage(tr("创建会话失败: %1 %2").arg(error.code, error.message));
			}
		});
}

void DSHHub::switchToFreshSession(const QString& sessionId, const QString& title, bool loadHistory)
{
	// onSessionSelected 切换语义保持一致：停流式与交互面板，避免旧状态污染新会话
	// （取消旧构建MessageHost::showFreshSession 内部做）
	if (m_messageHost)
		m_messageHost->stopStreaming();
	clearInteractionPanels();

	m_sessionId = sessionId;
	// 0.1.5：实时日志事件来mux per-session session/follow 流，
	// 换会话时把跟随流也换过去（快照帧会补齐初始历史，DshApiClient）
	if (m_api)
		m_api->followSession(sessionId);
	// 新会话重新统见过的最seq"（缓存新鲜度判定用）；旧值交给消息区写缓存元数据
	const int observedLastSeq = m_sessionLastSeq;
	m_sessionLastSeq = 0;
	syncComposerSession();

	if (m_topBar)
		m_topBar->setTitle(title);
	if (m_topBar)
		m_topBar->setSessionId(sessionId);

	// 消息区：旧内容交缓存 → 换空列表 → 按需绑定/加载 loader（回落游标用 asOfSeq）
	if (m_messageHost) {
		const int fallbackCursor = m_sidebar
			? m_sidebar->workspaceList()->catalog().asOfSeqFor(sessionId)
			: 0;
		m_messageHost->showFreshSession(sessionId, loadHistory, fallbackCursor, observedLastSeq);
	}
}

// ------------------------------------------------------------------
// 首屏预取.1.5：一session/page + 事件入库 + 立即点亮
// ------------------------------------------------------------------

/**
 * 会话列表刷新后，对每个可见会话排一轮预取
 *
 * 游标session/list 行里projections.asOfSeq（实测与 follow 快照cursor 相等），
 * 所以预*不需要先开任何*。asOfSeq<=0 的会话（例如刚创建、还没产生投影）跳过
 */
void DSHHub::onSessionsRefreshed()
{
	if (!m_prefetcher || !m_api || !m_sidebar)
		return;

	m_prefetcher->setApi(m_api);

	const QVector<SessionRecord> sessions = m_sidebar->workspaceList()->catalog().visibleSessions();
	int scheduled = 0;
	for (const SessionRecord& record : sessions) {
		if (record.asOfSeq <= 0)
			continue;
		if (m_cacheManager.hasCachedMessages(record.sessionId))
			continue; // 已有控件树：进入它就是 cache hit，不必再预取
		m_prefetcher->prefetch(record.sessionId, record.asOfSeq, 20);
		++scheduled;
	}

	if (scheduled > 0)
		TimingLogger::mark(QStringLiteral("prefetch scheduled (%1 sessions)").arg(scheduled));
}

void DSHHub::onSessionSelected(const QString& sessionId)
{
	if (sessionId.isEmpty())
		return;

	TimingLogger::mark(QStringLiteral("session selected: cache restore begin"));

	// 切换会话时停止旧会话的流式渲染状态，避免旧输出继续污染新会话
	if (m_messageHost)
		m_messageHost->stopStreaming();
	clearInteractionPanels();

	m_sessionId = sessionId;
	// 0.1.5：实时日志事件来mux per-session session/follow 流，
	// 换会话时把跟随流也换过去（快照帧会补齐初始历史，DshApiClient）
	if (m_api)
		m_api->followSession(sessionId);
	// 新会话重新统见过的最seq"（缓存新鲜度判定用）；旧会话观测到的值交
	// MessageHost 写进缓存元数据（缓存内容最新位）
	const int observedLastSeq = m_sessionLastSeq;
	m_sessionLastSeq = 0;
	syncComposerSession();

	if (m_sidebar)
		m_sidebar->workspaceList()->setCurrentSession(sessionId);

	if (m_topBar && m_sidebar)
		m_topBar->setTitle(m_sidebar->workspaceList()->titleForSession(sessionId));
	if (m_topBar)
		m_topBar->setSessionId(sessionId);

	// 消息区整块交给宿主：交缓存旧内容 → 依次尝试 控件缓存/预取页/网络拉取 → 分批构建。
	// 回落游标session/list projections.asOfSeq（follow 快照没来时兜底）
	if (m_messageHost) {
		const int fallbackCursor = m_sidebar
			? m_sidebar->workspaceList()->catalog().asOfSeqFor(sessionId)
			: 0;
		m_messageHost->showSession(sessionId, fallbackCursor, observedLastSeq);
	}
}

void DSHHub::onDeleteSessionRequested(const QString& sessionId)
{
	if (sessionId.isEmpty() || !m_api)
		return;

	const auto ret = QMessageBox::question(
		this,
		tr("删除会话"),
		tr("确定要删除这个会话吗？此操作无法撤销。"),
		QMessageBox::Yes | QMessageBox::No,
		QMessageBox::No);
	if (ret != QMessageBox::Yes)
		return;

	m_api->callMethod(
		QStringLiteral("workspace/archiveSession"),
		SessionCommands::sessionArchive(sessionId),
		[this, sessionId](const QJsonObject& value) {
			qInfo().noquote() << "[DSH Hub] session archived:" << sessionId;

			// 回包给的完整归档集合"：立刻用它更新侧栏，不必等服务端
			// workspace/follow archived 帧（省掉一次往返才可见的延迟）
			if (m_sidebar) {
				const QSet<QString> archived = SessionCatalog::parseArchivedSessionIds(value);
				m_workspaceArchived = QJsonArray::fromStringList(
					QStringList(archived.cbegin(), archived.cend()));
				applyWorkspaceState();
			}

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
				if (m_messageHost)
					m_messageHost->stopStreaming();
				clearInteractionPanels();
				// 当前会话被删除时丢弃消息区当前实例（交给缓存丢弃），
				// 避免继续持有已归档会
				if (m_messageHost)
					m_messageHost->discardCurrent();
				m_sessionId.clear();
				syncComposerSession();
				if (m_topBar)
					m_topBar->setTitle(QStringLiteral("New session"));
				if (m_topBar)
					m_topBar->setSessionId(QString());
			}

			if (m_sidebar && m_api)
				m_sidebar->refreshSessions(m_api);
		},
		[this](const DshApiClient::RpcError& error) {
			qWarning().noquote() << "[DSH Hub] delete session failed:"
				<< error.code << error.message;
			if (m_messageHost->current()) {
				m_messageHost->addSystemMessage(tr("删除会话失败: %1 %2").arg(error.code, error.message));
			}
		});
}

void DSHHub::onClearConversationClicked()
{
	m_sidebar->clearAllSessions(
		m_serverManager->dshHome(),
		[this]() {
			clearInteractionPanels();
			if (m_messageHost) {
				m_messageHost->clearCurrent();
				m_cacheManager.clearAll();
				m_sessionId.clear();
				syncComposerSession();

				if (AgentMessageUnit* agent = m_messageHost->current()->lastAgentUnit())
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

	m_api->callMethod(
		QStringLiteral("session/create"),
		SessionCommands::sessionCreate(QString(), m_defaultAgentPreset),
		[this](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			switchToFreshSession(sid, tr("未命名会话"), /*loadHistory=*/true);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSession(sid, tr("未命名会话"));
				m_sidebar->workspaceList()->setCurrentSession(sid);
			}
		},
		[this](const DshApiClient::RpcError& error) {
			finishInitialization();
			if (m_messageHost->current())
				m_messageHost->addSystemMessage(tr("创建会话失败: %1 %2").arg(error.code, error.message));
		});
}

void DSHHub::handleConnected()
{
	qInfo().noquote() << QStringLiteral("[DSH Hub] connected to DSH");
	TimingLogger::mark(QStringLiteral("WS streams connected (mux + host)"));
	if (m_sidebar && m_api)
		m_sidebar->refreshSessions(m_api);
}

void DSHHub::onInitialSessionReady(const QString& sessionId, const QString& title)
{
	Q_UNUSED(title)
		onSessionSelected(sessionId);
}

void DSHHub::onSessionCreated(const QString& sessionId, const QString& workspaceId)
{
	// 统一入口：取消旧构建/停流式/缓存当前会话/换空列表/绑定并加载新会话
	switchToFreshSession(sessionId, tr("未命名会话"), /*loadHistory=*/true);
	if (!m_defaultAgentPreset.isEmpty() && m_api) {
		m_api->callMethod(QStringLiteral("agentPresets/select"),
			SessionCommands::agentPresetSelect(sessionId, m_defaultAgentPreset), {}, {});
	}

	if (m_sidebar)
		m_sidebar->addCreatedSession(sessionId, workspaceId);
	if (m_sidebar)
		m_sidebar->workspaceList()->setCurrentSession(sessionId);
}

void DSHHub::onNoSessionAvailable()
{
	if (m_sidebar && m_api)
		m_sidebar->createSession(m_api);
}

void DSHHub::onSessionListError(const QString& code, const QString& message)
{
	if (m_messageHost->current())
		m_messageHost->addSystemMessage(QStringLiteral("Session list error: %1 %2").arg(code, message));
	finishInitialization();
}

void DSHHub::onSessionCreateError(const QString& code, const QString& message)
{
	if (m_messageHost->current())
		m_messageHost->addSystemMessage(QStringLiteral("Session create error: %1 %2").arg(code, message));
	finishInitialization();
}

void DSHHub::openExtensions()
{
	if (m_extensionPopup)
		return;

	// 遮罩：本窗口上那唯一一层半透明控件（铺满内容区、不含自绘标题栏，否则
	// 窗口按钮会被一起盖住点不动）。showOverlay() 里带一次同步重绘，所以
	// 下面直接 show() 弹窗就行，不会再出现"弹窗先出、遮罩后到"。
	WindowFrame::showOverlay(this, this);

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
			// 移除扩展前先卸载该扩展的 DLL，释放文件占用（其它扩展不受影响
			if (m_dllCaller && !m_dllCaller->removeExtension(name)) {
				qWarning() << "[DSH DllCaller] removeExtension failed:" << name
					<< m_dllCaller->errorString();
			}
			// 如果移除后服务端因残留配置启动失败，自动清理一
			m_cleanupResidualsAfterServerError = true;
		});
	connect(m_extensionPopup, &PopupWindow::closed, this, [this]() {
		// 先收遮罩、再销毁弹窗：收遮罩那一步会同步重绘一次本窗口，两件事
		// 落在同一帧上（理由见 WindowFrame.h 的 showOverlay/hideOverlay）。
		WindowFrame::hideOverlay(this, this);
		if (m_extensionPopup) {
			m_extensionPopup->deleteLater();
			m_extensionPopup = nullptr;
		}
		});
}

void DSHHub::handlePipeRequest(int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket)
{
	// DLL/COM 调用与响应回投（Worker 线程执行 + GUI 线程发送）收敛
	// ToolRequestDispatcher，见其头文件注释
	ToolRequestDispatcher::dispatch(m_dllCaller, m_pipeBridge, m_toolPool, id, tool, args, socket);
}

/**
 * session/follow 的快照到达：把游标交给历史加载器（唤醒可能挂起的首屏请求），
 * 并直接用快照里的 records 播种首屏——省掉一session/page 往返
 */
void DSHHub::handleSessionSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore)
{
	if (!m_messageHost)
		return;

	qInfo().noquote() << "[DSH Hub] session snapshot sessionId=" << sessionId
		<< "cursor=" << cursor << "records=" << records.size() << "hasMore=" << hasMore;

	m_messageHost->setStreamCursor(cursor);
	m_messageHost->onFollowSnapshot(sessionId, cursor, records, hasMore);
}

/**
 * 快照帧里那份会话投影（projectionMode: all，直接从日志折叠）：
 * 输入区下方那行小灰字的主要来源。
 *
 * 比 session/list 那条路可靠得多 —— 列表行读的是投影缓存里的检查点，
 * 而"只是打开看看、还没附着 Agent"的会话，缓存可能根本没写过或停在很旧的位点。
 */
void DSHHub::handleSessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values)
{
	applySessionProjections(sessionId, asOfSeq, values);
}

/**
 * session/control 的 baseline：活会话的现算投影快照（服务端 registry.snapshot）。
 * 只挑当前会话那一份用。
 */
void DSHHub::handleSessionControlBaseline(const QJsonObject& projectionsBySession)
{
	if (!m_chatInput || m_sessionId.isEmpty())
		return;

	const QJsonObject block = projectionsBySession.value(m_sessionId).toObject();
	if (block.isEmpty())
		return;

	applySessionProjections(m_sessionId,
		block.value(QStringLiteral("asOfSeq")).toInt(),
		block.value(QStringLiteral("values")).toObject());
}

/**
 * session/control 的实时帧：某个会话的某个投影键变了。
 * 只认当前会话的、我们显示要用的那两个 key —— 服务端现算现推，正好是"每步实时跳"。
 */
void DSHHub::handleSessionProjectionChanged(const QString& sessionId, const QString& key,
	const QJsonValue& value, int seq)
{
	if (!m_chatInput || sessionId != m_sessionId)
		return;

	if (key != QStringLiteral("sessionStats") && key != QStringLiteral("tokenUsage"))
		return;

	QJsonObject values;
	values.insert(key, value);
	applySessionProjections(sessionId, seq, values);
}

/**
 * 把一份 session/list 行或 follow 快照里的 projections.values 并到小灰字上。
 *
 * 两块投影分开记：哪个来源带了哪块就更新哪块，没带的那块保持原样。
 * （早先整包覆盖时，只有 tokenUsage 的列表行会把快照带来的"轮/步 + 耗时"那段擦掉。）
 */
void DSHHub::applySessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values)
{
	if (!m_chatInput || sessionId != m_sessionId)
		return;

	// 服务端那套 higher-seq-wins：比已经并进来的还旧就丢掉（列表行的检查点常常很旧）
	if (asOfSeq < m_statsAsOfSeq)
		return;
	m_statsAsOfSeq = asOfSeq;

	const QJsonValue sessionStats = values.value(QStringLiteral("sessionStats"));
	if (sessionStats.isObject())
		m_sessionStatsBlock = sessionStats.toObject();

	const QJsonValue tokenUsage = values.value(QStringLiteral("tokenUsage"));
	if (tokenUsage.isObject())
		m_tokenUsageBlock = tokenUsage.toObject();

	// 合并后整包重算：控件的拼行只看结果，不关心数据是哪一次来的
	QJsonObject merged;
	if (!m_sessionStatsBlock.isEmpty())
		merged.insert(QStringLiteral("sessionStats"), m_sessionStatsBlock);
	if (!m_tokenUsageBlock.isEmpty())
		merged.insert(QStringLiteral("tokenUsage"), m_tokenUsageBlock);

	const SessionUsageStats stats = parseSessionUsage(merged);

	// 全 0（新会话、或投影还没送出任何事件）也照样交给控件：
	// 控件那条小灰字是"始终显示"的，全 0 时显示 0 轮 · 0 步 | 输入 0 tok · 输出 0 tok，
	// 而不是把整行藏掉（见 SessionStatsLine::formatStats 的说明）。
	qInfo().noquote() << "[DSH Hub] session stats applied: turns=" << stats.turns
		<< "steps=" << stats.steps << "llmMs=" << stats.llmMs << "toolMs=" << stats.toolMs
		<< "ttftSteps=" << stats.ttftSteps << "hasUsage=" << stats.hasUsage
		<< "asOfSeq=" << asOfSeq;
	m_chatInput->setSessionStats(stats);
}

/** workspace/follow baseline：全量工作区 + 归档集合*/
void DSHHub::handleWorkspaceSnapshot(const QJsonArray& items, const QJsonArray& archivedSessionIds)
{
	qInfo().noquote() << "[DSH Hub] workspace baseline items=" << items.size()
		<< "archived=" << archivedSessionIds.size();
	m_workspaceItems = items;
	m_workspaceArchived = archivedSessionIds;
	applyWorkspaceState();
}

/** workspace/follow upsert：同 id 替换，否则追加*/
void DSHHub::handleWorkspaceUpserted(const QJsonObject& workspace)
{
	const QString workspaceId = workspace.value(QStringLiteral("workspaceId")).toString();
	if (workspaceId.isEmpty())
		return;

	bool replaced = false;
	for (int i = 0; i < m_workspaceItems.size(); ++i) {
		const QString existing = m_workspaceItems.at(i).toObject()
			.value(QStringLiteral("workspaceId")).toString();
		if (existing == workspaceId) {
			m_workspaceItems.replace(i, workspace);
			replaced = true;
			break;
		}
	}
	if (!replaced)
		m_workspaceItems.append(workspace);

	applyWorkspaceState();
}

/** workspace/follow remove*/
void DSHHub::handleWorkspaceRemoved(const QString& workspaceId)
{
	if (workspaceId.isEmpty())
		return;

	QJsonArray kept;
	for (const auto& item : m_workspaceItems) {
		if (item.toObject().value(QStringLiteral("workspaceId")).toString() != workspaceId)
			kept.append(item);
	}
	m_workspaceItems = kept;
	applyWorkspaceState();
}

/** workspace/follow archived：归档集合整体替换*/
void DSHHub::handleWorkspaceArchiveChanged(const QJsonArray& archivedSessionIds)
{
	m_workspaceArchived = archivedSessionIds;
	applyWorkspaceState();
}

/** workspace/follow order：按服务端给的完整顺序重排本地缓存（拖拽排序）*/
void DSHHub::handleWorkspaceReordered(const QStringList& workspaceIds)
{
	if (workspaceIds.isEmpty())
		return;

	QJsonArray ordered;
	QSet<QString> used;
	for (const QString& workspaceId : workspaceIds) {
		for (const auto& item : m_workspaceItems) {
			const QJsonObject workspace = item.toObject();
			if (workspace.value(QStringLiteral("workspaceId")).toString() != workspaceId)
				continue;
			ordered.append(workspace);
			used.insert(workspaceId);
			break;
		}
	}

	// 服务端没点名的（例如upsert、order 还没更新）接在后面，避免丢工作区
	for (const auto& item : m_workspaceItems) {
		const QString workspaceId = item.toObject()
			.value(QStringLiteral("workspaceId")).toString();
		if (!used.contains(workspaceId))
			ordered.append(item);
	}

	qInfo().noquote() << "[DSH Hub] workspace order update ids=" << workspaceIds.size();
	m_workspaceItems = ordered;
	applyWorkspaceState();
}

/** 把缓存的工作归档状态推给侧catalog，并重建会话列表视图*/
void DSHHub::applyWorkspaceState()
{
	if (!m_sidebar)
		return;

	SessionCatalog& catalog = m_sidebar->workspaceList()->catalog();

	QSet<QString> archived;
	for (const auto& value : m_workspaceArchived) {
		const QString sessionId = value.toString();
		if (!sessionId.isEmpty())
			archived.insert(sessionId);
	}

	catalog.setArchivedSessionIds(archived);
	catalog.setWorkspaces(m_workspaceItems);
	m_sidebar->workspaceList()->rebuildFromCatalog();

	// 重建会丢当前选中"的视觉状态，按当前会话再标一
	if (!m_sessionId.isEmpty())
		m_sidebar->workspaceList()->setCurrentSession(m_sessionId);
}

void DSHHub::handleMuxFrame(const QJsonObject& frame)
{
	const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
	const QString type = payload.value(QStringLiteral("type")).toString();
	const QString frameSessionId = payload.value(QStringLiteral("sessionId")).toString();

	if (type == QStringLiteral("session/event")) {
		const QJsonObject event = payload.value(QStringLiteral("event")).toObject();
		// 记录本会话见过的最seq：缓存快照用它判断缓存是否已被后来事件超
		const int eventSeq = event.value(QStringLiteral("seq")).toInt();
		if (eventSeq > m_sessionLastSeq)
			m_sessionLastSeq = eventSeq;
		// 只渲染当前会话的事件：防止在 A 会话输出时切B 会话
		// A 的流式内容错误地显示B 里。（0.1.5 只跟随当前会话，所以这里的
		// "别的会话的帧"正常不会出现；真出现也直接丢弃。）
		if (!frameSessionId.isEmpty() && frameSessionId != m_sessionId)
			return;
		if (!m_messageHost)
			return;

		// “事件 → 气泡内容”的解析、streaming 标志 / 节流定时器 / 输入区按钮状态
		// 都在 MessageHost::onStreamEvent 里；这里只管窗口侧的事（会话标题刷新）
		if (m_messageHost->onStreamEvent(event) == MessageHost::StreamOutcome::Finished) {
			// 一次对话完成后，刷新会话标题（如果服务端已经生成了标题
			if (m_sidebar && m_api)
				m_sidebar->workspaceList()->refreshTitles(m_api);
			// 小灰字不用在这里补：本轮产生的投影变化由 session/control 流实时推过来
		}
	}
	else if (type == QStringLiteral("question/requested")) {
		QWidget* panel = InteractionHandler::handleQuestion(frame, m_api, m_messageHost->layout());
		if (panel) {
			m_interactionPanels.append(panel);
			m_messageHost->scrollToBottomNow();
			connect(panel, &QObject::destroyed, this, [this, panel]() {
				m_interactionPanels.removeAll(panel);
				});
		}
		else if (m_messageHost->current()) {
				m_messageHost->addSystemMessage(tr("收到提问请求，但无法创建内联面板。"));
		}
	}
	else if (type == QStringLiteral("approval/requested")) {
		QWidget* panel = InteractionHandler::handleApproval(frame, m_api, m_messageHost->layout());
		if (panel) {
			m_interactionPanels.append(panel);
			m_messageHost->scrollToBottomNow();
			connect(panel, &QObject::destroyed, this, [this, panel]() {
				m_interactionPanels.removeAll(panel);
				});
		}
		else if (m_messageHost->current()) {
				m_messageHost->addSystemMessage(tr("收到审批请求，但无法创建内联面板。"));
		}
	}
}

void DSHHub::handleTransportError(const QString& context, const QString& message)
{
	// 服务端主动重启时，旧 WebSocket 断开是预期行为，不当作错误刷到聊天区
	if (m_serverManager && m_serverManager->isRestarting())
		return;

	m_messageHost->addSystemMessage(tr("传输错误 [%1]: %2").arg(context, message));
}
