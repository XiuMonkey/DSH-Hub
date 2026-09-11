#include "DSHHub.h"
#include "ServerManager.h"
#include "SessionCommands.h"
#include "ThemeManager.h"
#include "ToolRequestDispatcher.h"
#include "ChatInputWidget.h"
#include "Sidebar.h"
#include "TimingLogger.h"

#include "DshApiClient.h"
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
#include "MessageQuery.h"
#include "LoadMoreButton.h"

#include <QCoreApplication>
#include <QEventLoop>
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

#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>

#include <QScrollBar>
#include <QScrollArea>

#include <QProcess>


#include <QTimer>
#include <QUrl>
#include <QLocalSocket>
#include <QThreadPool>

#include "SettingsStore.h"

DSHHub::DSHHub(QWidget* parent, const QUrl& initialBaseUrl, QProcess* initialServerProcess)
	: QMainWindow(parent)
	, m_api(new DshApiClient(this))
{
	qInfo().noquote() << QStringLiteral("[DSH Hub] constructor started");
	TimingLogger::mark(QStringLiteral("DSHHub ctor enter"));

	// 服务端 spawn 提前到构造函数最前：Node 进程启动（0.9~1.6s）与下面
	// 的扩展加载 / UI 构建 / 首帧真正并行，缩短初始化墙钟时间。
	// ServerManager::start 会同步填充 dshHome，因此之后创建的
	// Settings/PluginsManager 仍可正常使用它。
	m_serverManager = new ServerManager(this);
	connect(m_serverManager, &ServerManager::baseUrlReady, this, [this](const QUrl& url) {
		TimingLogger::mark(QStringLiteral("server baseUrl ready -> open WS streams"));
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

	// DLL/COM 工具调用线程池：请求在 Worker 上执行（GUI 不阻塞）；
	// DllCaller 内部按 DLL 串行、跨 DLL 并行。
	m_toolPool = new QThreadPool(this);
	m_toolPool->setMaxThreadCount(4);

	// 无边框窗口：系统标题栏与边框全部由自绘替代 —— 标题栏见 TitleBar，
	// 圆角 + 1px 描边由 #dshhubCentral 的 QSS 画（见 base.qss），
	// 窗口本体透明，圆角之外什么都不画。
	// 必须在原生窗口创建之前设置，否则会触发窗口重建。
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

	TimingLogger::mark(QStringLiteral("extension DLLs loaded"));

	buildUi();
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
	connect(m_streamTimer, &QTimer::timeout, this, &DSHHub::flushStreamingFrame);

	// 信号槽
	connect(m_chatInput, &ChatInputWidget::sendRequested, this, &DSHHub::onSendClicked);
	connect(m_chatInput, &ChatInputWidget::stopRequested, this, &DSHHub::onStopRequested);
	connect(m_chatInput, &ChatInputWidget::thinkingDepthChanged, this, [](const QString& levelId) {
		qInfo().noquote() << QStringLiteral("[DSH Hub] thinking depth ->")
			<< (levelId.isEmpty() ? QStringLiteral("(默认)") : levelId);
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
	connect(m_historyLoader, &HistoryLoader::firstHistoryArrived,
		this, &DSHHub::finishInitialization);
	connect(m_historyLoader, &HistoryLoader::loadMoreButtonVisibleChanged,
		this, &DSHHub::onHistoryLoadMoreButtonVisibleChanged);
	connect(m_historyLoader, &HistoryLoader::historyError,
		this, &DSHHub::onHistoryError);
	connect(m_historyLoader, &HistoryLoader::incrementalBuildReady,
		this, &DSHHub::onIncrementalBuildReady);
	connect(m_historyLoader, &HistoryLoader::noMoreHistory, this, [this]() {
		// 弹“没有更多了”toast；按钮保持原样不隐藏不改文案，
		// 避免 hide/显隐造成整列消息重排重绘。
		showNoMoreToast();
		});
	connect(m_loadMoreButton, &QPushButton::clicked, m_historyLoader, &HistoryLoader::loadMore);

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
			m_api->callMethod(QStringLiteral("agentPreset.select"),
				SessionCommands::agentPresetSelect(m_sessionId, presetId), {}, {});
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

	TimingLogger::mark(QStringLiteral("DSHHub ctor done (server spawned / UI ready)"));
}

void DSHHub::resizeEvent(QResizeEvent* event)
{
	QMainWindow::resizeEvent(event);

	// 窗口尺寸变了就重算一次边框状态（含跨显示器/DPI 变化时的工作区补偿）
	syncWindowFrameStyle();

	if (m_initOverlay)
		m_initOverlay->setGeometry(rect());

	if (m_settings)
		m_settings->syncOverlayToHost();

	if (m_pluginsManager)
		m_pluginsManager->syncOverlayToHost();
	if (m_extensionOverlay)
		m_extensionOverlay->setGeometry(WindowFrame::overlayRect(this));

	// 宿主缩放后把打开的弹窗重新居中
	keepOpenPopupsCentered();
}

void DSHHub::moveEvent(QMoveEvent* event)
{
	QMainWindow::moveEvent(event);
	// 弹窗是宿主“拥有的”独立窗口（Windows 上不随宿主拖动），这里手动跟随
	keepOpenPopupsCentered();
}

// ------------------------------------------------------------------
// 无边框窗口（自绘圆角边框 + 自绘标题栏）
// ------------------------------------------------------------------

void DSHHub::showEvent(QShowEvent* event)
{
	QMainWindow::showEvent(event);
	// 原生窗口到这一刻才真正存在，补样式位只能在这里做（逻辑见 common/WindowFrame）
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
// 边框的“界面收尾”：三步都在 common/WindowFrame 里判定，这里只负责按顺序调用。
// 画的部分见 resources/styles/base.qss 的 #dshhubCentral。
// ------------------------------------------------------------------
void DSHHub::syncWindowFrameStyle()
{
	QWidget* surface = centralWidget();
	if (!surface)
		return;

	const bool edgeToEdge = WindowFrame::isEdgeToEdge(this);
	// 贴屏幕边缘时切掉圆角与描边（QSS 的 [maximized="true"] 规则）
	WindowFrame::applyBorderState(surface, edgeToEdge);
	// 最大化时系统给的矩形比工作区大一圈，补进内容边距
	WindowFrame::applyMaximizedContentInset(this, surface->layout());
	// 标题栏中间的按钮在“最大化/还原”之间换图标
	if (m_titleBar)
		m_titleBar->setMaximizedState(edgeToEdge);
}

bool DSHHub::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
	// 无边框窗口的原生消息判定全在 common/WindowFrame（含 WM_NCCALCSIZE 让客户区
	// 铺满窗口、WM_NCHITTEST 判缩放热区与标题栏拖动区）；这里只是转交。
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

	m_api->callMethod(
		QStringLiteral("session.cancel"),
		SessionCommands::sessionCancel(m_sessionId),
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

void DSHHub::syncComposerSession()
{
	// 输入区底部的“思考深度”控件按当前会话的模型目录刷新；
	// 会话为空（尚未创建/已被删除）时控件自行隐藏
	if (m_chatInput)
		m_chatInput->setThinkingSession(m_api, m_sessionId);
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

	m_api->callMethod(
		QStringLiteral("workspace.create"),
		SessionCommands::workspaceCreate(path),
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
	m_api->callMethod(
		QStringLiteral("session.create"),
		SessionCommands::sessionCreate(workspaceId, m_defaultAgentPreset),
		[this, workspaceId](const QJsonObject& value) {
			const QString newSessionId = value.value(QStringLiteral("sessionId")).toString();
			if (newSessionId.isEmpty())
				return;

			switchToFreshSession(newSessionId, QStringLiteral("未命名会话"), /*loadHistory=*/true);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSessionToWorkspace(newSessionId, QStringLiteral("未命名会话"), workspaceId);
				m_sidebar->workspaceList()->setCurrentSession(newSessionId);
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

	m_api->callMethod(
		QStringLiteral("session.prompt"),
		SessionCommands::sessionPrompt(m_sessionId, text),
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

	m_api->callMethod(
		QStringLiteral("session.create"),
		SessionCommands::sessionCreate(QString(), m_defaultAgentPreset),
		[this, text](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			// 不 load 历史：等首条 prompt 的 mux 事件即可；adoptSession 让
			// loader 绑定新会话，之后"加载更多"不会误用旧会话 id。
			switchToFreshSession(sid, QStringLiteral("未命名会话"), /*loadHistory=*/false);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSession(sid, QStringLiteral("未命名会话"));
				m_sidebar->workspaceList()->setCurrentSession(sid);
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
	const QString cachedSessionId = m_sessionId;
	m_cacheManager.cacheOrDiscardCurrentSession(m_sessionId, m_messages, m_messagesLayout);
	// 把当前分页状态（原始事件数 / 是否有更早内容）一并快照进缓存，
	// 恢复该会话时可直接播种、跳过重拉与二次渲染。
	if (!cachedSessionId.isEmpty()) {
		m_cacheManager.storeCacheMeta(cachedSessionId,
			m_history.eventCount(),
			m_history.hasMore());
	}
	m_messages = nullptr;
}

void DSHHub::switchToFreshSession(const QString& sessionId, const QString& title, bool loadHistory)
{
	// 与 onSessionSelected 切换语义保持一致：先取消上一个会话未完成的
	// 增量/老页构建，停流式与交互面板，避免旧状态污染新会话。
	if (m_historyLoader)
		m_historyLoader->cancelBuild();
	m_streaming = false;
	if (m_streamTimer)
		m_streamTimer->stop();
	updateStreamingUi();
	clearInteractionPanels();

	cacheCurrentMessages();
	m_messages = new MessageQuery;
	m_sessionId = sessionId;
	syncComposerSession();

	if (m_topBar)
		m_topBar->setTitle(title);

	m_history.reset();
	m_usingPrefetched = false;
	if (m_loadMoreButton)
		m_loadMoreButton->hide();

	if (m_historyLoader) {
		m_historyLoader->setMessages(m_messages);
		m_historyLoader->setUsingPrefetched(false);
		if (loadHistory)
			m_historyLoader->load(sessionId);
		else
			m_historyLoader->adoptSession(sessionId);
	}
}

bool DSHHub::tryRestoreCachedMessages(const QString& sessionId)
{
	const bool dirty = m_cacheManager.isDirtyCache(sessionId);
	int cachedRawCount = 0;
	bool cachedHasMore = false;
	// 快照必须先于 restore 取出：restoreCachedSession(take) 会清掉缓存快照
	const bool haveMeta = m_cacheManager.takeCacheMeta(sessionId, &cachedRawCount, &cachedHasMore);
	m_messages = m_cacheManager.restoreCachedSession(sessionId, m_messagesLayout);
	if (!m_messages)
		return false;

	TimingLogger::mark(QStringLiteral("cache hit -> instant restore (dirty=%1)")
		.arg(dirty ? QStringLiteral("true") : QStringLiteral("false")));

	if (m_loadMoreButton)
		m_loadMoreButton->setVisible(!m_messages->messages.empty());

	// 恢复缓存后把当前列表同步给 HistoryLoader，避免 loader 停留在上一个
	// （可能已删除/被缓存接管）会话的对象上。
	if (m_historyLoader)
		m_historyLoader->setMessages(m_messages);

	scrollToBottomNow();

	if (dirty) {
		// 缓存建立之后有后台事件流入：内容确实过期，必须重拉重建。
		// 这是唯一需要"预览+重建"双渲染的场景，且重建内容必然不同。
		TimingLogger::mark(QStringLiteral("dirty cache -> full history reload begin"));
		m_history.reset();
		if (m_historyLoader)
			m_historyLoader->load(sessionId);
	}
	else {
		// 缓存与重拉请求是同一个 maxMessages 尾窗口，且无后台变更：
		// 内容一致，跳过重拉与二次渲染；用快照播种分页状态即可，
		// "加载更多"仍可正常使用。
		if (!haveMeta) {
			// 兜底（无快照的旧缓存）：按可见内容估算，不显示"加载更多"
			cachedRawCount = static_cast<int>(m_messages->messages.size());
			cachedHasMore = false;
		}
		TimingLogger::mark(QStringLiteral("cache (clean) -> skip reload, seed pagination (rawCount=%1 hasMore=%2)")
			.arg(cachedRawCount)
			.arg(cachedHasMore ? QStringLiteral("true") : QStringLiteral("false")));

		m_history.reset();
		m_history.setEventCount(cachedRawCount);
		m_history.setHasMore(cachedHasMore);
		m_usingPrefetched = false;
		if (m_historyLoader) {
			m_historyLoader->cancelBuild();
			m_historyLoader->setUsingPrefetched(false);
			m_historyLoader->adoptSession(sessionId);
		}
		onHistoryLoadMoreButtonVisibleChanged(cachedHasMore);
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

	// 关键：消息列表被整体替换（旧对象已 delete）后，必须同步 HistoryLoader，
	// 否则 loader 仍持有指向已释放 MessageQuery 的悬垂指针，加载更多/历史回调
	// 会在 prepend/insert 时崩溃（访问已释放的 messages 容器）。
	if (m_historyLoader)
		m_historyLoader->setMessages(m_messages);

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

	fitContentThenLayout();

	// 等 Qt 完成本轮布局/事件处理后，再真正滚动并恢复刷新
	QTimer::singleShot(0, this, [this]() {
		if (!m_scrollArea)
			return;

		fitContentThenLayout();

		if (m_scrollArea->verticalScrollBar())
			m_scrollArea->verticalScrollBar()->setValue(
				m_scrollArea->verticalScrollBar()->maximum());

		m_scrollToBottomScheduled = false;
		m_scrollArea->setUpdatesEnabled(true);
		m_scrollArea->viewport()->update();
		});
}

// 流式渲染的一帧：本帧先暂停重绘，等拟合/布局落定后再一次性画出来。
//
// flushStream() 每帧都会重建 live 区域：新视图刚创建时宽度/高度还是 Qt 默认值，
// 布局也还没铺完，这些中间态一旦被画到屏幕上，看起来就是气泡上下抖动。
// 以前的代码只在“贴近底部自动跟随”时才走 scrollToBottomNow()（它顺带做了暂停
// 重绘 + 落定后刷新），所以用户上拉离开底部后就没有这层保护，抖动也就随之出现。
void DSHHub::flushStreamingFrame()
{
	AgentMessageUnit* target = m_messages ? m_messages->lastAgentUnitIfLast() : nullptr;
	if (!target) {
		// 没有可渲染的气泡时别把刷新窗口挂着不放
		if (m_scrollArea)
			m_scrollArea->setUpdatesEnabled(true);
		return;
	}

	if (m_scrollArea)
		m_scrollArea->setUpdatesEnabled(false);

	target->flushStream();

	bool followBottom = false;
	if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
		QScrollBar* bar = m_scrollArea->verticalScrollBar();
		followBottom = bar->value() >= bar->maximum() - 80;
	}

	if (followBottom) {
		// 跟随底部：scrollToBottomNow() 自己会恢复刷新并滚到最新底部
		scrollToBottomNow();
	}
	else {
		// 不跟随（用户正在读旧内容）：只把布局落定 + 恢复刷新，绝不改滚动位置
		settleStreamingFrame();
	}
}

// 消息列（滚动区里的内容控件）必须“先长够高度，再铺布局”：
// 直接 layout()->activate() 会在内容控件仍是上一帧的旧高度时给子部件分配几何，
// 于是刚刚变高的气泡被就地挤扁（子部件从 2000+ px 压到几十 px），表现为流式输出
// 时后半段内容被吞掉、滚动条范围也随之缩水、拉不到底；下一轮布局再撑开，如此反复
// 就成了闪烁。这里先按布局需要的尺寸把内容控件撑够，再执行布局。
void DSHHub::fitContentThenLayout()
{
	if (!m_scrollArea)
		return;

	QWidget* content = m_scrollArea->widget();
	if (!content)
		return;

	QLayout* contentLayout = content->layout();
	if (!contentLayout)
		return;

	contentLayout->invalidate();
	const int wanted = qMax(m_scrollArea->viewport()->height(), content->sizeHint().height());
	if (content->height() < wanted)
		content->resize(content->width(), wanted);

	contentLayout->activate();
}

// 流式渲染的“落定”收尾（不跟随底部时使用）：等本帧排队的二次拟合、布局都跑完，
// 再把这一帧一次性画出来，并且**不动滚动位置**（用户正在读旧内容）。
void DSHHub::settleStreamingFrame()
{
	QTimer::singleShot(0, this, [this]() {
		if (!m_scrollArea)
			return;

		fitContentThenLayout();

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
		// HistoryLoader 正在拉全量历史/增量构建时不要重复塞预取，
		// 避免同一份历史被渲染两遍（预取只用于给空列表快速垫底）
		if (m_historyLoader && m_historyLoader->isLoading())
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

		// 只预构建少量预取消息，控件树很小，避免初始化时卡顿；
		// 缓存建立时记录分页快照，恢复该会话时可直接播种、跳过重拉。
		MessageQuery* query = MessageQuery::fromEvents(events);
		m_cacheManager.cacheSessionMessages(sessionId, query);
		m_cacheManager.storeCacheMeta(sessionId, events.size(), events.size() >= 20);
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

	TimingLogger::mark(QStringLiteral("session selected: cache restore begin"));

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
	syncComposerSession();

	if (m_sidebar)
		m_sidebar->workspaceList()->setCurrentSession(sessionId);

	if (m_topBar && m_sidebar)
		m_topBar->setTitle(m_sidebar->workspaceList()->titleForSession(sessionId));

	if (tryRestoreCachedMessages(sessionId))
		return;

	TimingLogger::mark(QStringLiteral("cache miss -> history fetch begin"));

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

	m_api->callMethod(
		QStringLiteral("workspace.archiveSession"),
		SessionCommands::sessionArchive(sessionId),
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
				syncComposerSession();
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
			syncComposerSession();

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

	m_api->callMethod(
		QStringLiteral("session.create"),
		SessionCommands::sessionCreate(QString(), m_defaultAgentPreset),
		[this](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			switchToFreshSession(sid, QStringLiteral("未命名会话"), /*loadHistory=*/true);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSession(sid, QStringLiteral("未命名会话"));
				m_sidebar->workspaceList()->setCurrentSession(sid);
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
	TimingLogger::mark(QStringLiteral("WS streams connected (mux + host)"));
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
	// 统一入口：取消旧构建/停流式/缓存当前会话/换空列表/绑定并加载新会话
	switchToFreshSession(sessionId, QStringLiteral("未命名会话"), /*loadHistory=*/true);
	if (!m_defaultAgentPreset.isEmpty() && m_api) {
		m_api->callMethod(QStringLiteral("agentPreset.select"),
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
	if (!m_loadMoreButton)
		return;

	// 到“历史顶部”不隐藏也不改样式：只要列表里有内容，按钮就保持
	// 原样“加载更多”（置灰/文案变化都会带来额外重绘，且用户不需要该提示）；
	// 只有列表为空（新会话/加载中）时才真正 hide——那时布局里没有内容可重排。
	const bool hasContent = m_messages && !m_messages->messages.empty();
	if (visible || hasContent) {
		m_loadMoreButton->show();
		m_loadMoreButton->setEnabled(true);
		if (m_loadMoreButton->text() != QStringLiteral("加载更多"))
			m_loadMoreButton->setText(QStringLiteral("加载更多"));
	}
	else {
		m_loadMoreButton->hide();
	}
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
	TimingLogger::mark(QStringLiteral("history built -> swap into UI"));
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
	m_extensionOverlay->setGeometry(WindowFrame::overlayRect(this));
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
	// DLL/COM 调用与响应回投（Worker 线程执行 + GUI 线程发送）收敛在
	// ToolRequestDispatcher，见其头文件注释。
	ToolRequestDispatcher::dispatch(m_dllCaller, m_pipeBridge, m_toolPool, id, tool, args, socket);
}

void DSHHub::handleMuxFrame(const QJsonObject& frame)
{
	const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
	const QString type = payload.value(QStringLiteral("type")).toString();
	const QString frameSessionId = payload.value(QStringLiteral("sessionId")).toString();

	if (type == QStringLiteral("session/event")) {
		const QJsonObject event = payload.value(QStringLiteral("event")).toObject();
		// 防止在 A 会话输出时切到 B 会话，A 的流式内容错误地显示到 B 里；
		// 同时把 A 的缓存标记为 dirty（若有），切回 A 时重新拉取历史，
		// 不丢失后台输出、也不用旧缓存冒充最新内容。
		if (!frameSessionId.isEmpty() && frameSessionId != m_sessionId) {
			m_cacheManager.markDirtyCache(frameSessionId);
			return;
		}
		// “事件 → 气泡内容”的解析与更新下沉到 MessageQuery::applyStreamEvent，
		// 这里只保留控制器策略：streaming 标志 / 节流定时器 / 标题刷新。
		if (!m_messages)
			return;

		const MessageQuery::StreamFrameResult streamResult =
			m_messages->applyStreamEvent(event, m_messagesLayout, m_streaming);

		if (streamResult.kind == MessageQuery::StreamFrameResult::FinalMessage) {
			// assistant/message 收尾：结束流式渲染状态（内容已在内部合并/封存）
			if (m_streaming) {
				m_streaming = false;
				if (m_streamTimer)
					m_streamTimer->stop();
				updateStreamingUi();
			}
			// 一次对话完成后，刷新会话标题（如果服务端已经生成了标题）
			if (m_sidebar && m_api)
				m_sidebar->workspaceList()->refreshTitles(m_api);
		}
		else if (streamResult.kind == MessageQuery::StreamFrameResult::Streaming) {
			// 收到流式内容（chunk/tool…）：进入流式态，节流 50ms 批量重渲染
			m_streaming = true;
			if (m_streamTimer && !m_streamTimer->isActive())
				m_streamTimer->start();
			updateStreamingUi();
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