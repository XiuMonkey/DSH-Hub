#include "core/DSHHub.h"
#include "core/ServerManager.h"
#include "core/ConnectionManager.h"
#include "core/HostExports.h"
#include "common/util/CommonRegistry.h"
#include "common/session/SessionCommands.h"
#include "common/appearance/ThemeManager.h"
#include "core/ToolRequestDispatcher.h"
#include "ui/ChatInputWidget.h"
#include "ui/Sidebar.h"
#include "common/util/Logger.h"
#include "network/DshApiClient.h"
#include "core/MessageHost.h"
#include "network/SessionPrefetcher.h"
#include "common/util/CodeHighlighter.h"
#include "ui/TopBar.h"
#include "ui/TitleBar.h"
#include "common/appearance/WindowFrame.h"
#include "ui/Settings.h"
#include "ui/PluginsManager.h"
#include "ExtensionSystem/DshNamedPipeBridge.h"
#include "ExtensionSystem/DllCaller.h"
#include "ExtensionSystem/ExtensionDllLoader.h"
#include "ExtensionSystem/ClientExtension.h"
#include "common/session/SessionProjectionState.h"
#include "ui/ExtensionManagerPopup.h"
#include "chat/AgentMessageUnit.h"
// 要调它的成员函数，需完整类型，不能只靠前置声明
#include "chat/MessageQuery.h"

#include <QCoreApplication>
#include <QMoveEvent>
#include <QShowEvent>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QJsonArray>
#include <QMessageBox>
#include <QResizeEvent>
#include <QProcess>
#include <QUrl>
#include <QUrlQuery>
#include <QLocalSocket>
#include <QThreadPool>

DSHHub::DSHHub(QWidget* parent, const QUrl& initialBaseUrl, QProcess* initialServerProcess)
	: QMainWindow(parent)
	, m_api(new DshApiClient(this))
{
	qInfo().noquote() << QStringLiteral("[DSH Hub] constructor started");
	TimingLogger::mark(QStringLiteral("DSHHub ctor enter"));

	// 顺序约束：installWindowShell 最早（无边框标志要在原生窗口创建前设）、registerHostObjects 最后
	installWindowShell();
	installServer(initialBaseUrl, initialServerProcess);
	installToolRuntime();
	buildUi();
	loadHighlightRules();
	installInputWiring();
	installSidebarWiring();
	installApiWiring();
	installMessageWiring();
	installSettingsAndPlugins();

	TimingLogger::mark(QStringLiteral("DSHHub ctor done (server spawned / UI ready)"));
	registerHostObjects();
}

// ⚠️ 必须第一步：无边框标志只能在原生窗口创建之前设
void DSHHub::installWindowShell()
{
	// 标题栏与边框全自绘，窗口本体透明
	setObjectName(QStringLiteral("dshHubWindow"));
	setWindowFlag(Qt::FramelessWindowHint, true);
	setAttribute(Qt::WA_TranslucentBackground, true);

	// 按窗口安装样式表，覆盖本窗口与其子控件
	ThemeManager::instance().applyToWindow(this);
	setWindowTitle(QStringLiteral("DSH Hub"));
	setAttribute(Qt::WA_DeleteOnClose);
}

