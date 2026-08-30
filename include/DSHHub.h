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
class DllJsonCaller;
class QLabel;
class QLocalSocket;
class QUrl;
class QResizeEvent;
class ServerManager;
class SessionPrefetcher;
class ChatInputWidget;
class Sidebar;
class TopBar;
class Settings;
class PluginsPopup;
class ExtensionManagerPopup;
class QPushButton;
class AgentMessageUnit;
class MessageQuery;
class HistoryLoader;
class QVBoxLayout;
class QScrollArea;
class QTimer;
class LoadingCard;
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
	void onHistoryLoadingChanged(bool loading);
	void onHistoryLoadMoreButtonVisibleChanged(bool visible);
	void onHistoryError(const QString& code, const QString& message);
	void onIncrementalBuildReady(MessageQuery* query);

private:
	void hideLoadingIndicator();
	void swapToMessageQuery(MessageQuery* query);
	void finishInitialization();
	void resizeEvent(QResizeEvent* event) override;

	void scrollToBottomNow();

	void openSettings();
	void closeSettings();
	void openPlugins();
	void openExtensions();

	void cacheCurrentMessages();
	void callSessionCreate();
	void sendPrompt(const QString& text);
	void updateStreamingUi();
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
	QWidget* m_settingsOverlay = nullptr;
	Settings* m_settings = nullptr;
	QWidget* m_pluginsOverlay = nullptr;
	PluginsPopup* m_pluginsPopup = nullptr;
	QWidget* m_extensionOverlay = nullptr;
	ExtensionManagerPopup* m_extensionPopup = nullptr;

	bool m_initializationComplete = false;
	bool m_scrollToBottomScheduled = false;

	QScrollArea* m_scrollArea = nullptr;

	LoadMoreButton* m_loadMoreButton = nullptr;
	QLabel* m_toastLabel = nullptr;
	LoadingCard* m_loadingCard = nullptr;
	QWidget* m_loadingContainer = nullptr;
	MessageQuery* m_messages = nullptr;
	TopBar* m_topBar = nullptr;

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
	DllJsonCaller* m_dllCaller = nullptr;
	bool m_cleanupResidualsAfterServerError = false;
};
