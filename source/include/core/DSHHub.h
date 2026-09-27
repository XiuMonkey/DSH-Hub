#pragma once

// 主窗口：窗口 / 侧栏 / 输入区接线，并实现客户端扩展的 VirtualMain 与 VirtualShell 两个宿主接口。

#include <QJsonObject>
#include <QJsonArray>
#include "chat/CacheHistoryManager.h"
// 小灰字（会话统计）的投影合并态：按块合并 + higher-seq-wins，纯 header
#include "common/session/SessionProjectionState.h"
// 工作区基线 + workspace/create、workspace/delete 的编排
#include "common/workspace/WorkplaceManager.h"
// 客户端扩展的全内联虚接口；插件按 index 取到本对象再 qobject_cast
#include "VirtualClass/VirtualCommon.h"
#include "ExtensionSystem/UiStage.h"
#include "common/appearance/WindowFrame.h"
#include <QMainWindow>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <functional>

class DshApiClient;
class DshNamedPipeBridge;
class DllCaller;
class QByteArray;
class QCloseEvent;
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

class DSHHub : public QMainWindow, public VirtualMain, public VirtualShell
{
	Q_OBJECT
	Q_INTERFACES(VirtualMain VirtualShell)

public:
	explicit DSHHub(QWidget* parent = nullptr, const QUrl& initialBaseUrl = QUrl(), QProcess* initialServerProcess = nullptr);
	~DSHHub() override;
	QUrl baseUrl() const;
	// 重建主窗口时必须用它，否则新窗口换不到 cookie 会 401
	QUrl authenticatedBaseUrl() const;

	bool isInitializationComplete() const;

	QProcess* takeServerProcess();

	// VirtualMain 接口：遮罩铺在宿主上、弹窗居中；逻辑全在 WindowFrame.cpp，本类只做转发
	// 插件侧只能 qobject_cast<VirtualMain*>（转 DSHHub* 撞 LNK2019，dynamic_cast 跨模块静默返回 nullptr）
	// owner 用弹窗自身：遮罩按 owner 记名，show/hide 成对就不会串
	void ExternalShowOverlay(QWidget* popup) override
	{
		if (!popup)
			return;

		WindowFrame::showOverlayWithPopup(this, popup, popup);
	}
	void ExternalHideOverlay(QWidget* popup) override
	{
		if (!popup)
			return;

		WindowFrame::hideOverlay(this, popup);
	}

	// VirtualShell 接口（架空原 UI）：逻辑全在 UiStage.cpp，本类只做转发
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

	// VirtualMain 的入站注入面（后端接管专用）：全部转发到下面那批同名私有槽，槽不对外
	void HandleConnected() override {
		handleConnected();
	}
	void ForwardMuxFrame(const QJsonObject& frame) override
	{
		forwardMuxFrame(frame); 
	}
	void HandleTransportError(const QString& context, const QString& message) override
	{
		handleTransportError(context, message);
	}
	void HandleSessionSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore) override
	{
		handleSessionSnapshot(sessionId, cursor, records, hasMore);
	}
	void HandleSessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values) override
	{
		handleSessionProjections(sessionId, asOfSeq, values);
	}
	void HandleSessionControlBaseline(const QJsonObject& projectionsBySession) override
	{
		handleSessionControlBaseline(projectionsBySession);
	}
	void HandleSessionProjectionChanged(const QString& sessionId, const QString& key, const QJsonValue& value, int seq) override
	{
		handleSessionProjectionChanged(sessionId, key, value, seq);
	}
	void HandleWorkspaceSnapshot(const QJsonArray& items, const QJsonArray& archivedSessionIds) override
	{
		handleWorkspaceSnapshot(items, archivedSessionIds);
	}
	void HandleWorkspaceUpserted(const QJsonObject& workspace) override { handleWorkspaceUpserted(workspace); }
	void HandleWorkspaceRemoved(const QString& workspaceId) override { handleWorkspaceRemoved(workspaceId); }
	void HandleWorkspaceReordered(const QStringList& workspaceIds) override { handleWorkspaceReordered(workspaceIds); }
	void HandleWorkspaceArchiveChanged(const QJsonArray& archivedSessionIds) override
	{
		handleWorkspaceArchiveChanged(archivedSessionIds);
	}
	// 只刷标题，逻辑在本类私有槽 refreshSessionTitles（与回合收尾那条路共用同一份实现）
	void ExternalRefreshSessionTitles() override { refreshSessionTitles(); }

signals:
	void initializationComplete();
	// 主窗口真正 close 之前广播一次：宿主有不退进程也不调 detachHost() 的情形，扩展靠它自救
	void aboutToClose();