// ⚠️ spawn 要最前：Node 启动 0.9~1.6s，提前起才能与后续步骤并行；start() 同步填充 dshHome
void DSHHub::installServer(const QUrl& initialBaseUrl, QProcess* initialServerProcess)
{
	m_serverManager = new ServerManager(this);
	dshRegister("DSHHub.001", m_serverManager, &ServerManager::baseUrlReady, this,
		[this](const QUrl& url) {
			TimingLogger::mark(QStringLiteral("server baseUrl ready -> open WS streams"));
			if (!m_api)
				return;
			m_api->setBaseUrl(url);
			if (m_pluginsManager) {
				// 传干净 baseUrl：带 ?token= 的那条会让插件市场丢 path
				m_pluginsManager->setBaseUrl(m_api->baseUrl());
			}
			m_api->openStreams();
		});
	dshRegister("DSHHub.002", m_serverManager, &ServerManager::errorLine, this,
		[this](const QString& line) {
			if (m_messageHost && m_messageHost->current())
				m_messageHost->addSystemMessage(qtTrId("server_status_fmt").arg(line));
			// 移除扩展后服务端启动失败：清理 cordis.patch.yml 残留
			if (m_cleanupResidualsAfterServerError && m_extensionPopup) {
				m_cleanupResidualsAfterServerError = false;
				m_extensionPopup->cleanupResiduals();
			}
			if (!isInitializationComplete())
				finishInitialization();
		});
	dshRegister("DSHHub.003", m_serverManager, &ServerManager::outputLine, this,
		[](const QString& line) {
			// 只记与插件市场 / 扩展 / 服务相关的输出（DSH_HUB_SERVER_TRACE=1 可全量转储）
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
	dshRegister("DSHHub.004", m_serverManager, &ServerManager::finished, this,
		[this](int exitCode, QProcess::ExitStatus) {
			if (m_serverManager && m_serverManager->isRestarting())
				return;

			if (m_api && !m_api->isConnected()) {
				if (m_messageHost && m_messageHost->current())
					m_messageHost->addSystemMessage(qtTrId("server_exited_fmt").arg(exitCode));
				if (m_cleanupResidualsAfterServerError && m_extensionPopup) {
					m_cleanupResidualsAfterServerError = false;
					m_extensionPopup->cleanupResiduals();
				}
				if (exitCode != 0 && !isInitializationComplete())
					finishInitialization();
			}
		});

	m_serverManager->start(initialBaseUrl, initialServerProcess);
}

// 工具扩展运行时（跑 Worker 线程、只做 JSON 工具调用），与"客户端扩展"不是一套
void DSHHub::installToolRuntime()
{
	// 请求在 Worker 执行；DllCaller 内部同 DLL 串行、跨 DLL 并行
	m_toolPool = new QThreadPool(this);
	m_toolPool->setMaxThreadCount(4);

	m_pipeBridge = new DshNamedPipeBridge(this);
	dshRegister("DSHHub.005", m_pipeBridge, &DshNamedPipeBridge::requestReceived, this,
		&DSHHub::handlePipeRequest);
	if (!m_pipeBridge->start()) {
		qWarning() << QStringLiteral("[DSH Pipe] failed to start:") << m_pipeBridge->errorString();
	}

	// env 显式指定（调试）或扫描已安装扩展目录
	m_dllCaller = new DllCaller;
	const QString serverProfilePath =
		QCoreApplication::applicationDirPath() + QStringLiteral("/resources/server/harness/profiles/web");
	ExtensionDllLoader::loadAll(m_dllCaller, serverProfilePath);

	TimingLogger::mark(QStringLiteral("extension DLLs loaded"));
}

// 规则文件缺失只记警告
void DSHHub::loadHighlightRules()
{
	if (!CodeHighlighter::instance().loadFromFile(QStringLiteral(":/DSHHub/highlight_rules.json"))) {
		qWarning().noquote() << QStringLiteral("[DSH Hub] 未找到内置资源 highlight_rules.json，代码高亮不可用");
	}
}

void DSHHub::installInputWiring()
{
	dshRegister("DSHHub.006", m_chatInput, &ChatInputWidget::modelChanged, this,
		[](const QString& provider, const QString& model) {
			qInfo().noquote() << QStringLiteral("[DSH Hub] model ->")
				<< QStringLiteral("%1/%2").arg(provider, model);
		});
	dshRegister("DSHHub.007", m_chatInput, &ChatInputWidget::thinkingDepthChanged, this,
		[](const QString& levelId) {
			qInfo().noquote() << QStringLiteral("[DSH Hub] thinking depth ->")
				<< (levelId.isEmpty() ? qtTrId("common_default_suffix") : levelId);
		});
}

// 侧栏接线：新建 / 切会话 / 删除 / 清空 + 设置 / 插件 / 主题 / 扩展 + 会话列表事件
void DSHHub::installSidebarWiring()
{
	dshRegister("DSHHub.008", m_sidebar, &Sidebar::newWorkspaceRequested, this, &DSHHub::onNewWorkspaceClicked);
	dshRegister("DSHHub.009", m_sidebar, &Sidebar::createSessionInWorkspaceRequested, this,
		&DSHHub::onCreateSessionInWorkspace);
	dshRegister("DSHHub.010", m_sidebar, &Sidebar::sessionSelected, this, &DSHHub::onSessionSelected);
	dshRegister("DSHHub.011", m_sidebar, &Sidebar::deleteSessionRequested, this, &DSHHub::onDeleteSessionRequested);
	dshRegister("DSHHub.012", m_sidebar, &Sidebar::clearRequested, this, &DSHHub::onClearConversationClicked);

	// 这里只接线一次；实际创建在 installSettingsAndPlugins（那时 dshHome 才可用）
	dshRegister("DSHHub.025", m_sidebar, &Sidebar::settingsRequested, this,
		[this]() { if (m_settings) m_settings->openSettings(); });
	dshRegister("DSHHub.026", m_sidebar, &Sidebar::pluginsRequested, this,
		[this]() {
			// 接管态再挡一道：信号也可能从别处被触发
			if (m_api && m_api->isTakenover()) {
				qInfo().noquote() << QStringLiteral(
					"[DSH Hub] 后端已被接管：插件市场入口不可用");
				return;
			}
			if (m_pluginsManager)
				m_pluginsManager->openPlugins();
		});
	dshRegister("DSHHub.027", m_sidebar, &Sidebar::themeToggleRequested, this, &DSHHub::toggleTheme);
	dshRegister("DSHHub.028", m_sidebar, &Sidebar::extensionsRequested, this, &DSHHub::openExtensions);
	dshRegister("DSHHub.029", m_sidebar, &Sidebar::initialSessionReady, this, &DSHHub::onInitialSessionReady);
	dshRegister("DSHHub.030", m_sidebar, &Sidebar::sessionsRefreshed, this, &DSHHub::onSessionsRefreshed);
	dshRegister("DSHHub.031", m_sidebar, &Sidebar::sessionCreated, this, &DSHHub::onSessionCreated);
	dshRegister("DSHHub.032", m_sidebar, &Sidebar::noSessionAvailable, this, &DSHHub::onNoSessionAvailable);
	dshRegister("DSHHub.033", m_sidebar, &Sidebar::sessionListError, this, &DSHHub::onSessionListError);
	dshRegister("DSHHub.034", m_sidebar, &Sidebar::sessionCreateError, this, &DSHHub::onSessionCreateError);
}

// DshApiClient → DSHHub 的数据入口
void DSHHub::installApiWiring()
{
	dshRegister("DSHHub.013", m_api, &DshApiClient::connected, this, &DSHHub::handleConnected);
	dshRegister("DSHHub.014", m_api, &DshApiClient::muxFrameReceived, this, &DSHHub::forwardMuxFrame);
	dshRegister("DSHHub.015", m_api, &DshApiClient::sessionSnapshotReady, this, &DSHHub::handleSessionSnapshot);
	// 小灰字：快照的全量投影做种子 + session/control 实时推送做更新
	dshRegister("DSHHub.016", m_api, &DshApiClient::sessionProjectionsReady, this,
		&DSHHub::handleSessionProjections);
	dshRegister("DSHHub.017", m_api, &DshApiClient::sessionProjectionsBaselineReady, this,
		&DSHHub::handleSessionControlBaseline);
	dshRegister("DSHHub.018", m_api, &DshApiClient::sessionProjectionChanged, this,
		&DSHHub::handleSessionProjectionChanged);
	dshRegister("DSHHub.019", m_api, &DshApiClient::workspaceSnapshotReady, this, &DSHHub::handleWorkspaceSnapshot);
	dshRegister("DSHHub.020", m_api, &DshApiClient::workspaceUpserted, this, &DSHHub::handleWorkspaceUpserted);
	dshRegister("DSHHub.021", m_api, &DshApiClient::workspaceRemoved, this, &DSHHub::handleWorkspaceRemoved);
	dshRegister("DSHHub.022", m_api, &DshApiClient::workspaceReordered, this, &DSHHub::handleWorkspaceReordered);
	dshRegister("DSHHub.023", m_api, &DshApiClient::workspaceArchiveChanged, this,
		&DSHHub::handleWorkspaceArchiveChanged);
	dshRegister("DSHHub.024", m_api, &DshApiClient::transportError, this, &DSHHub::handleTransportError);

	// 接管开关：插件调 VirtualApiHost::Takenover 时收回执，只做接管特有的收尾
	dshRegister("DSHHub.045", m_api, &DshApiClient::takeoverChanged, this,
		[this](bool takenover) {
			if (m_serverManager) {
				if (takenover)
					m_serverManager->stopForTakeover();
				else
					m_serverManager->setTakenover(false);
			}
			applyTakeoverBypasses(takenover);
		});
}

// ⚠️ 只关这三条旁路，绝不碰"扩展管理"：接管要靠它把扩展装进来
void DSHHub::applyTakeoverBypasses(bool takenover)
{
	// ① 顶栏工具过滤：走 /api/tools-filter，服务 DSH 服务端那套工具清单
	if (m_topBar)
		m_topBar->setToolsFilterEnabled(!takenover);

	// ② 插件市场入口（cordis 插件）：连已打开的窗口一起收掉
	if (m_sidebar)
		m_sidebar->setPluginsEntryEnabled(!takenover);

	if (takenover && m_pluginsManager)
		m_pluginsManager->closePlugins();

	// ③ 进程那条旁路不在这里关，处置都在 ServerManager::stopForTakeover
	qInfo().noquote() << QStringLiteral("[DSH Hub] 后端接管态") << (takenover ? "已生效" : "已复位")
		<< QStringLiteral("（工具过滤/插件市场入口已按态切换；扩展管理照旧）");
}

// ⚠️ 必须排在 installWindowShell / buildUi 之后（要用它们建好的滚动区与布局）
void DSHHub::installMessageWiring()
{
	// ⚠️ parent 传 nullptr：挂成子对象会先删控件树再删 MessageQuery，clear() 二次释放，
	// 故改由 ~DSHHub 显式删除
	m_messageHost = new MessageHost(m_api, &m_cacheManager, m_scrollArea, m_messagesLayout,
		m_loadMoreButton, m_toastLabel, m_chatInput, nullptr);

	dshRegister("DSHHub.035", m_messageHost, &MessageHost::sendWithoutSession, this,
		&DSHHub::createSessionAndSend);
	// 首屏上屏 -> 收掉启动遮罩
	dshRegister("DSHHub.036", m_messageHost, &MessageHost::contentReady, this,
		&DSHHub::finishInitialization);
	// 整列表被替换 -> 先收面板再同步滚到底，避免先显示顶部再闪烁
	dshRegister("DSHHub.037", m_messageHost, &MessageHost::contentReplaced, this,
		[this]() {
			m_messageHost->clearInteractionPanels();
			m_messageHost->scrollToBottomNow();
		});
	// 对话收尾：刷新侧栏会话标题
	dshRegister("DSHHub.038", m_messageHost, &MessageHost::turnFinished, this,
		[this]() {
			if (m_sidebar && m_api)
				m_sidebar->workspaceList()->refreshTitles(m_api);
		});

	// 并发发一批 session/page，结果入库供"立即点亮"
	m_prefetcher = new SessionPrefetcher(this);
	m_prefetcher->setApi(m_api);
	dshRegister("DSHHub.039", m_prefetcher, &SessionPrefetcher::historyFetched, this,
		[this](const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore) {
			if (!m_messageHost)
				return;
			// 只有真的用它点亮了才记"见过的最新 seq"
			if (m_messageHost->onPrefetched(sessionId, events, throughSeq, hasMore)
				== MessageHost::PrefetchOutcome::Painted) {
				m_messageHost->noteObservedSeq(throughSeq);
			}
		});
	dshRegister("DSHHub.040", m_prefetcher, &SessionPrefetcher::prefetchFailed, this,
		[](const QString& sessionId, const QString& code, const QString& message) {
			qWarning().noquote() << "[DSH Hub] prefetch failed sessionId=" << sessionId
				<< "code=" << code << "message=" << message;
		});
}

// ⚠️ 必须排在 installServer 之后：构造要用 dshHome
void DSHHub::installSettingsAndPlugins()
{
	// 常驻设置系统：自管窗口开关 / 遮罩 / 居中，这里只接线一次
	m_settings = new Settings(m_api, this);
	// 默认预设的写入已在 Settings 完成，客户端不另存一份，建会话时也不带 agentPreset
	dshRegister("DSHHub.041", m_settings, &Settings::agentPresetChanged, this,
		[](const QString& presetId) {
			qInfo().noquote() << QStringLiteral("[DSH Hub] default agent preset ->") << presetId;
		});
	dshRegister("DSHHub.042", m_settings, &Settings::serverSettingsSaved, this,
		[this]() {
			if (m_serverManager)
				m_serverManager->restart();
		});
	// 新增模型后重拉会话目录，刷新输入框底的选择器
	dshRegister("DSHHub.043", m_settings, &Settings::modelAdded, this,
		[this](const QString& provider, const QString& modelId) {
			qInfo().noquote() << QStringLiteral("[DSH Hub] model added ->")
				<< QStringLiteral("%1/%2").arg(provider, modelId);
			if (m_chatInput)
				m_chatInput->refreshModelCatalog();
		});

	// 常驻插件系统：baseUrl 在 baseUrlReady 时更新，重启信号只在此接线一次
	m_pluginsManager = new PluginsManager(m_api ? m_api->baseUrl() : QUrl(), this);
	dshRegister("DSHHub.044", m_pluginsManager, &PluginsManager::serverRestartRequested,
		m_serverManager, &ServerManager::restart);

	// baseUrl 用回调现取（启动 / 重启都会变）
	if (m_topBar) {
		m_topBar->setBaseUrlProvider([this]() { return m_api ? m_api->baseUrl() : QUrl(); });
	}
}

// ⚠️ 装载客户端扩展必须最后：窗口、顶栏 / 侧栏都建好且已登记之后
void DSHHub::registerHostObjects()
{
	// 登记是覆盖语义，侧栏 / 顶栏各自在自己构造函数里登记
	CommonRegistry::instance().AddToRegistry(DshHostIndex::kMainWindow, this);

	// 客户端扩展靠它拿后端接管接口（VirtualApiHost）
	// ⚠️ 切主题会换掉 m_api ⇒ 插件每次 attachHost() 都要重新 findObject + cast
	CommonRegistry::instance().AddToRegistry(DshHostIndex::kApiClient, m_api);

	// 消息区宿主（VirtualMessageHost）：接管态下"正在载入会话"提示要由扩展收掉
	// ⚠️ 与上面两条一样是覆盖语义，切主题时新窗口会顶掉旧登记
	if (m_messageHost)
		CommonRegistry::instance().AddToRegistry(DshHostIndex::kMessageHost, m_messageHost);

	// ⚠️ 与 DllCaller 那条线不是一套：这里在 GUI 线程直接改宿主界面
	const auto clientExtensions = ClientExtension::loadAll();
	if (!clientExtensions.isEmpty()) {
		qInfo().noquote() << QStringLiteral("[DSH Hub] client extensions:")
			<< clientExtensions.join(QStringLiteral(", "));
	}
}

void DSHHub::resizeEvent(QResizeEvent* event)
{
	QMainWindow::resizeEvent(event);

	// 尺寸变了重算边框状态（含跨显示 DPI 的工作区补偿）
	syncWindowFrameStyle();

	if (m_initOverlay)
		m_initOverlay->setGeometry(rect());
	if (m_messageHost)
		m_messageHost->syncLoadingGeometry();
	if (m_settings)
		m_settings->syncOverlayToHost();
	if (m_pluginsManager)
		m_pluginsManager->syncOverlayToHost();
	WindowFrame::syncOverlay(this);  // 扩展管理弹窗的遮罩

	// 把打开的弹窗重新居中
	keepOpenPopupsCentered();
}

void DSHHub::moveEvent(QMoveEvent* event)
{
	QMainWindow::moveEvent(event);
	// 弹窗是宿主的独立窗口，Windows 上不随宿主拖动，需手动跟
	keepOpenPopupsCentered();
}

void DSHHub::showEvent(QShowEvent* event)
{
	QMainWindow::showEvent(event);
	// 原生窗口此刻才存在，补样式位只能在这里做
	WindowFrame::applyNativeStyle(this);
	syncWindowFrameStyle();
}

void DSHHub::changeEvent(QEvent* event)
{
	QMainWindow::changeEvent(event);
	if (event->type() == QEvent::WindowStateChange)
		syncWindowFrameStyle();
}

// 判定与顺序都在 WindowFrame::applyFrameStyle
void DSHHub::syncWindowFrameStyle()
{
	const bool edgeToEdge = WindowFrame::applyFrameStyle(this, centralWidget());
	if (m_titleBar)
		m_titleBar->setMaximizedState(edgeToEdge);
}

bool DSHHub::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
	// 原生消息判定全在 common/WindowFrame，这里只转交
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
		m_topBar->syncOverlayToHost();  // 铺满遮罩并让工具过滤窗口居中
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
		// 新窗口须带启动令牌才能换到 /api 的认证 cookie
		QUrlQuery query(url);
		query.addQueryItem(QStringLiteral("token"), token);
		url.setQuery(query);
	}
	return url;
}

