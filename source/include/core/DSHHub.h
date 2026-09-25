#pragma once

// 主窗口：窗口 / 侧栏 / 输入区接线，并实现客户端扩展的 VirtualWindow 与 VirtualShell 两个宿主接口。

#include <QJsonObject>
#include <QJsonArray>
#include "chat/CacheHistoryManager.h"

// 小灰字（会话统计）的投影合并态：按块合并 + higher-seq-wins，纯 header
#include "common/session/SessionProjectionState.h"

// 客户端扩展的全内联虚接口（本窗口实现其中的 VirtualWindow）：插件按 index 取到本对象后 qobject_cast 成接口再用。
#include "VirtualClass/VirtualCommon.h"

// 架空（VirtualShell）的让渡动作本体，与客户端扩展装载同属一套。
#include "ExtensionSystem/UiStage.h"

#include <QMainWindow>
#include <QProcess>
#include <QString>
#include <QStringList>

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
	// 带认证令牌的 baseUrl：重建主窗口但仍复用同一服务端（切主题就是重建窗口）时必须用它，否则新窗口换不到 cookie 会 401 卡住。
	QUrl authenticatedBaseUrl() const;

	bool isInitializationComplete() const;

	QProcess* takeServerProcess();

	// VirtualWindow 接口（客户端扩展唯一入口）：插件用顶层窗口铺遮罩 + 居中当宿主弹窗；⚠️ 插件侧只能 qobject_cast<VirtualWindow*>（转 DSHHub* 撞 LNK2019，dynamic_cast 跨模块静默返回 nullptr）。
	void ExternalShowOverlay(QWidget* popup) override;
	void ExternalHideOverlay(QWidget* popup) override;

	// VirtualShell 接口（"架空原 UI"）：全是单行转发，逻辑都在 ExtensionSystem/UiStage.cpp（摘原生客户区 / 建舞台 / 收宿主浮层 / 窗口条登记），本类只做接口接线 + char*→QString。
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
	/** session/follow 快照里的会话投影：全量折叠算出，所以还没附着 Agent 的会话也准；输入区下方小灰字的主要来源。 */
	void handleSessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values);

	/** session/control 的 baseline：活会话的现算投影快照（只取当前会话那一份）。 */
	void handleSessionControlBaseline(const QJsonObject& projectionsBySession);

	/** session/control 的实时帧：当前会话的 sessionStats / tokenUsage 变了。 */
	void handleSessionProjectionChanged(const QString& sessionId, const QString& key,
		const QJsonValue& value, int seq);

	/** 把一份会话投影按块并入小灰字（follow 快照与 list 行各带部分块，整包覆盖会擦掉快照那段）；asOfSeq 用服务端 higher-seq-wins。 */
	void applySessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values);
	/** workspace/follow 的 baseline：工作区清单 + 归档集合。 */
	void handleWorkspaceSnapshot(const QJsonArray& items, const QJsonArray& archivedSessionIds);
	void handleWorkspaceUpserted(const QJsonObject& workspace);
	void handleWorkspaceRemoved(const QString& workspaceId);
	/** workspace/follow 的增量帧：order / archived（两者都是整体替换）。 */
	void handleWorkspaceReordered(const QStringList& workspaceIds);
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
	// 无边框窗口的原生消息全部转交 common/WindowFrame 判定（WM_NCCALCSIZE 铺满客户区 / WM_NCHITTEST 判缩放热区与标题栏拖动区）
	bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
	// 窗口状态变化后的界面收尾：切圆角描边、补偿系统多给的一圈、刷新标题栏按钮图标
	void syncWindowFrameStyle();

	// 把当前打开的弹窗重新居中于宿主窗口（拖动/缩放宿主时保持跟随）
	void keepOpenPopupsCentered();

	/** 会话列表刷新后，对可见会话排一轮预取（见 SessionPrefetcher）。 */
	void onSessionsRefreshed();
	void openExtensions();

	// 切到"刚创建的空会话"的统一入口：停流式、交缓存旧会话、换空列表；loadHistory=false 时只接管会话，等首条 prompt 的 mux 事件。
	void switchToFreshSession(const QString& sessionId, const QString& title, bool loadHistory);
	// 主窗口 UI 搭建（实现位于 src/ui/Main.cpp，减少构造函数体积）
	void buildUi();

	// ---- 构造函数的装配步骤（实现位于 src/core/DSHHub.cpp，按构造函数里的调用顺序排列）----
	// 无边框标志 / objectName / 主题 QSS：必须在原生窗口创建之前
	void installWindowShell();
	// 服务端进程：创建 + DSHHub.001-004 接线 + spawn（spawn 必须最前，Node 启动与后面并行）
	void installServer(const QUrl& initialBaseUrl, QProcess* initialServerProcess);
	// 工具运行时：DLL 调用线程池 + 命名管道桥（005）+ 工具扩展 DLL 装载
	void installToolRuntime();
	// Qt 资源里的代码高亮规则
	void loadHighlightRules();
	// UI 接线四组（按 sender 分：输入区 / 侧栏 / mux-api / 消息区+预取）
	void installInputWiring();
	void installSidebarWiring();
	void installApiWiring();
	void installMessageWiring();
	// 常驻"设置系统"/"插件系统"：创建 + 041-044 接线 + 顶栏 baseUrl 现取回调
	void installSettingsAndPlugins();
	// 登记宿主对象 + 装载客户端扩展（必须最后：插件要查注册表、往已建好的布局挂控件）
	void registerHostObjects();
	// 后端接管态下的三条 DSH 专属旁路开关（工具过滤 / 插件市场 / 启动 DSH 进程），
	// 由 DshApiClient::takeoverChanged 驱动；幂等。**不动"扩展管理"**。
	void applyTakeoverBypasses(bool takenover);

	void callSessionCreate();
	// 新建会话挂到哪个工作区：优先"当前会话所在的那个"，其次退回基线里的第一个（服务端 session/create 响应里没有 workspaceId，归属只能来自 workspace/follow 基线）。
	QString preferredWorkspaceId();
	// 把当前会话同步给输入区（模型 / 思考深度按该会话的模型目录刷新；控件那半边在 MessageHost，这里只做会话目录查询）
	void syncComposerSession();

	void createSessionAndSend(const QString& text);

	void handlePipeRequest(int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket);

	QString m_sessionId;

	// workspace/follow 的本地缓存：baseline 给全量，upsert/remove/archived 增量改它，每次变化后整体推给侧栏 catalog（catalog 只认全量清单）。
	QJsonArray m_workspaceItems;
	QJsonArray m_workspaceArchived;

	CacheManager m_cacheManager;
	SessionPrefetcher* m_prefetcher = nullptr;

	QWidget* m_initOverlay = nullptr;
	QLabel* m_initLabel = nullptr;

	// 消息区宿主：拥有当前列表 + HistoryLoader + 预构建队列 + 消息区的 UI 反馈；控件由 buildUi() 搭好后传进去。
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

	// 小灰字（会话统计）的合并态：数据全部服务端现算、客户端不读任何缓存。
	// 两块投影按块记 + higher-seq-wins，规则见 common/session/SessionProjectionState.h；
	// 换会话时由 syncComposerSession() 调 reset()。
	SessionProjectionState m_projectionState;

	DshApiClient* m_api = nullptr;
	ServerManager* m_serverManager = nullptr;
	DshNamedPipeBridge* m_pipeBridge = nullptr;
	DllCaller* m_dllCaller = nullptr;
	QThreadPool* m_toolPool = nullptr;         // DLL/COM 工具调用线程池（跨扩展并行）
	bool m_cleanupResidualsAfterServerError = false;
};