private slots:
	void onNewWorkspaceClicked();
	void onCreateSessionInWorkspace(const QString& workspaceId);
	void onSessionSelected(const QString& sessionId);
	void onDeleteSessionRequested(const QString& sessionId);
	void onClearConversationClicked();
	void toggleTheme();

	void handleConnected();
	// 只刷标题，不重选会话（供 ExternalRefreshSessionTitles 与 turnFinished 两条路复用）
	void refreshSessionTitles();
	// 一帧 mux 消息：路由整体归 MessageHost，这里只转发
	void forwardMuxFrame(const QJsonObject& frame);
	void handleTransportError(const QString& context, const QString& message);
	// 快照带日志游标，并用来播种首屏
	void handleSessionSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore);
	// 全量折叠算出的投影，还没附着 Agent 的会话也准
	void handleSessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values);
	void handleSessionControlBaseline(const QJsonObject& projectionsBySession);
	void handleSessionProjectionChanged(const QString& sessionId, const QString& key, const QJsonValue& value, int seq);
	// 按块并入小灰字：整包覆盖会擦掉另一份的部分块
	void applySessionProjections(const QString& sessionId, int asOfSeq, const QJsonObject& values);
	void handleWorkspaceSnapshot(const QJsonArray& items, const QJsonArray& archivedSessionIds);
	void handleWorkspaceUpserted(const QJsonObject& workspace);
	void handleWorkspaceRemoved(const QString& workspaceId);
	// 增量帧：order / archived 都是整体替换
	void handleWorkspaceReordered(const QStringList& workspaceIds);
	void handleWorkspaceArchiveChanged(const QJsonArray& archivedSessionIds);
	void applyWorkspaceState();

	void onInitialSessionReady(const QString& sessionId, const QString& title);
	void onSessionCreated(const QString& sessionId, const QString& workspaceId);
	void onNoSessionAvailable();
	void onSessionListError(const QString& code, const QString& message);
	void onSessionCreateError(const QString& code, const QString& message);

private:
	void finishInitialization();
	// 服务端错误上屏：消息区没就绪（启动即失败）就先攒着，等首个会话视图 ready 再补投
	void pushServerNotice(const QString& text);
	void flushPendingServerNotices();
	void resizeEvent(QResizeEvent* event) override;
	void moveEvent(QMoveEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void changeEvent(QEvent* event) override;
	// 广播 aboutToClose()（扩展最早的知情点）→ 调 ClientExtension::shutdownForExit() 同步收尾
	// （这一步是"宿主负责收干净"：插件退场 + 摘出退出卸载路径，详见那个函数的说明），然后转给基类
	void closeEvent(QCloseEvent* event) override;
	// 无边框窗口的原生消息全部转交 common/WindowFrame 判定
	bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
	void syncWindowFrameStyle();
	void keepOpenPopupsCentered();

	void onSessionsRefreshed();
	void openExtensions();

	// 切到新建的空会话；loadHistory=false 时只接管会话，等首条 prompt 的事件
	void switchToFreshSession(const QString& sessionId, const QString& title, bool loadHistory);
	void buildUi();

	// 构造函数的装配步骤，按构造函数里的调用顺序排列
	// 无边框标志 / objectName / 主题 QSS：必须在原生窗口创建之前
	void installWindowShell();
	// spawn 必须最前，Node 启动与后面并行
	void installServer(const QUrl& initialBaseUrl, QProcess* initialServerProcess);
	void installToolRuntime();
	void loadHighlightRules();
	void installInputWiring();
	void installSidebarWiring();
	void installApiWiring();
	void installMessageWiring();
	void installSettingsAndPlugins();
	// 必须最后：插件要查注册表、往已建好的布局挂控件
	void registerHostObjects();
	// 接管态下的三条 DSH 专属旁路开关，由 takeoverChanged 驱动；**不动"扩展管理"**
	void applyTakeoverBypasses(bool takenover);

	void callSessionCreate();
	// 带 workspaceId 发 session/create（没分组时先补建默认工作区再进来，见实现）
	void createSessionIn(const QString& workspaceId);
	// 新会话该挂哪个工作区：优先现有分组；一个分组都没有就先补建默认工作区（接管态除外）。
	// 回调拿到的 id 可能为空 —— 那表示确实没有可归属的工作区，只能落到未分组。
	void resolveSessionWorkspace(const std::function<void(const QString&)>& onResolved);
	// 新建会话挂到哪个工作区：服务端 session/create 响应里没有 workspaceId
	QString preferredWorkspaceId();
	// 把当前会话同步给输入区；控件那半边在 MessageHost
	void syncComposerSession();

	void createSessionAndSend(const QString& text);

	void handlePipeRequest(int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket);

	QString m_sessionId;

	CacheManager m_cacheManager;
	SessionPrefetcher* m_prefetcher = nullptr;

	QWidget* m_initOverlay = nullptr;
	QLabel* m_initLabel = nullptr;

	// 消息区宿主：拥有当前列表 + HistoryLoader + 预构建队列 + 消息区 UI 反馈
	MessageHost* m_messageHost = nullptr;
	Settings* m_settings = nullptr;
	PluginsManager* m_pluginsManager = nullptr;
	ExtensionManagerPopup* m_extensionPopup = nullptr;

	bool m_initializationComplete = false;

	QScrollArea* m_scrollArea = nullptr;
	LoadMoreButton* m_loadMoreButton = nullptr;
	QLabel* m_toastLabel = nullptr;
	TopBar* m_topBar = nullptr;
	TitleBar* m_titleBar = nullptr;
	Sidebar* m_sidebar = nullptr;
	QVBoxLayout* m_messagesLayout = nullptr;
	ChatInputWidget* m_chatInput = nullptr;

	// 小灰字合并态：数据全由服务端现算；换会话时由 syncComposerSession() 调 reset()
	SessionProjectionState m_projectionState;

	DshApiClient* m_api = nullptr;
	// 工作区基线唯一持有者；基线一变就回调 applyWorkspaceState 把投影推给侧栏
	WorkplaceManager m_workplaces;
	ServerManager* m_serverManager = nullptr;
	DshNamedPipeBridge* m_pipeBridge = nullptr;
	DllCaller* m_dllCaller = nullptr;
	QThreadPool* m_toolPool = nullptr;
	bool m_cleanupResidualsAfterServerError = false;

	// 消息区未就绪期间收到的服务端错误文案（启动即失败时 m_messageHost 还是空）
	QStringList m_pendingServerNotices;
};