void DSHHub::toggleTheme()
{
	ThemeManager::instance().switchTheme(this);
}

QProcess* DSHHub::takeServerProcess()
{
	return m_serverManager ? m_serverManager->takeProcess() : nullptr;
}

DSHHub::~DSHHub()
{
	// 先摘除登记，免得插件拿到半残对象（Destroy 带身份校验）
	CommonRegistry::instance().Destroy(DshHostIndex::kMainWindow, this);

	// 切主题时新窗口已顶掉这条登记，旧窗口的注销会被拒绝
	if (m_api)
		CommonRegistry::instance().Destroy(DshHostIndex::kApiClient, m_api);

	// 同上：m_messageHost 是本次窗口的对象，注销带身份校验
	if (m_messageHost)
		CommonRegistry::instance().Destroy(DshHostIndex::kMessageHost, m_messageHost);

	// 先停线程池，避免 Worker 仍引用 this
	if (m_toolPool) {
		m_toolPool->clear();
		m_toolPool->waitForDone();
		delete m_toolPool;
		m_toolPool = nullptr;
	}

	if (m_api)
		disconnect(m_api, nullptr, this, nullptr);

	// 必须在这里（控件树仍活着）删：它无父对象，靠 Qt 链会晚于 ~QWidget 而二次释放
	delete m_messageHost;
	m_messageHost = nullptr;

	delete m_dllCaller;

	m_cacheManager.clearAll();
}

