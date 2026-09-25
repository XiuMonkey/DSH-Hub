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

// 注意：current() 返回 MessageQuery*，这里调用它的成员函数（lastAgentUnit 等）
// 所以需要完整类型，不能只靠前置声明
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

	// 构造函数只剩这一串装配步骤 —— 它本身就是最好的初始化清单。
	// 三个顺序约束不要打乱（每条的来由写在对应方法的注释里）：
	//   1. installWindowShell 最早：无边框标志必须在原生窗口创建之前设置
	//   2. installServer 里的 spawn 最前：Node 启动（约 1s）与后面的步骤并行
	//   3. registerHostObjects 最后：客户端扩展要查注册表、往已建好的布局挂控件

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

/**
 * 无边框 + 自绘圆角描边窗口的初始标志。
 *
 * ⚠️ 必须在原生窗口创建之前设置，否则会触发窗口重建 —— Qt 一旦建了原生句柄，
 *    再改 FramelessWindowHint 就要销毁重建。所以它是构造函数的第一步，
 *    排在 ServerManager / DllCaller 这些不碰窗口的动作之前。
 */
void DSHHub::installWindowShell()
{
	// 无边框窗口：系统标题栏与边框全部由自绘替代 —— 标题栏见 TitleBar，
	// 圆角 + 1px 描边由 #dshhubCentral 的 QSS 画（main-window.qss），
	// 窗口本体透明，圆角之外什么都不画
	setObjectName(QStringLiteral("dshHubWindow"));
	setWindowFlag(Qt::FramelessWindowHint, true);
	setAttribute(Qt::WA_TranslucentBackground, true);

	// 样式表按窗口安装（替代全局 qApp 表）：本窗口与后续加入的子控件统一应用当前主题
	ThemeManager::instance().applyToWindow(this);
	setWindowTitle(QStringLiteral("DSH Hub"));
	setAttribute(Qt::WA_DeleteOnClose);
}

/**
 * 服务端进程：创建 ServerManager、接上 DSHHub.001-004 四条状态线、然后 spawn。
 *
 * ⚠️ spawn 必须放在构造函数最前段：Node 进程启动要 0.9~1.6s，提前起能让它与
 *    工具扩展加载 / UI 构建 / 首帧真正并行，缩短初始化墙钟时间。
 *    ServerManager::start 会同步填充 dshHome，因此之后创建的 Settings /
 *    PluginsManager 仍可正常使用它（见 installSettingsAndPlugins）。
 */
