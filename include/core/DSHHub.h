#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include "chat/CacheHistoryManager.h"

// 客户端扩展那套全内联虚接口（VirtualTopBar / VirtualTheme / VirtualWindow）：
// 本窗口实现其中的 VirtualWindow，插件按 index 取到本对象后 qobject_cast 成接口再用。
#include "VirtualClass/VirtualCommon.h"

// 架空（VirtualShell）的让渡动作本体。放在 common/appearance 下是因为
// WindowFrame 也要问它（遮罩范围 / 窗口条命中测试），见 UiStage.h 的文件头。
#include "common/appearance/UiStage.h"

#include <QMainWindow>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QList>

class DshApiClient;
class DshNamedPipeBridge;
class DllCaller;
class QByteArray;
class QEvent;
class QLabel;
class QLocalSocket;
class QMoveEvent;
class QShowEvent;
class QThreadPool;
class QUrl;
class QResizeEvent;
class ServerManager;
class SessionPrefetcher;
class ChatInputWidget;
class Sidebar;
class TopBar;
class TitleBar;
class Settings;
class PluginsManager;
class ExtensionManagerPopup;
class AgentMessageUnit;
class MessageHost;
class QVBoxLayout;
class QScrollArea;
class QTimer;
class LoadMoreButton;

class DSHHub : public QMainWindow, public VirtualWindow, public VirtualShell
{
	Q_OBJECT
		Q_INTERFACES(VirtualWindow VirtualShell)

public:
	explicit DSHHub(QWidget* parent = nullptr,
		const QUrl& initialBaseUrl = QUrl(),
		QProcess* initialServerProcess = nullptr);
	~DSHHub() override;
	QUrl baseUrl() const;
	/**
	 * 带认证令牌的 baseUrl：供"重建主窗口但仍复用同一个服务端"的场景使用
	 * （主题切换就是重建窗口）。不带令牌时新窗口换不到 cookie，会 401 卡住。
	 */
	QUrl authenticatedBaseUrl() const;

	bool isInitializationComplete() const;

	QProcess* takeServerProcess();

	// ------------------------------------------------------------------
	// VirtualWindow 接口的实现（客户端扩展唯一入口）
	// ------------------------------------------------------------------
	// 插件把注册表里取到的 mainWindow 转成 VirtualWindow*，就能把**它自己的**顶层窗口
	// 当成宿主弹窗：铺遮罩 + 居中显示。内部走的就是宿主自己的设置 / 插件市场 / 扩展管理
	// 那条路径（WindowFrame::showOverlayWithPopup），所以"遮罩先到、弹窗后到"的缝不存在。
	// 所以扩展要把窗口**先建好**（控件都搭完、尺寸定下来），再调它。
	//
	// 插件侧用法（它只需 include VirtualClass/VirtualCommon.h 与 core/HostExports.h）：
	//     const QPointer<QObject> hostObject = DshHost::findObject(DshHostIndex::kMainWindow);
	//     if (auto* window = qobject_cast<VirtualWindow*>(hostObject.data()))
	//         window->ExternalShowOverlay(myWindow);
	//
	// ⚠️ 别改成 qobject_cast<DSHHub*>：那会去链宿主的 staticMetaObject（宿主 exe 的
	//    外部符号且零导出）⇒ 插件 DLL 链接期 LNK2019。也别用 dynamic_cast
	//    （Itanium ABI 下跨模块静默返回 nullptr）。
	//
	// ⚠️ show/hide 必须成对：遮罩在宿主窗口上只留一层、按 owner 记名（见 WindowFrame.h），
	//    只有最后一个 release 的才真正隐藏。
	void ExternalShowOverlay(QWidget* popup) override;
	void ExternalHideOverlay(QWidget* popup) override;

	// ------------------------------------------------------------------
	// VirtualShell 接口的实现（"架空原 UI"）—— 全部只有一行转发
	// ------------------------------------------------------------------
	// 宿主侧刻意**不在本类里写任何逻辑**：让渡动作（摘原生客户区、建舞台、
	// 收宿主浮层、窗口条登记）全在 common/appearance/UiStage.cpp。
	// 这里只做三件事：接上接口、把 owner 从 char* 转成 QString、转发。
	// 所以本文件相对"没有架空功能"的版本只多了这几行 + 一个基类。
	//
	// 内联定义在这里而不是另开 .cpp：这 4 个都是单行转发，另开一个编译单元
	// 只会多一份样板（也省掉往 .vcxproj 里再登一个源文件）。
	QWidget* ExternalAcquireStage(const char* owner) override
	{
		return (owner && *owner) ? UiStage::acquire(this, QString::fromUtf8(owner)) : nullptr;
	}
	bool ExternalReleaseStage(const char* owner) override
	{
		return (owner && *owner) ? UiStage::release(this, QString::fromUtf8(owner)) : false;
	}
	bool ExternalStageAcquired() override
	{
		return UiStage::isTakenOver(this);
	}
	void ExternalSetCaptionBand(int top, int height) override
	{
		UiStage::setCaptionBand(this, top, height);
	}

signals:
	void initializationComplete();

private slots:
	void onNewWorkspaceClicked();
	void onCreateSessionInWorkspace(const QString& workspaceId);
	void onSessionSelected(const QString& sessionId);
	void onDeleteSessionRequested(const QString& sessionId);
	void onClearConversationClicked();
	void toggleTheme();

