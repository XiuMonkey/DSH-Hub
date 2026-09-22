#pragma once

// 消息区宿主：把"消息列表这一整块"的状态与决策从 DSHHub 里拿出来 —— 当前 MessageQuery + HistoryLoader/HistoryManager + 预构建队列 + mux 帧路由（含内联交互面板）+ 消息区 UI 反馈。
// 为什么单独一层：MessageQuery 是"每会话一份、切会话整体替换或被缓存接管"的短命值对象，而滚动区/按钮/队列/缓存/当前会话身份是跨会话存活的单例，所有权放不进多实例对象。
// 它只借用（不拥有，由 DSHHub 建好传进来）：scrollArea / messagesLayout / loadMoreButton / toastLabel / CacheManager / DshApiClient；会话标题与工作区清单归 DSHHub 的侧栏。

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
	MessageHost(DshApiClient* api,
		CacheManager* cache,
		QScrollArea* scrollArea,
		QVBoxLayout* messagesLayout,
		LoadMoreButton* loadMoreButton,
		QLabel* toastLabel,
		ChatInputWidget* chatInput,
		QObject* parent = nullptr);
	~MessageHost() override;

	/** 预取到达后的处置结果（DSHHub 据此决定要不要记"见过的最新 seq"）。 */
	enum class PrefetchOutcome
	{
		Ignored, // 与本会话无关 / 已缓存 / 页太大不值得预构建
		Painted, // 当前会话且界面还空着：已用它点亮
		Queued   // 已排入预构建队列
	};

	/** 当前列表（永远非空）。使用者只读它，不负责它的生命周期。 */
	MessageQuery* current() const { return m_messages; }

	// 会话切换：DSHHub 只说"切到谁"，其余（缓存命中 / 预取 / 拉页 / 分批构建）都在这里决策
	/** 显示某个会话：先把手上的内容交缓存，再依次尝试 控件缓存 → 预取页 → 网络拉取。 */
	void showSession(const QString& sessionId, int fallbackCursor, int observedLastSeq);
	/** 切到"刚创建的空会话"（loadHistory=false 时只接管会话，等首条 prompt 再拉）。 */
	void showFreshSession(const QString& sessionId, bool loadHistory, int fallbackCursor,
		int observedLastSeq);
	void cancelBuild();
	/** session/follow 给的日志游标（= session/page 需要的 throughSeq）。 */
	void setStreamCursor(int cursor);

	/** mux 帧路由：收下一帧（DSHHub 从 DshApiClient::muxFrameReceived 转发过来）。 */
	void handleMuxFrame(const QJsonObject& frame);
	/** 本会话见过的最新事件 seq（缓存元数据与快照新鲜度判定共用）。 */
	int observedLastSeq() const { return m_sessionLastSeq; }
	/** 预取点亮后，窗口侧把这一页的 seq 并进"见过的"（higher wins）。 */
	void noteObservedSeq(int seq) { m_sessionLastSeq = qMax(m_sessionLastSeq, seq); }

	/** 收掉所有内联交互面板（切会话、整表替换、清空会话时都要）。 */
	void clearInteractionPanels();
	/** mux 上 session/follow 的快照：给游标并用记录播种首屏（非当前会话忽略）。 */
	void onFollowSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore);
	/** 丢弃当前实例（会话被删除）：交给缓存丢弃 + 换空实例 + 重置分页显示。 */
	void discardCurrent();
	/** 清空当前列表内容（保留实例）。 */
	void clearCurrent();

	PrefetchOutcome onPrefetched(const QString& sessionId, const QJsonArray& events,
		int throughSeq, bool hasMore);

	// 消息区的 UI 反馈：toast / 载入中提示层 / 几何同步
	void addSystemMessage(const QString& text);
	/** 切换会话期间的"正在载入"提示（只盖聊天区，鼠标穿透）。 */
	void showLoading();
	void hideLoading();
	void syncLoadingGeometry();

	/** mux 上的会话事件：更新气泡内容并维护流式态；返回是否"一次对话收尾"。 */
	enum class StreamOutcome
	{
		Ignored,
		Streaming,
		Finished
	};
	StreamOutcome onStreamEvent(const QJsonObject& event);
	/** 结束流式渲染态（切会话、删除会话、发送新消息前都要复位）。 */
	void stopStreaming();
	/** 布局落定后滚到底并把这一帧一次性画出来（流式跟随底部、整表替换时用）。 */
	void scrollToBottomNow();