void DSHHub::syncComposerSession()
{
	// 按当前会话的模型目录刷新；会话为空时控件自行隐藏
	if (m_chatInput)
		m_chatInput->setModelSession(m_api, m_sessionId);

	// 会话自己的模型选择在 session/list 行的投影里，这里推给 chip
	if (m_chatInput && m_sidebar && !m_sessionId.isEmpty()) {
		QString provider;
		QString model;
		QString effort;
		if (m_sidebar->workspaceList()->catalog().modelSelectionFor(m_sessionId, &provider, &model, &effort))
			m_chatInput->applySessionModelSelection(provider, model, effort);
		else
			m_chatInput->applySessionModelSelection(QString(), QString(), QString());
	}

	// 换会话要整体重置合并态，否则旧会话的数字会留在新会话上
	m_projectionState.reset();
	if (m_chatInput)
		m_chatInput->clearSessionStats();
}

namespace
{
	// 取出小灰字要的两块投影；服务端保证 key 一定在，宽松解析即可
	SessionUsageStats parseSessionUsage(const QJsonObject& projectionValues)
	{
		SessionUsageStats stats;
		const QJsonObject sessionStats =
			projectionValues.value(QStringLiteral("sessionStats")).toObject();
		stats.turns = sessionStats.value(QStringLiteral("turns")).toInt();
		stats.steps = sessionStats.value(QStringLiteral("steps")).toInt();
		// 毫秒数是累加值，走 double 再取整
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

void DSHHub::onNewWorkspaceClicked()
{
	const QString path = QFileDialog::getExistingDirectory(this, qtTrId("workdir_choose_dir_title"));
	if (path.isEmpty())
		return;

	m_api->callMethod(QStringLiteral("workspace/create"), SessionCommands::workspaceCreate(path),
		[this](const QJsonObject&) {
			if (m_sidebar && m_api)
				m_sidebar->refreshSessions(m_api);
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messageHost->current()) {
				m_messageHost->addSystemMessage(qtTrId("workdir_create_failed_fmt").arg(error.code, error.message));
			}
		});
}

void DSHHub::onCreateSessionInWorkspace(const QString& workspaceId)
{
	m_api->callMethod(QStringLiteral("session/create"), SessionCommands::sessionCreate(workspaceId),
		[this, workspaceId](const QJsonObject& value) {
			const QString newSessionId = value.value(QStringLiteral("sessionId")).toString();
			if (newSessionId.isEmpty())
				return;

			switchToFreshSession(newSessionId, qtTrId("session_untitled"), /*loadHistory=*/true);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSessionToWorkspace(newSessionId,
					qtTrId("session_untitled"), workspaceId);
				m_sidebar->workspaceList()->setCurrentSession(newSessionId);
			}
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messageHost->current()) {
				m_messageHost->addSystemMessage(qtTrId("session_new_failed_fmt").arg(error.code, error.message));
			}
		});
}