void DSHHub::installServer(const QUrl& initialBaseUrl, QProcess* initialServerProcess)
{
	m_serverManager = new ServerManager(this);
	dshRegister("DSHHub.001",
		m_serverManager, &ServerManager::baseUrlReady, this, [this](const QUrl& url) {
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
	dshRegister("DSHHub.002",
		m_serverManager, &ServerManager::errorLine, this, [this](const QString& line) {
			if (m_messageHost && m_messageHost->current())
				m_messageHost->addSystemMessage(qtTrId("server_status_fmt").arg(line));
			// 移除扩展后服务端启动失败时，自动清理 cordis.patch.yml 残留
			if (m_cleanupResidualsAfterServerError && m_extensionPopup) {
				m_cleanupResidualsAfterServerError = false;
				m_extensionPopup->cleanupResiduals();
			}
			if (!isInitializationComplete())
				finishInitialization();
		});
	dshRegister("DSHHub.003",
		m_serverManager, &ServerManager::outputLine, this, [](const QString& line) {
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
	dshRegister("DSHHub.004",
		m_serverManager, &ServerManager::finished, this, [this](int exitCode, QProcess::ExitStatus) {
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

/**
 * 工具扩展运行时：DLL 调用线程池 + 命名管道桥 + 启动时装载已安装的工具扩展 DLL。
 *
 * ⚠️ 这三条线都属于"工具扩展"（跑 Worker 线程、只做 JSON 工具调用），
 *    与 registerHostObjects 里的"客户端扩展"不是一套东西（那个跑 GUI 线程、改宿主界面）。
 */
void DSHHub::installToolRuntime()
{
	// DLL/COM 工具调用线程池：请求在 Worker 上执行（GUI 不阻塞）；
	// DllCaller 内部同 DLL 串行、跨 DLL 并行
	m_toolPool = new QThreadPool(this);
	m_toolPool->setMaxThreadCount(4);

	// 启动命名管道桥接服务，供 Node/DSh server 调用 DLL 工具
	m_pipeBridge = new DshNamedPipeBridge(this);
	dshRegister("DSHHub.005",
		m_pipeBridge, &DshNamedPipeBridge::requestReceived, this, &DSHHub::handlePipeRequest);
	if (!m_pipeBridge->start()) {
		qWarning() << QStringLiteral("[DSH Pipe] failed to start:") << m_pipeBridge->errorString();
	}

	// 工具扩展（DLL）装载：env 显式指定（调试）或扫描已安装扩展目录；
	// 细节全在 ExtensionSystem/ExtensionDllLoader.h
	m_dllCaller = new DllCaller;
	const QString serverProfilePath =
		QCoreApplication::applicationDirPath() + QStringLiteral("/resources/server/harness/profiles/web");
	ExtensionDllLoader::loadAll(m_dllCaller, serverProfilePath);

	TimingLogger::mark(QStringLiteral("extension DLLs loaded"));
}

/** 从 Qt 资源里加载代码高亮规则（规则文件缺失时只记警告，代码高亮不可用）。 */
void DSHHub::loadHighlightRules()
{
	if (!CodeHighlighter::instance().loadFromFile(QStringLiteral(":/DSHHub/highlight_rules.json"))) {
		qWarning().noquote() << QStringLiteral("[DSH Hub] 未找到内置资源 highlight_rules.json，代码高亮不可用");
	}
}

/** 输入区接线：模型 / 思考深度变更只记日志（业务动作都在输入控件自己那侧）。 */
void DSHHub::installInputWiring()
{
	dshRegister("DSHHub.006",
		m_chatInput, &ChatInputWidget::modelChanged, this,
		[](const QString& provider, const QString& model) {
			qInfo().noquote() << QStringLiteral("[DSH Hub] model ->")
				<< QStringLiteral("%1/%2").arg(provider, model);
		});
	dshRegister("DSHHub.007",
		m_chatInput, &ChatInputWidget::thinkingDepthChanged, this, [](const QString& levelId) {
			qInfo().noquote() << QStringLiteral("[DSH Hub] thinking depth ->")
				<< (levelId.isEmpty() ? qtTrId("common_default_suffix") : levelId);
		});
}

/**
 * 侧栏 15 条接线：新建 / 切会话 / 删除 / 清空 + 设置 / 插件 / 主题 / 扩展四个入口
 * + 会话列表事件。
 *
 * 这里刻意按 sender 把原本散在构造函数两处的侧栏接线合并到一起（008-012 与 025-034
 * 原来被 api 那 12 条隔开）。各条 RegisterConnection 彼此独立，合并只影响可读性。
 */
void DSHHub::installSidebarWiring()
{
	dshRegister("DSHHub.008",
		m_sidebar, &Sidebar::newWorkspaceRequested, this, &DSHHub::onNewWorkspaceClicked);
	dshRegister("DSHHub.009",
		m_sidebar, &Sidebar::createSessionInWorkspaceRequested, this, &DSHHub::onCreateSessionInWorkspace);
	dshRegister("DSHHub.010",
		m_sidebar, &Sidebar::sessionSelected, this, &DSHHub::onSessionSelected);
	dshRegister("DSHHub.011",
		m_sidebar, &Sidebar::deleteSessionRequested, this, &DSHHub::onDeleteSessionRequested);
	dshRegister("DSHHub.012",
		m_sidebar, &Sidebar::clearRequested, this, &DSHHub::onClearConversationClicked);

	// 侧边栏"设置 / 插件"入口：Settings 与 PluginsManager 都是常驻"系统"，
	// 窗口开关由它们自己管理，这里只做一次接线；实际创建放在
	// installSettingsAndPlugins（在 ServerManager::start 之后，那时 dshHome 才可用）
	dshRegister("DSHHub.025",
		m_sidebar, &Sidebar::settingsRequested, this,
		[this]() { if (m_settings) m_settings->openSettings(); });
	dshRegister("DSHHub.026",
		m_sidebar, &Sidebar::pluginsRequested, this,
		[this]() {
			// 接管态：插件市场是 DSH 服务端专属的那条线，入口已收（Sidebar 那颗按钮不可见），
			// 这里再挡一道 —— 信号也可能从别处被触发（侧栏是登记在册的对象）。
			if (m_api && m_api->isTakenover()) {
				qInfo().noquote() << QStringLiteral(
					"[DSH Hub] 后端已被接管：插件市场入口不可用");
				return;
			}
			if (m_pluginsManager)
				m_pluginsManager->openPlugins();
		});
	dshRegister("DSHHub.027",
		m_sidebar, &Sidebar::themeToggleRequested, this, &DSHHub::toggleTheme);
	dshRegister("DSHHub.028",
		m_sidebar, &Sidebar::extensionsRequested, this, &DSHHub::openExtensions);
	dshRegister("DSHHub.029",
		m_sidebar, &Sidebar::initialSessionReady, this, &DSHHub::onInitialSessionReady);
	// 会话列表刷新（含启动、增删会话）对可见会话排一轮首屏预取
	dshRegister("DSHHub.030",
		m_sidebar, &Sidebar::sessionsRefreshed, this, &DSHHub::onSessionsRefreshed);
	dshRegister("DSHHub.031",
		m_sidebar, &Sidebar::sessionCreated, this, &DSHHub::onSessionCreated);
	dshRegister("DSHHub.032",
		m_sidebar, &Sidebar::noSessionAvailable, this, &DSHHub::onNoSessionAvailable);
	dshRegister("DSHHub.033",
		m_sidebar, &Sidebar::sessionListError, this, &DSHHub::onSessionListError);
	dshRegister("DSHHub.034",
		m_sidebar, &Sidebar::sessionCreateError, this, &DSHHub::onSessionCreateError);
}

/**
 * mux 流 / 快照 / 投影 / workspace 共 12 条：DshApiClient → DSHHub 的数据入口。
 * 小灰字那条路（016-018）与 workspace 那条路（019-023）见各自 handler 的注释。
 */
void DSHHub::installApiWiring()
{
	dshRegister("DSHHub.013",
		m_api, &DshApiClient::connected, this, &DSHHub::handleConnected);
	dshRegister("DSHHub.014",
		m_api, &DshApiClient::muxFrameReceived, this, &DSHHub::forwardMuxFrame);
	// 0.1.5：历史由 session/follow 快照播种，工作区由 workspace/follow 驱动
	dshRegister("DSHHub.015",
		m_api, &DshApiClient::sessionSnapshotReady, this, &DSHHub::handleSessionSnapshot);
	// 小灰字：快照里那份"全量折叠"的投影做种子 + session/control 的实时推送做更新。
	// 两条都是服务端现算，客户端不读任何缓存（列表行那条读的是投影缓存检查点，已弃用）。
	dshRegister("DSHHub.016",
		m_api, &DshApiClient::sessionProjectionsReady, this, &DSHHub::handleSessionProjections);
	dshRegister("DSHHub.017",
		m_api, &DshApiClient::sessionProjectionsBaselineReady, this, &DSHHub::handleSessionControlBaseline);
	dshRegister("DSHHub.018",
		m_api, &DshApiClient::sessionProjectionChanged, this, &DSHHub::handleSessionProjectionChanged);
	dshRegister("DSHHub.019",
		m_api, &DshApiClient::workspaceSnapshotReady, this, &DSHHub::handleWorkspaceSnapshot);
	dshRegister("DSHHub.020",
		m_api, &DshApiClient::workspaceUpserted, this, &DSHHub::handleWorkspaceUpserted);
	dshRegister("DSHHub.021",
		m_api, &DshApiClient::workspaceRemoved, this, &DSHHub::handleWorkspaceRemoved);
	dshRegister("DSHHub.022",
		m_api, &DshApiClient::workspaceReordered, this, &DSHHub::handleWorkspaceReordered);
	dshRegister("DSHHub.023",
		m_api, &DshApiClient::workspaceArchiveChanged, this, &DSHHub::handleWorkspaceArchiveChanged);
	dshRegister("DSHHub.024",
		m_api, &DshApiClient::transportError, this, &DSHHub::handleTransportError);

	// 045 · 后端接管开关：插件调 VirtualApiHost::Takenover 时从这里收回执。
	// 只管三件"接管特有的收尾"（宿主上层其余部分一行不改，它们照常调出站方法、
	// 照常收回调、照常 emit 回执，因为分流在 DshApiClient 内部）：
	//   ① 停掉已经启动的内置 DSH 进程（装配顺序决定了它一定先起来过，见 D8=C）
	//   ② 关掉三条 DSH 专属旁路里需要 UI 配合的两条（工具过滤 / 插件市场入口）
	//   ③ 进程级接管标记：接管态下不再启动/重启内置 DSH 服务端
	dshRegister("DSHHub.045",
		m_api, &DshApiClient::takeoverChanged, this, [this](bool takenover) {
			if (m_serverManager) {
				if (takenover)
					m_serverManager->stopForTakeover();
				else
					m_serverManager->setTakenover(false);
			}
			applyTakeoverBypasses(takenover);
		});
}

/**
 * 后端接管态下的三条 DSH 专属旁路（见 misc/API_TAKEOVER_PLAN.zh-CN.md §2.5-A）。
 *
 * ⚠️ 只关这三条，**绝不**碰"扩展管理"：那是客户端扩展的装载通道（ExtensionManagerPopup →
 *    ClientExtension::loadOne），接管机制本身要靠它把扩展装进来。
 * 幂等：重复拨同一状态只重复设置同一批控件的可见性，没有副作用。
 */
void DSHHub::applyTakeoverBypasses(bool takenover)
{
	// ① 顶栏"工具过滤"：面板走 /api/tools-filter，自带 QNetworkAccessManager，
	//    服务的完全是 DSH 服务端那套工具清单
	if (m_topBar)
		m_topBar->setToolsFilterEnabled(!takenover);

	// ② 插件市场入口：DSH 服务端侧的 cordis 插件（7 个 /dsh-market/* + pnpm/dsh CLI）。
	//    接管时连同已经打开的窗口一起收掉，别留一个注定刷不出东西的空壳。
	//    （扩展管理那颗按钮是 Sidebar::m_extensionButton，不在这里动。）
	if (m_sidebar)
		m_sidebar->setPluginsEntryEnabled(!takenover);

	if (takenover && m_pluginsManager)
		m_pluginsManager->closePlugins();

	// ③ "启动 DSH 进程"这条旁路不在这里关：接管到来时进程已经起来了（装配顺序如此），
	//    所以处置是"停掉 + 之后不再启动/重启"，两者都在 ServerManager 里（stopForTakeover()
	//    与进程级接管标记），见上面 045 那条接线。
	qInfo().noquote() << QStringLiteral("[DSH Hub] 后端接管态") << (takenover ? "已生效" : "已复位")
		<< QStringLiteral("（工具过滤/插件市场入口已按态切换；扩展管理照旧）");
}

/**
 * 消息区宿主 MessageHost + 首屏预取 SessionPrefetcher，含 DSHHub.035-040 六条接线。
 *
 * ⚠️ 必须排在 installWindowShell / buildUi 之后：它要用 buildUi 建好的滚动区、
 *    消息布局、输入控件与"加载更多"按钮。
 */
void DSHHub::installMessageWiring()
{
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

	dshRegister("DSHHub.035",
		m_messageHost, &MessageHost::sendWithoutSession, this, &DSHHub::createSessionAndSend);
	// 首屏内容真正上屏 -> 收掉启动遮罩（finishInitialization 幂等）
	dshRegister("DSHHub.036",
		m_messageHost, &MessageHost::contentReady, this, &DSHHub::finishInitialization);
	// 整列表被整体替换（缓存恢复首屏构建完成）→ 先收掉内联交互面板，
	// 再在刷新前同步滚到底，避免先显示顶部再闪烁
	dshRegister("DSHHub.037",
		m_messageHost, &MessageHost::contentReplaced, this, [this]() {
			m_messageHost->clearInteractionPanels();
			m_messageHost->scrollToBottomNow();
		});
	// 一次对话收尾：刷新会话标题（标题在侧栏，MessageHost 不碰）
	dshRegister("DSHHub.038",
		m_messageHost, &MessageHost::turnFinished, this, [this]() {
			if (m_sidebar && m_api)
				m_sidebar->workspaceList()->refreshTitles(m_api);
		});

	// 首屏预取：session/list 回来后并发发一批 session/page，结果入库供"立即点亮"
	m_prefetcher = new SessionPrefetcher(this);
	m_prefetcher->setApi(m_api);
	dshRegister("DSHHub.039",
		m_prefetcher, &SessionPrefetcher::historyFetched, this,
		[this](const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore) {
			if (!m_messageHost)
				return;
			// 命中当前会话且界面还空着 → 已用它点亮；这时才记"见过的最新 seq"
			if (m_messageHost->onPrefetched(sessionId, events, throughSeq, hasMore)
				== MessageHost::PrefetchOutcome::Painted) {
				m_messageHost->noteObservedSeq(throughSeq);
			}
		});
	dshRegister("DSHHub.040",
		m_prefetcher, &SessionPrefetcher::prefetchFailed, this,
		[](const QString& sessionId, const QString& code, const QString& message) {
			qWarning().noquote() << "[DSH Hub] prefetch failed sessionId=" << sessionId
				<< "code=" << code << "message=" << message;
		});
}

/**
 * 常驻"设置系统"与"插件系统"：创建 + DSHHub.041-044 四条接线 + 顶栏 baseUrl 现取回调。
 *
 * ⚠️ 必须排在 installServer 之后：Settings / PluginsManager 的构造要用 dshHome，
 *    由 ServerManager::start 同步填充。
 */
void DSHHub::installSettingsAndPlugins()
{
	// ------------------------------------------------------------------
	// 常驻“设置系统”：随主窗口存在，自管设置窗口的开关/遮罩/居中。
	// 放在 start() 之后创建，因为 Settings 构造需要 dshHome
	// （由 ServerManager::start 填充）。这里只做一次业务信号接线：
	// 预设变更只记日志、新增模型后刷新选择器
	// ------------------------------------------------------------------
	m_settings = new Settings(m_api, this);
	// 用户在设置里改了默认预设：那次服务端写入（settings/update）已经在
	// Settings 里完成，这里不需要跟着改任何本地状态或当前会话 ——
	// 客户端不再自己存一份默认值，新建会话时也不带 agentPreset，
	// 由服务端按它自己的设置文档组装（“默认值只影响新建会话”是服务端的语义）。
	dshRegister("DSHHub.041",
		m_settings, &Settings::agentPresetChanged, this, [](const QString& presetId) {
			qInfo().noquote() << QStringLiteral("[DSH Hub] default agent preset ->") << presetId;
		});
	dshRegister("DSHHub.042",
		m_settings, &Settings::serverSettingsSaved, this, [this]() {
			if (m_serverManager)
				m_serverManager->restart();
		});
	// 设置里新增了模型：服务端 settings 已热生效，输入框底的模型选择
	// 需要重新拉一次会话目录才能看到新模型
	dshRegister("DSHHub.043",
		m_settings, &Settings::modelAdded, this, [this](const QString& provider, const QString& modelId) {
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
	dshRegister("DSHHub.044",
		m_pluginsManager, &PluginsManager::serverRestartRequested,
		m_serverManager, &ServerManager::restart);

	// ------------------------------------------------------------------
	// 工具栏右侧的“工具过滤”：baseUrl 用回调现取（启动/重启都会变），
	// 会话 id 由 setSessionId() 在切会话时推给 TopBar。
	// ------------------------------------------------------------------
	if (m_topBar) {
		m_topBar->setBaseUrlProvider([this]() { return m_api ? m_api->baseUrl() : QUrl(); });
	}
}

/**
 * 登记宿主对象并装载客户端扩展（ClientExtension）。
 *
 * ⚠️ 必须最后：窗口、顶栏 / 侧栏都建好且已登记之后才能装载 —— 客户端扩展在
 *    attachHost() 里要查注册表并往布局里挂控件。
 */
void DSHHub::registerHostObjects()
{
	// 登记主窗口：插件可用 C 导出 DshHubHostRegistryFind 按 index 取到本窗口。
	// 侧栏/顶栏各自在自己的构造函数里登记，这里只管窗口自身。
	// 登记是覆盖语义 —— 切主题时新窗口先建、旧窗口下一轮事件循环才析构，
	// 新窗口这一句会直接顶掉旧窗口的登记（见 CommonRegistry.h 的设计要点）。
	CommonRegistry::instance().AddToRegistry(DshHostIndex::kMainWindow, this);

	// 登记 DSH API 客户端：客户端扩展要用它拿**后端接管**的宿主侧接口
	// （VirtualClass/VirtualApiTakeover.h 的 VirtualApiHost：Takenover / CompleteCall / FailCall）。
	// 时序是现成可用的：m_api 在构造函数开头创建、这里登记，而下面的 ClientExtension::loadAll()
	// 在登记之后才跑 —— 插件在 attachHost() 里查得到。
	// ⚠️ 切主题会重建主窗口、连带换掉 m_api（m_api 是窗口的子对象）⇒ 插件必须在**每次**
	//    attachHost() 里重新 findObject + 重新 cast；旧指针是 QPointer，只会变空、不会变野。
	CommonRegistry::instance().AddToRegistry(DshHostIndex::kApiClient, m_api);

	// 装载「客户端扩展」（ClientExtension）：装进本进程、在 GUI 线程直接改宿主界面。
	// 位置很关键：必须在窗口、顶栏/侧栏都建好且已登记之后 —— 插件在 attachHost()
	// 里要查注册表并往布局里挂控件。
	// ⚠️ 与上面 DllCaller 那条线（工具扩展）不是一套东西：那套跑 Worker 线程、
	//    只做 JSON 工具调用，碰不到宿主对象。
	const auto clientExtensions = ClientExtension::loadAll();
	if (!clientExtensions.isEmpty()) {
		qInfo().noquote() << QStringLiteral("[DSH Hub] client extensions:")
			<< clientExtensions.join(QStringLiteral(", "));
	}
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
	ThemeManager::instance().switchTheme(this);
}

// openPlugins 已迁出：插件窗口的开关/遮罩/居中由常驻的
// PluginsManager（openPlugins()/closePlugins()）自管，见构造函数

QProcess* DSHHub::takeServerProcess()
{
	return m_serverManager ? m_serverManager->takeProcess() : nullptr;
}

DSHHub::~DSHHub()
{
	// 先摘除登记：一旦开始拆成员，这个窗口就不再是"可用的主窗口"了，
	// 继续登记着只会让插件拿到半残对象。Destroy 带身份校验，安全。
	CommonRegistry::instance().Destroy(DshHostIndex::kMainWindow, this);

	// 摘除 DSH API 客户端的登记（Destroy 带身份校验）：切主题时新窗口已经顶掉了这条登记，
	// 旧窗口在这里的注销会被拒绝，不会误删新记录。
	if (m_api)
		CommonRegistry::instance().Destroy(DshHostIndex::kApiClient, m_api);

	// 先停掉工具调用线程池，避免 Worker 仍在 m_dllCaller / 排队任务中引用 this
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
	m_projectionState.reset();
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

void DSHHub::onNewWorkspaceClicked()
{
	const QString path = QFileDialog::getExistingDirectory(
		this,
		qtTrId("workdir_choose_dir_title"));

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
				m_messageHost->addSystemMessage(qtTrId("workdir_create_failed_fmt").arg(error.code, error.message));
			}
		});
}

void DSHHub::onCreateSessionInWorkspace(const QString& workspaceId)
{
	m_api->callMethod(
		QStringLiteral("session/create"),
		SessionCommands::sessionCreate(workspaceId),
		[this, workspaceId](const QJsonObject& value) {
			const QString newSessionId = value.value(QStringLiteral("sessionId")).toString();
			if (newSessionId.isEmpty())
				return;

			switchToFreshSession(newSessionId, qtTrId("session_untitled"), /*loadHistory=*/true);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSessionToWorkspace(newSessionId, qtTrId("session_untitled"), workspaceId);
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

	m_api->callMethod(
		QStringLiteral("session/create"),
		SessionCommands::sessionCreate(),
		[this, text](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			// load 历史：等首条 prompt mux 事件即可；adoptSession
			// loader 绑定新会话，之后"加载更多"不会误用旧会id
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
	// onSessionSelected 切换语义保持一致：停流式与交互面板，避免旧状态污染新会话
	// （取消旧构建MessageHost::showFreshSession 内部做）
	if (m_messageHost)
		m_messageHost->stopStreaming();
	if (m_messageHost)
		m_messageHost->clearInteractionPanels();

	m_sessionId = sessionId;
	// 0.1.5：实时日志事件来mux per-session session/follow 流，
	// 换会话时把跟随流也换过去（快照帧会补齐初始历史，DshApiClient）
	if (m_api)
		m_api->followSession(sessionId);
	// 新会话重新开始统计"见过的最新 seq"（缓存新鲜度判定用）；旧值交给消息区写缓存元数据
	const int observedLastSeq = m_messageHost ? m_messageHost->observedLastSeq() : 0;
	if (m_messageHost)
		m_messageHost->noteObservedSeq(0);
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
	if (m_messageHost)
		m_messageHost->clearInteractionPanels();

	m_sessionId = sessionId;
	// 0.1.5：实时日志事件来mux per-session session/follow 流，
	// 换会话时把跟随流也换过去（快照帧会补齐初始历史，DshApiClient）
	if (m_api)
		m_api->followSession(sessionId);
	// 新会话重新开始统计"见过的最新 seq"（缓存新鲜度判定用）；旧会话观测到的值交
	// MessageHost 写进缓存元数据（缓存内容最新位）
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
		qtTrId("session_delete_label"),
		qtTrId("session_delete_confirm"),
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
				if (m_messageHost)
					m_messageHost->clearInteractionPanels();
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
				m_messageHost->addSystemMessage(qtTrId("session_delete_failed_fmt").arg(error.code, error.message));
			}
		});
}

void DSHHub::onClearConversationClicked()
{
	m_sidebar->clearAllSessions(
		m_serverManager->dshHome(),
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
	// 用户预期"新会话跟旧会话同处"，所以先看当前会话的归属
	if (m_sidebar && !m_sessionId.isEmpty()) {
		const QString fromCurrent =
			m_sidebar->workspaceList()->catalog().workspaceFor(m_sessionId);
		if (!fromCurrent.isEmpty())
			return fromCurrent;
	}

	// 退一步：基线里的第一个工作区（清空会话时当前会话已经没了）
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

	// 归属由我们显式给出（理由见 preferredWorkspaceId），并且在建之前先把工作区
	// **分组**恢复出来 —— "清空会话"把 catalog 连分组一起清了，而
	// addSessionToWorkspace 需要那个分组已经存在，否则照样落到"未分组"。
	// applyWorkspaceState 用的是客户端手里那份 workspace/follow 基线，不发请求。
	const QString workspaceId = preferredWorkspaceId();
	if (m_sidebar)
		applyWorkspaceState();

	m_api->callMethod(
		QStringLiteral("session/create"),
		SessionCommands::sessionCreate(workspaceId),
		[this, workspaceId](const QJsonObject& value) {
			const QString sid = value.value(QStringLiteral("sessionId")).toString();
			if (sid.isEmpty())
				return;

			switchToFreshSession(sid, qtTrId("session_untitled"), /*loadHistory=*/true);
			if (m_sidebar) {
				m_sidebar->workspaceList()->addSessionToWorkspace(
					sid, qtTrId("session_untitled"), workspaceId);
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
	// 统一入口：取消旧构建/停流式/缓存当前会话/换空列表/绑定并加载新会话
	//
	// 这里不再补一次 agentPresets/select：会话的预设由服务端在建会话那一刻定死
	// （默认会话按 settings 文档里的默认值组装，并写进会话头），客户端没有
	// “想让它用哪个”的额外主张 —— 与原版一致，也不会有客户端默认值覆盖服务端的问题。
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

	// 删除最后一个会话会走到这里。新建的会话要挂进工作区分组，所以先按基线把分组
	// 恢复出来（分组不存在时 addSessionToWorkspace 会退回"未分组"），再带上归属建。
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

	// 先把弹窗整个建好：构造函数要建全部控件、读扩展清单，是本函数里最耗时的一步。
	// 它必须排在铺遮罩**之前** —— 遮罩那次同步重绘要紧贴弹窗 show()，中间夹工作
	// 就会出现"遮罩先出、弹窗后到"（见 WindowFrame::showOverlayWithPopup）。
	m_extensionPopup = new ExtensionManagerPopup(m_serverManager->dshHome() + QStringLiteral("/profiles/web"), this);

	dshRegister("DSHHub.045",
		m_extensionPopup, &ExtensionManagerPopup::serverRestartRequested,
		m_serverManager, &ServerManager::restart);
	dshRegister("DSHHub.046",
		m_extensionPopup, &ExtensionManagerPopup::extensionInstalled, this,
		[this](const QString& jsonPath, const QString& dllPath) {
			if (m_dllCaller && m_dllCaller->loadDescriptor(jsonPath) && m_dllCaller->loadLibrary(dllPath)) {
				qInfo().noquote() << "[DSH DllCaller] ready tools=" << m_dllCaller->tools().join(',');
			}
			else if (m_dllCaller) {
				qWarning().noquote() << "[DSH DllCaller] load failed:" << m_dllCaller->errorString();
			}
		});
	dshRegister("DSHHub.047",
		m_extensionPopup, &ExtensionManagerPopup::extensionRemoving, this,
		[this](const QString& name) {
			// 移除扩展前先卸载该扩展的 DLL，释放文件占用（其它扩展不受影响）
			if (m_dllCaller && !m_dllCaller->removeExtension(name)) {
				qWarning() << "[DSH DllCaller] removeExtension failed:" << name
					<< m_dllCaller->errorString();
			}
			// 如果移除后服务端因残留配置启动失败，自动清理一次
			m_cleanupResidualsAfterServerError = true;
		});
	dshRegister("DSHHub.048",
		m_extensionPopup, &PopupWindow::closed, this, [this]() {
			// 先收遮罩、再销毁弹窗：收遮罩那一步会同步重绘一次本窗口，两件事
			// 落在同一帧上（理由见 WindowFrame.h 的 showOverlay/hideOverlay）。
			WindowFrame::hideOverlay(this, this);
			if (m_extensionPopup) {
				m_extensionPopup->deleteLater();
				m_extensionPopup = nullptr;
			}
		});

	// 铺遮罩 + 居中 + 显示：背靠背完成，两者落在同一帧
	WindowFrame::showOverlayWithPopup(this, this, m_extensionPopup);
}

void DSHHub::handlePipeRequest(int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket)
{
	// DLL/COM 调用与响应回投（Worker 线程执行 + GUI 线程发送）收敛到
	// ToolRequestDispatcher，见其头文件注释
	ToolRequestDispatcher::dispatch(m_dllCaller, m_pipeBridge, m_toolPool, id, tool, args, socket);
}

/**
 * session/follow 的快照到达：把游标交给历史加载器（唤醒可能挂起的首屏请求），
 * 并直接用快照里的 records 播种首屏 —— 省掉一次 session/page 往返
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

	// 小灰字只显示这两块键；别的 key 不能进合并态 —— 它会把 asOfSeq 顶高，
	// 接着真正要用的那块就被 higher-seq-wins 丢掉了
	if (!SessionProjectionState::handlesKey(key))
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

	// higher-seq-wins + 按块合并都在 SessionProjectionState 里（那条规则最容易写错）
	if (!m_projectionState.merge(asOfSeq, values))
		return;

	// 合并后整包重算：控件的拼行只看结果，不关心数据是哪一次来的
	const SessionUsageStats stats = parseSessionUsage(m_projectionState.merged());

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

void DSHHub::forwardMuxFrame(const QJsonObject& frame)
{
	// 帧的路由（会话事件渲染、交互面板）整体归 MessageHost；
	// 这里只是一个转发点，顺便保证 messageHost 还没搭好时不崩。
	if (!m_messageHost)
		return;

	m_messageHost->handleMuxFrame(frame);
}

void DSHHub::handleTransportError(const QString& context, const QString& message)
{
	// 服务端主动重启时，旧 WebSocket 断开是预期行为，不当作错误刷到聊天区
	if (m_serverManager && m_serverManager->isRestarting())
		return;

	m_messageHost->addSystemMessage(qtTrId("chat_transport_error_fmt").arg(context, message));
}

// ------------------------------------------------------------------
// VirtualWindow 接口的实现（客户端扩展用）
// ------------------------------------------------------------------
// 与设置 / 插件市场 / 扩展管理走同一条路径：遮罩铺在宿主上、弹窗居中显示，
// 两步背靠背完成（理由见 WindowFrame::showOverlayWithPopup）。
// owner 就用弹窗自身：遮罩按 owner 记名，扩展只要 show/hide 成对就不会串。
void DSHHub::ExternalShowOverlay(QWidget* popup)
{
	if (!popup)
		return;

	WindowFrame::showOverlayWithPopup(this, popup, popup);
}

void DSHHub::ExternalHideOverlay(QWidget* popup)
{
	if (!popup)
		return;

	WindowFrame::hideOverlay(this, popup);
}