signals:
	/** 首屏内容真正上屏（分批构建完成、控件已挂进实时布局）。 */
	void contentReady();
	/** 整列表被替换：窗口侧该收掉交互面板并滚到底。 */
	void contentReplaced();
	/** 一次对话收尾（assistant/message 封存）：窗口侧该刷新会话标题。 */
	void turnFinished();
	/** 没有会话时用户点了发送：建会话要动侧边栏/会话列表，交给 DSHHub，建好后再调 sendPrompt。 */
	void sendWithoutSession(const QString& text);

public slots:
	/** 发出一条 prompt（建好会话后由 DSHHub 回调，或输入区直接触发）。 */
	void sendPrompt(const QString& text);

private:
	HistoryLoader* loader() const { return m_loader; }

	/** 控件缓存命中则恢复并接管分页态；未命中返回 false。 */
	bool restoreFromCache(const QString& sessionId);
	/** 拉起首屏：空实例 + 绑定 loader + 发请求。 */
	void startFreshFetch(const QString& sessionId);
	/** 用预取的一页点亮当前会话（并接管分页态，避免随后快照重建）。 */
	void showPrefetched(const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore);
	/** 把离屏构建好的列表换成当前列表，并把当前实例与分页元数据交给缓存。 */
	void swapInBuilt(MessageQuery* query);
	void handOffToCache(int observedLastSeq);
	void setLoadMoreVisible(bool visible);
	void showNoMoreToast();
	void enqueuePrebuild(const QString& sessionId);
	void processPrebuildQueue();

	void onSendClicked();
	void onStopRequested();
	void updateStreamingUi();
	/** 流式渲染的一帧：暂停重绘 → flush → 落定后刷新。 */
	void flushStreamingFrame();
	/** 消息列先撑够高度再铺布局，避免流式期间气泡被挤扁。 */
	void fitContentThenLayout();
	/** 流式帧落定：布局跑完后再恢复重绘（不跟随底部时用，不动滚动位置）。 */
	void settleStreamingFrame();

	CacheManager* m_cache = nullptr;
	DshApiClient* m_api = nullptr;
	QScrollArea* m_scrollArea = nullptr;
	QVBoxLayout* m_layout = nullptr;
	LoadMoreButton* m_loadMoreButton = nullptr;
	QLabel* m_toastLabel = nullptr;
	ChatInputWidget* m_chatInput = nullptr;

	// 当前实例与它的分页显示状态
	MessageQuery* m_messages = nullptr;
	HistoryManager m_history;
	HistoryLoader* m_loader = nullptr;
	QString m_sessionId; // 消息区当前显示的会话

	// 当前会话见过的最新事件 seq：缓存快照带上它，恢复缓存时用来判断内容是否已被后来的事件超越（超越就重建、没超越就秒开）。
	int m_sessionLastSeq = 0;

	// 内联交互面板（question/approval）：统一回收点 —— 谁挂进布局谁负责摘，所以容器跟着布局一起归这里。
	QList<QWidget*> m_interactionPanels;

	// 预构建控件树队列与配额（每个控件树占内存，所以只做有限个）
	QStringList m_prebuildQueue;
	bool m_prebuilding = false;
	int m_prebuildRemaining = 2;

	// 载入中提示层（懒建，常驻复用；父控件是聊天区 viewport）
	QWidget* m_loadingOverlay = nullptr;
	QLabel* m_loadingLabel = nullptr;
	QTimer* m_loadingWatchdog = nullptr;

	// 流式渲染态：定时器节流 50ms 批量重渲染，避免每个 chunk 都全量重排
	bool m_streaming = false;
	QTimer* m_streamTimer = nullptr;
	bool m_scrollToBottomScheduled = false;

	// 聊天区的滚轮平滑（补间动画）：由滚动区持有、这里只借用 —— 钉滚动位置前先 stop()，并借 isAnimating() 判断"用户正在滚"。
	SmoothWheelScroller* m_wheelScroller = nullptr;
	// 用户"意图"上是否跟随底部：滚轮上滚即关、滚回底部 / 显式滚到底再开（光看滚动位置不行，流式输出时底部会从脚下溜走）。
	bool m_followBottom = true;
};