void DSHHub::createSessionAndSend(const QString& text)
{
	if (!m_api)
		return;

	m_api->callMethod(QStringLiteral("session/create"), SessionCommands::sessionCreate(),
		[this, text](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			// load 历史靠首条 prompt 的 mux 事件
			switchToFreshSession(sid, qtTrId("session_untitled"), /*loadHistory=*/false);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSession(sid, qtTrId("session_untitled"));
				m_sidebar->workspaceList()->setCurrentSession(sid);
			}

			m_messageHost->sendPrompt(text);
		},
		[this](const DshApiClient::RpcError& error) {
			if (m_messageHost->current()) {
				m_messageHost->addSystemMessage(qtTrId("session_create_failed_fmt").arg(error.code, error.message));
			}
		});
}

void DSHHub::switchToFreshSession(const QString& sessionId, const QString& title, bool loadHistory)
{
	// 与 onSessionSelected 语义一致：停流式与交互面板，避免旧状态污染新会话
	if (m_messageHost)
		m_messageHost->stopStreaming();
	if (m_messageHost)
		m_messageHost->clearInteractionPanels();

	m_sessionId = sessionId;
	// 实时事件来自 per-session 的 session/follow 流，换会话要换跟随流
	if (m_api)
		m_api->followSession(sessionId);
	// 重新统计"见过的最新 seq"（缓存新鲜度判定用）
	const int observedLastSeq = m_messageHost ? m_messageHost->observedLastSeq() : 0;
	if (m_messageHost)
		m_messageHost->noteObservedSeq(0);
	syncComposerSession();

	if (m_topBar)
		m_topBar->setTitle(title);
	if (m_topBar)
		m_topBar->setSessionId(sessionId);

	// 旧内容交缓存 -> 换空列表 -> 按需绑定 loader
	if (m_messageHost) {
		const int fallbackCursor = m_sidebar
			? m_sidebar->workspaceList()->catalog().asOfSeqFor(sessionId)
			: 0;
		m_messageHost->showFreshSession(sessionId, loadHistory, fallbackCursor, observedLastSeq);
	}
}