	void handleConnected();
	/** 一帧 mux 消息：路由（会话事件 / 交互面板）整体归 MessageHost，这里只转发。 */
	void forwardMuxFrame(const QJsonObject& frame);
	void handleTransportError(const QString& context, const QString& message);
	/** mux 上 session/follow 的快照：给历史加载器游标，并用快照播种首屏。 */
	void handleSessionSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore);
	/**
	 * session/follow 快照帧里带的会话投影：输入区下方那行小灰字的主要来源。
	 * 快照上的投影是"全量折叠"（直接从日志算），所以只是打开来看、还没附着 Agent
	 * 的会话也能拿到真实数字。
	 */
	void handleSessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values);

	/** session/control 的 baseline：活会话的现算投影快照（只取当前会话那一份）。 */
	void handleSessionControlBaseline(const QJsonObject& projectionsBySession);

	/** session/control 的实时帧：当前会话的 sessionStats / tokenUsage 变了。 */
	void handleSessionProjectionChanged(const QString& sessionId, const QString& key,
		const QJsonValue& value, int seq);

	/**
	 * 把一份"会话投影"并到小灰字上（两块投影按块记，见成员变量）。
	 *
	 * 为什么必须按块合并而不是整包覆盖：投影是分块来的，而且同一份数据有两个来源 ——
	 *   session/follow 快照：全量折叠，两块都有（打开会话时最先到）
	 *   session/list 行  ：投影缓存检查点，可能只有 tokenUsage、没有 sessionStats
	 * 早先整包覆盖时，后到的列表行会把快照带来的"轮/步 + LLM/工具/首 token"那段擦掉，
	 * 表现就是那一段闪一下然后只剩"缓存命中 / 输入输出"。
	 *
	 * asOfSeq 用服务端那套 higher-seq-wins：旧值直接丢，避免用停得很旧的检查点盖掉新值。
	 */
	void applySessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values);	/** workspace/follow 的 baseline：工作区清单 + 归档集合。 */
	void handleWorkspaceSnapshot(const QJsonArray& items, const QJsonArray& archivedSessionIds);
	/** workspace/follow 的 upsert。 */
	void handleWorkspaceUpserted(const QJsonObject& workspace);
	/** workspace/follow 的 remove。 */
	void handleWorkspaceRemoved(const QString& workspaceId);
	/** workspace/follow 的 order：工作区排序整体替换。 */
	void handleWorkspaceReordered(const QStringList& workspaceIds);
	/** workspace/follow 的 archived（归档集合整体替换）。 */
	void handleWorkspaceArchiveChanged(const QJsonArray& archivedSessionIds);
	/** 把缓存的工作区/归档状态推给侧栏 catalog 并重建视图。 */
	void applyWorkspaceState();

	void onInitialSessionReady(const QString& sessionId, const QString& title);
	void onSessionCreated(const QString& sessionId, const QString& workspaceId);
	void onNoSessionAvailable();
	void onSessionListError(const QString& code, const QString& message);
	void onSessionCreateError(const QString& code, const QString& message);

