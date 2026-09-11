#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include "CacheHistoryManager.h"

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
class QPushButton;
class AgentMessageUnit;
class MessageQuery;
class HistoryLoader;
class QVBoxLayout;
class QScrollArea;
class QTimer;
class LoadMoreButton;

class DSHHub : public QMainWindow
{
	Q_OBJECT

public:
	explicit DSHHub(QWidget* parent = nullptr,
		const QUrl& initialBaseUrl = QUrl(),
		QProcess* initialServerProcess = nullptr);
	~DSHHub() override;
	QUrl baseUrl() const;

	bool isInitializationComplete() const;

	QProcess* takeServerProcess();
	void adoptServerProcess(QProcess* process);

signals:
	void initializationComplete();

private slots:
	void onSendClicked();
	void onStopRequested();
	void onNewWorkspaceClicked();
	void onCreateSessionInWorkspace(const QString& workspaceId);
	void onSessionSelected(const QString& sessionId);
	void onDeleteSessionRequested(const QString& sessionId);
	void onClearConversationClicked();
	void toggleTheme();

	void handleConnected();
	void handleMuxFrame(const QJsonObject& frame);
	void handleTransportError(const QString& context, const QString& message);

	void onInitialSessionReady(const QString& sessionId, const QString& title);
	void onSessionCreated(const QString& sessionId, const QString& workspaceId);
	void onNoSessionAvailable();
	void onSessionListError(const QString& code, const QString& message);
	void onSessionCreateError(const QString& code, const QString& message);
	void onHistoryLoadMoreButtonVisibleChanged(bool visible);
	void onHistoryError(const QString& code, const QString& message);
	void onIncrementalBuildReady(MessageQuery* query);

private:
	void swapToMessageQuery(MessageQuery* query);
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

	void scrollToBottomNow();

	void openExtensions();

	void cacheCurrentMessages();
	// 切换到"刚创建的空会话"的统一入口：取消旧构建、停流式、缓存当前会话、
	// 换空列表、重置分页状态并按需绑定/加载 HistoryLoader。
	// loadHistory=true → loader->load(sessionId)；false → adoptSession（等待首条 prompt
	// 的 mux 事件，例如"无会话时直接发送"）。
	void switchToFreshSession(const QString& sessionId, const QString& title, bool loadHistory);
	// 主窗口 UI 搭建（实现位于 src/ui/Main.cpp，减少构造函数体积）
	void buildUi();
	void callSessionCreate();
	void sendPrompt(const QString& text);
	void updateStreamingUi();
	// 把当前会话同步给输入区（底部“思考深度”控件按该会话的模型目录刷新）
	void syncComposerSession();
	// 流式渲染的一帧：暂停重绘 → flush → 落定后刷新（见实现处注释）
	void flushStreamingFrame();
	// 消息列先撑够高度再铺布局，避免流式期间气泡被挤扁（见实现处注释）
	void fitContentThenLayout();
	// 流式帧落定：布局跑完后再恢复重绘（不跟随底部时用，不动滚动位置）
	void settleStreamingFrame();
	void clearInteractionPanels();
	void createSessionAndSend(const QString& text);
	void onHistoryPrefetched(const QString& sessionId, const QJsonArray& events);
	void processPrebuildQueue();

	bool tryRestoreCachedMessages(const QString& sessionId);

	void showNoMoreToast();

	void handlePipeRequest(int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket);

	HistoryManager m_history;
	bool m_usingPrefetched = false;

	QString m_sessionId;
	QString m_defaultAgentPreset;

	bool m_streaming = false;
	QTimer* m_streamTimer = nullptr;

	CacheManager m_cacheManager;
	SessionPrefetcher* m_prefetcher = nullptr;

	QWidget* m_initOverlay = nullptr;
	QLabel* m_initLabel = nullptr;
	Settings* m_settings = nullptr;          // 常驻“设置系统”（自管窗口开关）
	PluginsManager* m_pluginsManager = nullptr;  // 常驻“插件系统”（自管窗口开关）
	QWidget* m_extensionOverlay = nullptr;
	ExtensionManagerPopup* m_extensionPopup = nullptr;

	bool m_initializationComplete = false;
	bool m_scrollToBottomScheduled = false;

	QScrollArea* m_scrollArea = nullptr;

	LoadMoreButton* m_loadMoreButton = nullptr;
	QLabel* m_toastLabel = nullptr;
	MessageQuery* m_messages = nullptr;
	TopBar* m_topBar = nullptr;
	TitleBar* m_titleBar = nullptr;           // 自绘标题栏（替代系统标题栏）

	HistoryLoader* m_historyLoader = nullptr;

	QStringList m_prebuildQueue;
	bool m_prebuilding = false;

	Sidebar* m_sidebar = nullptr;

	QVBoxLayout* m_messagesLayout = nullptr;
	ChatInputWidget* m_chatInput = nullptr;
	QList<QWidget*> m_interactionPanels;

	DshApiClient* m_api = nullptr;
	ServerManager* m_serverManager = nullptr;
	DshNamedPipeBridge* m_pipeBridge = nullptr;
	DllCaller* m_dllCaller = nullptr;
	QThreadPool* m_toolPool = nullptr;         // DLL/COM 工具调用线程池（跨扩展并行）
	bool m_cleanupResidualsAfterServerError = false;
};