// 游标取 session/list 行的 asOfSeq（与 follow 快照的 cursor 相等），无需先开流
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
			continue; // 已有控件树，进入即 cache hit
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

	// 停掉旧会话的流式渲染，避免旧输出污染新会话
	if (m_messageHost)
		m_messageHost->stopStreaming();
	if (m_messageHost)
		m_messageHost->clearInteractionPanels();

	m_sessionId = sessionId;
	// 实时事件来自 per-session 的 session/follow 流，换会话要换跟随流
	if (m_api)
		m_api->followSession(sessionId);
	// 重新统计"见过的最新 seq"（缓存新鲜度判定用）
	const int observedLastSeq = m_messageHost ? m_messageHost->observedLastSeq() : 0;
	if (m_messageHost)
		m_messageHost->noteObservedSeq(0);
	syncComposerSession();

	if (m_sidebar)
		m_sidebar->workspaceList()->setCurrentSession(sessionId);

	if (m_topBar && m_sidebar)
		m_topBar->setTitle(m_sidebar->workspaceList()->titleForSession(sessionId));
	if (m_topBar)
		m_topBar->setSessionId(sessionId);

	// 整块交给宿主：控件缓存 / 预取页 / 网络拉取 -> 分批构建
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

	const auto ret = QMessageBox::question(this, qtTrId("session_delete_label"), qtTrId("session_delete_confirm"),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (ret != QMessageBox::Yes)
		return;

	m_api->callMethod(QStringLiteral("workspace/archiveSession"), SessionCommands::sessionArchive(sessionId),
		[this, sessionId](const QJsonObject& value) {
			qInfo().noquote() << "[DSH Hub] session archived:" << sessionId;

			// 回包给的是完整归档集合，立刻更新侧栏，不必等 archived 帧
			if (m_sidebar) {
				const QSet<QString> archived = SessionCatalog::parseArchivedSessionIds(value);
				m_workspaceArchived = QJsonArray::fromStringList(
					QStringList(archived.cbegin(), archived.cend()));
				applyWorkspaceState();
			}

			// archiveSession 只改归档状态，这里顺手删本地会话文件
			if (m_serverManager) {
				const QString sessionsRoot = m_serverManager->dshHome() + QStringLiteral("/sessions");
				QDir sessionsDir(sessionsRoot);
				if (sessionsDir.exists()) {
					const QStringList workspaceDirs = sessionsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
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
				if (m_messageHost)
					m_messageHost->stopStreaming();
				if (m_messageHost)
					m_messageHost->clearInteractionPanels();
				// 丢弃当前实例，避免继续持有已归档会话
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
				m_messageHost->addSystemMessage(qtTrId("session_delete_failed_fmt").arg(error.code, error.message));
			}
		});
}

void DSHHub::onClearConversationClicked()
{
	m_sidebar->clearAllSessions(m_serverManager->dshHome(),
		[this]() {
			if (m_messageHost) {
				m_messageHost->clearInteractionPanels();
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

QString DSHHub::preferredWorkspaceId()
{
	// 新会话跟旧会话同处：先看当前会话的归属
	if (m_sidebar && !m_sessionId.isEmpty()) {
		const QString fromCurrent =
			m_sidebar->workspaceList()->catalog().workspaceFor(m_sessionId);
		if (!fromCurrent.isEmpty())
			return fromCurrent;
	}

	// 退一步取基线里的第一个工作区
	for (const auto& item : m_workspaceItems) {
		const QString workspaceId = item.toObject()
			.value(QStringLiteral("workspaceId")).toString();
		if (!workspaceId.isEmpty())
			return workspaceId;
	}
	return QString();
}

void DSHHub::callSessionCreate()
{
	if (!m_api)
		return;

	// 先恢复工作区分组再建：addSessionToWorkspace 要求分组已存在
	const QString workspaceId = preferredWorkspaceId();
	if (m_sidebar)
		applyWorkspaceState();

	m_api->callMethod(QStringLiteral("session/create"), SessionCommands::sessionCreate(workspaceId),
		[this, workspaceId](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			switchToFreshSession(sid, qtTrId("session_untitled"), /*loadHistory=*/true);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSessionToWorkspace(sid, qtTrId("session_untitled"), workspaceId);
				m_sidebar->workspaceList()->setCurrentSession(sid);
			}
		},
		[this](const DshApiClient::RpcError& error) {
			finishInitialization();
			if (m_messageHost->current())
				m_messageHost->addSystemMessage(qtTrId("session_create_failed_fmt").arg(error.code, error.message));
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
	// 会话预设由服务端在建会话那一刻定死，客户端不补 agentPresets/select
	switchToFreshSession(sessionId, qtTrId("session_untitled"), /*loadHistory=*/true);

	if (m_sidebar)
		m_sidebar->addCreatedSession(sessionId, workspaceId);
	if (m_sidebar)
		m_sidebar->workspaceList()->setCurrentSession(sessionId);
}

void DSHHub::onNoSessionAvailable()
{
	if (!m_sidebar || !m_api)
		return;

	// 先按基线恢复分组再建（否则会退回"未分组"）
	applyWorkspaceState();
	m_sidebar->createSession(m_api, preferredWorkspaceId());
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

	// 弹窗必须先整个建好（构造最耗时）再铺遮罩，否则"遮罩先出、弹窗后到"
	m_extensionPopup = new ExtensionManagerPopup(m_serverManager->dshHome() + QStringLiteral("/profiles/web"), this);

	dshRegister("DSHHub.045", m_extensionPopup, &ExtensionManagerPopup::serverRestartRequested,
		m_serverManager, &ServerManager::restart);
	dshRegister("DSHHub.046", m_extensionPopup, &ExtensionManagerPopup::extensionInstalled, this,
		[this](const QString& jsonPath, const QString& dllPath) {
			if (m_dllCaller && m_dllCaller->loadDescriptor(jsonPath) && m_dllCaller->loadLibrary(dllPath)) {
				qInfo().noquote() << "[DSH DllCaller] ready tools=" << m_dllCaller->tools().join(',');
			}
			else if (m_dllCaller) {
				qWarning().noquote() << "[DSH DllCaller] load failed:" << m_dllCaller->errorString();
			}
		});
	dshRegister("DSHHub.047", m_extensionPopup, &ExtensionManagerPopup::extensionRemoving, this,
		[this](const QString& name) {
			// 先卸载该扩展的 DLL 释放文件占用
			if (m_dllCaller && !m_dllCaller->removeExtension(name)) {
				qWarning() << "[DSH DllCaller] removeExtension failed:" << name
					<< m_dllCaller->errorString();
			}
			// 服务端若因残留配置启动失败，自动清理一次
			m_cleanupResidualsAfterServerError = true;
		});
	dshRegister("DSHHub.048", m_extensionPopup, &PopupWindow::closed, this,
		[this]() {
			// 先收遮罩再销毁弹窗，两件事落在同一帧
			WindowFrame::hideOverlay(this, this);
			if (m_extensionPopup) {
				m_extensionPopup->deleteLater();
				m_extensionPopup = nullptr;
			}
		});

	// 铺遮罩 + 居中 + 显示，背靠背落在同一帧
	WindowFrame::showOverlayWithPopup(this, this, m_extensionPopup);
}

void DSHHub::handlePipeRequest(int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket)
{
	// DLL/COM 调用与响应回投都收敛到 ToolRequestDispatcher
	ToolRequestDispatcher::dispatch(m_dllCaller, m_pipeBridge, m_toolPool, id, tool, args, socket);
}

// 游标交给历史加载器唤醒挂起的首屏，并用快照 records 播种
void DSHHub::handleSessionSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore)
{
	if (!m_messageHost)
		return;

	qInfo().noquote() << "[DSH Hub] session snapshot sessionId=" << sessionId
		<< "cursor=" << cursor << "records=" << records.size() << "hasMore=" << hasMore;

	m_messageHost->setStreamCursor(cursor);
	m_messageHost->onFollowSnapshot(sessionId, cursor, records, hasMore);
}

// 快照里的会话投影（projectionMode: all）是小灰字的主要来源
void DSHHub::handleSessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values)
{
	applySessionProjections(sessionId, asOfSeq, values);
}

// session/control baseline：活会话的现算投影快照，只挑当前会话那份
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

// session/control 实时帧：只认当前会话的、显示要用的那两个 key
void DSHHub::handleSessionProjectionChanged(const QString& sessionId, const QString& key,
	const QJsonValue& value, int seq)
{
	if (!m_chatInput || sessionId != m_sessionId)
		return;

	// 别的 key 不能进合并态：会把 asOfSeq 顶高，把要用的那块挤掉
	if (!SessionProjectionState::handlesKey(key))
		return;

	QJsonObject values;
	values.insert(key, value);
	applySessionProjections(sessionId, seq, values);
}

// 两块投影分开记：哪个来源带了哪块就更新哪块
void DSHHub::applySessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values)
{
	if (!m_chatInput || sessionId != m_sessionId)
		return;

	// higher-seq-wins 与按块合并都在 SessionProjectionState 里（那条规则最容易写错）
	if (!m_projectionState.merge(asOfSeq, values))
		return;

	// 合并后整包重算，控件只看结果
	const SessionUsageStats stats = parseSessionUsage(m_projectionState.merged());

	// 全 0 也照样交给控件：小灰字始终显示，不整行藏掉
	qInfo().noquote() << "[DSH Hub] session stats applied: turns=" << stats.turns
		<< "steps=" << stats.steps << "llmMs=" << stats.llmMs << "toolMs=" << stats.toolMs
		<< "ttftSteps=" << stats.ttftSteps << "hasUsage=" << stats.hasUsage
		<< "asOfSeq=" << asOfSeq;
	m_chatInput->setSessionStats(stats);
}

// 全量工作区 + 归档集合
void DSHHub::handleWorkspaceSnapshot(const QJsonArray& items, const QJsonArray& archivedSessionIds)
{
	qInfo().noquote() << "[DSH Hub] workspace baseline items=" << items.size()
		<< "archived=" << archivedSessionIds.size();
	m_workspaceItems = items;
	m_workspaceArchived = archivedSessionIds;
	applyWorkspaceState();
}

// upsert：同 id 替换，否则追加
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

// archived：归档集合整体替换
void DSHHub::handleWorkspaceArchiveChanged(const QJsonArray& archivedSessionIds)
{
	m_workspaceArchived = archivedSessionIds;
	applyWorkspaceState();
}

// order：按服务端给的完整顺序重排本地缓存
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

	// 服务端没点名的接在后面，避免丢工作区
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

// 把归档状态推给侧栏 catalog 并重建列表视图
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

	// 重建会丢选中态，按当前会话再标一次
	if (!m_sessionId.isEmpty())
		m_sidebar->workspaceList()->setCurrentSession(m_sessionId);
}

void DSHHub::forwardMuxFrame(const QJsonObject& frame)
{
	// 帧的路由整体归 MessageHost，这里只是转发点
	if (!m_messageHost)
		return;

	m_messageHost->handleMuxFrame(frame);
}

void DSHHub::handleTransportError(const QString& context, const QString& message)
{
	// 服务端主动重启时旧连接断开是预期的，不刷到聊天区
	if (m_serverManager && m_serverManager->isRestarting())
		return;

	m_messageHost->addSystemMessage(qtTrId("chat_transport_error_fmt").arg(context, message));
}