private:
	void finishInitialization();
	void resizeEvent(QResizeEvent* event) override;
	void moveEvent(QMoveEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void changeEvent(QEvent* event) override;
	// 无边框窗口的原生消息全部转交给 common/WindowFrame 判定
	// （WM_NCCALCSIZE 让客户区铺满窗口、WM_NCHITTEST 判缩放热区与标题栏拖动区）
	bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
	// 窗口状态变化后的界面收尾：切圆角描边、补偿系统多给的一圈、刷新标题栏按钮图标
	void syncWindowFrameStyle();

	// 把当前打开的弹窗重新居中于宿主窗口（拖动/缩放宿主时保持跟随）
	void keepOpenPopupsCentered();

	// ---- 首屏预取（0.1.5：一元 session/page，见 SessionPrefetcher）----
	/** 会话列表刷新后，对可见会话排一轮预取。 */
	void onSessionsRefreshed();
	void openExtensions();

	// 切换到"刚创建的空会话"的统一入口：停流式、交缓存旧会话、换空列表，
	// 并按需绑定/加载 HistoryLoader（消息区那部分都交给 MessageHost::showFreshSession）。
	// loadHistory=true → loader->load(sessionId)；false → adoptSession（等待首条 prompt
	// 的 mux 事件，例如"无会话时直接发送"）。
	void switchToFreshSession(const QString& sessionId, const QString& title, bool loadHistory);
	// 主窗口 UI 搭建（实现位于 src/ui/Main.cpp，减少构造函数体积）
	void buildUi();
	void callSessionCreate();
	// 新建会话该挂到哪个工作区：优先"当前会话所在的那个"，其次退回基线里的第一个。
	// 必须由客户端显式给 —— 服务端 session/create 的响应里没有 workspaceId，而
	// 0.1.5 已删掉 workspace list 这个 RPC，归属只能来自 workspace/follow 基线。
	QString preferredWorkspaceId();
	// 把当前会话同步给输入区（模型/思考深度按该会话的模型目录刷新；
	// 控件那半边在 MessageHost::syncComposerSession，这里只做会话目录查询）
	void syncComposerSession();

	// ------------------------------------------------------------------
	// 输入卡片下方那行小灰字（会话统计）
	// ------------------------------------------------------------------
	// 暂时放在这个 god class 里，等接口稳定了再分离（候选：一个 SessionStatsService
	// 负责取数 + 输入区自己的控制器负责推给控件）。
	//
	// 数据全部来自服务端现算，**客户端不读任何缓存**：
	//   session/follow 快照里的 projections（全量折叠整条日志，冷会话也准）
	//   session/control 的 baseline + projection 帧（活会话快照与实时变化推送）
	// 早先用过 session/list 行，那是投影缓存检查点（冷会话会很旧），已弃用。
	// 没有当前会话时把控件上的字擦掉（高度照旧占着）。
	// ------------------------------------------------------------------
	void clearInteractionPanels();
	void createSessionAndSend(const QString& text);

	void handlePipeRequest(int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket);

	QString m_sessionId;

	// workspace/follow 的本地缓存：baseline 给全量，upsert/remove/archived 增量改它，
	// 每次变化后整体推给侧栏 catalog（catalog 只认全量清单）。
	QJsonArray m_workspaceItems;
	QJsonArray m_workspaceArchived;

	CacheManager m_cacheManager;
	SessionPrefetcher* m_prefetcher = nullptr;

	// 预构建控件树的队列与配额：见 MessageHost（预构建能把"进入会话"变成真正的 cache hit，
	// 但每个控件树要占内存，所以只做有限个）

	QWidget* m_initOverlay = nullptr;
	QLabel* m_initLabel = nullptr;

	// 消息区宿主：拥有当前列表 + HistoryLoader + 预构建队列 + 消息区的 UI 反馈
	// （载入中提示、加载更多按钮显隐、toast）。控件由 buildUi() 搭好后传进去。
	MessageHost* m_messageHost = nullptr;
	Settings* m_settings = nullptr;          // 常驻“设置系统”（自管窗口开关）
	PluginsManager* m_pluginsManager = nullptr;  // 常驻“插件系统”（自管窗口开关）
	// 扩展管理弹窗（遮罩不用单独记：它用的是窗口级那一层，见 WindowFrame.h）
	ExtensionManagerPopup* m_extensionPopup = nullptr;

	bool m_initializationComplete = false;

	QScrollArea* m_scrollArea = nullptr;

	LoadMoreButton* m_loadMoreButton = nullptr;
	QLabel* m_toastLabel = nullptr;
	TopBar* m_topBar = nullptr;
	TitleBar* m_titleBar = nullptr;           // 自绘标题栏（替代系统标题栏）

	Sidebar* m_sidebar = nullptr;

	QVBoxLayout* m_messagesLayout = nullptr;
	ChatInputWidget* m_chatInput = nullptr;

	// 小灰字的合并态：两块投影分别记住（来源可能只带其中一块），
	// 每次变化后整包重算一行文本推给控件；换会话时连同 asOfSeq 一起清空
	QJsonObject m_sessionStatsBlock;   // projections.values.sessionStats
	QJsonObject m_tokenUsageBlock;     // projections.values.tokenUsage
	int m_statsAsOfSeq = -1;           // 已并进来的投影反映到哪个 seq（higher-seq-wins）

	DshApiClient* m_api = nullptr;
	ServerManager* m_serverManager = nullptr;
	DshNamedPipeBridge* m_pipeBridge = nullptr;
	DllCaller* m_dllCaller = nullptr;
	QThreadPool* m_toolPool = nullptr;         // DLL/COM 工具调用线程池（跨扩展并行）
	bool m_cleanupResidualsAfterServerError = false;
};
