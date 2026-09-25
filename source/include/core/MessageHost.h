#pragma once

// 消息区宿主：消息列表这一整块的状态与决策（MessageQuery + HistoryLoader + 预构建队列 + mux 帧路由 +
// UI 反馈）；滚动区 / 按钮 / 缓存 / 会话身份是跨会话存活的单例，只借用不拥有。

#include "chat/CacheHistoryManager.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class CacheManager;
class ChatInputWidget;
class DshApiClient;
class HistoryLoader;
class LoadMoreButton;
class MessageQuery;
class QLabel;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class QWidget;
class SmoothWheelScroller;

class MessageHost : public QObject
{
	Q_OBJECT

public:
	MessageHost(DshApiClient* api, CacheManager* cache, QScrollArea* scrollArea, QVBoxLayout* messagesLayout,
		LoadMoreButton* loadMoreButton, QLabel* toastLabel, ChatInputWidget* chatInput, QObject* parent = nullptr);
	~MessageHost() override;

	// 预取到达后的处置结果（DSHHub 据此决定要不要记"见过的最新 seq"）
	enum class PrefetchOutcome
	{
		Ignored,
		Painted,
		Queued
	};

	MessageQuery* current() const { return m_messages; }

	// 先把手上的内容交缓存，再依次尝试 控件缓存 → 预取页 → 网络拉取
	void showSession(const QString& sessionId, int fallbackCursor, int observedLastSeq);
	void showFreshSession(const QString& sessionId, bool loadHistory, int fallbackCursor, int observedLastSeq);
	void cancelBuild();
	// = session/page 需要的 throughSeq
	void setStreamCursor(int cursor);

	void handleMuxFrame(const QJsonObject& frame);
	int observedLastSeq() const { return m_sessionLastSeq; }
	void noteObservedSeq(int seq) { m_sessionLastSeq = qMax(m_sessionLastSeq, seq); }

	void clearInteractionPanels();
	void onFollowSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore);
	void discardCurrent();
	void clearCurrent();
	PrefetchOutcome onPrefetched(const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore);

	void addSystemMessage(const QString& text);
	void showLoading();
	void hideLoading();
	void syncLoadingGeometry();

	enum class StreamOutcome
	{
		Ignored,
		Streaming,
		Finished
	};
	StreamOutcome onStreamEvent(const QJsonObject& event);
	void stopStreaming();
	void scrollToBottomNow();

signals:
	void contentReady();
	void contentReplaced();
	void turnFinished();
	// 没有会话时用户点了发送：交给 DSHHub 建会话，建好后回调 sendPrompt
	void sendWithoutSession(const QString& text);

public slots:
	void sendPrompt(const QString& text);

private:
	HistoryLoader* loader() const { return m_loader; }

	bool restoreFromCache(const QString& sessionId);
	void startFreshFetch(const QString& sessionId);
	void showPrefetched(const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore);
	void swapInBuilt(MessageQuery* query);
	void handOffToCache(int observedLastSeq);
	void setLoadMoreVisible(bool visible);
	void showNoMoreToast();
	void enqueuePrebuild(const QString& sessionId);
	void processPrebuildQueue();

	void onSendClicked();
	void onStopRequested();
	void updateStreamingUi();
	void flushStreamingFrame();
	void fitContentThenLayout();
	void settleStreamingFrame();

	CacheManager* m_cache = nullptr;
	DshApiClient* m_api = nullptr;
	QScrollArea* m_scrollArea = nullptr;
	QVBoxLayout* m_layout = nullptr;
	LoadMoreButton* m_loadMoreButton = nullptr;
	QLabel* m_toastLabel = nullptr;
	ChatInputWidget* m_chatInput = nullptr;

	MessageQuery* m_messages = nullptr;
	HistoryManager m_history;
	HistoryLoader* m_loader = nullptr;
	QString m_sessionId;

	// 缓存快照带上它，恢复时判断内容是否已被后来的事件超越
	int m_sessionLastSeq = 0;

	// 内联交互面板（question/approval）：谁挂进布局谁负责摘，容器跟着布局归这里
	QList<QWidget*> m_interactionPanels;

	// 预构建控件树队列与配额（每个控件树占内存，只做有限个）
	QStringList m_prebuildQueue;
	bool m_prebuilding = false;
	int m_prebuildRemaining = 2;

	// 载入中提示层（懒建，常驻复用；父控件是聊天区 viewport）
	QWidget* m_loadingOverlay = nullptr;
	QLabel* m_loadingLabel = nullptr;
	QTimer* m_loadingWatchdog = nullptr;

	// 定时器节流 50ms 批量重渲染，避免每个 chunk 都全量重排
	bool m_streaming = false;
	QTimer* m_streamTimer = nullptr;
	bool m_scrollToBottomScheduled = false;

	// 滚轮平滑由滚动区持有、这里只借用：钉滚动位置前先 stop()，并借 isAnimating() 判断用户正在滚
	SmoothWheelScroller* m_wheelScroller = nullptr;
	// 用户"意图"上是否跟随底部；光看滚动位置不行 —— 流式输出时底部会从脚下溜走
	bool m_followBottom = true;
};
