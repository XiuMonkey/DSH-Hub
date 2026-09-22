#pragma once

// ------------------------------------------------------------------
// MessageHost.h
// ------------------------------------------------------------------
// 消息区宿主：把"消息列表这一整块"的状态与决策从 DSHHub 里拿出来。
//
// 它拥有（跨会话存活的那些）：
//   * 当前 MessageQuery 实例——永远非空；切会话整体替换，交给缓存时换新实例
//   * HistoryLoader（历史拉取 / 分页 / 离屏分批构建）+ HistoryManager（条数、是否有更多）
//   * 预构建控件树队列（配额有限，每轮事件循环只建一个）
//   * 消息区的 UI 反馈：载入中提示层、加载更多按钮的显隐、toast
//   * 消息区的分页显示状态（原始事件数 / 是否有更早内容），缓存元数据就取自这里
// 它只借用（不拥有，由 DSHHub 的界面代码创建后传进来）：
//   scrollArea / messagesLayout / loadMoreButton / toastLabel / CacheManager / DshApiClient
//
// 为什么这些不放进 MessageQuery 自己：MessageQuery 是"每个会话一份、切会话就整体
// 替换或被缓存接管"的短命值对象，而这里管的是跨会话存活的单例（滚动区、按钮、
// 队列、缓存、当前会话身份）——单例的所有权放不进多实例对象。所以宿主单独一层，
// MessageQuery 一行不改。
//
// 对外只有两件事：DSHHub 让它"显示哪个会话 / 收下哪个快照"；它回报
//   * contentReady()     首屏内容真正上屏（DSHHub 借此收启动遮罩）
//   * contentReplaced()  整列表被替换（窗口侧该收交互面板、滚到底）
//   * turnFinished()     一次对话收尾（DSHHub 借此刷新侧栏标题）
//
// 另有一块：mux 帧路由（handleMuxFrame）。原先在 DSHHub 里，拆出来后帧的
// 第一个消费者就是这里——DSHHub 只把 DshApiClient::muxFrameReceived 接过来转发。
// 它拥有：
//   * 当前会话见过的最新事件 seq（缓存新鲜度判定用）
//   * 内联交互面板（question/approval）的容器与其回收
// 不再碰的：会话标题、工作区清单（都归 DSHHub 的侧栏）。
// ------------------------------------------------------------------

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
	/** 消息列表所在的布局（往列表里插交互面板等用）。 */
	QVBoxLayout* layout() const { return m_layout; }
	/** 当前列表里还没有任何气泡（新会话 / 历史还在路上）。 */
	bool empty() const;

	// ---- 会话切换：DSHHub 只说"切到谁"，其余（缓存命中/预取/拉页/分批构建）在这里决策 ----
	/** 显示某个会话：先把手上的内容交缓存，再依次尝试 控件缓存 → 预取页 → 网络拉取。 */
	void showSession(const QString& sessionId, int fallbackCursor, int observedLastSeq);
	/** 切到"刚创建的空会话"（loadHistory=false 时只接管会话，等首条 prompt 再拉）。 */
	void showFreshSession(const QString& sessionId, bool loadHistory, int fallbackCursor,
		int observedLastSeq);
	/** 取消未完成的构建（切会话、丢弃会话时用）。 */
	void cancelBuild();
	/** session/follow 给的日志游标（= session/page 需要的 throughSeq）。 */
	void setStreamCursor(int cursor);

	// ---- mux 帧路由 ----
	/** 收下一帧 mux 消息（DSHHub 从 DshApiClient::muxFrameReceived 转发过来）。 */
	void handleMuxFrame(const QJsonObject& frame);
	/** 本会话见过的最新事件 seq（缓存元数据与快照新鲜度判定共用）。 */
	int observedLastSeq() const { return m_sessionLastSeq; }
	/** 预取点亮后，窗口侧把这一页的 seq 并进"见过的"（higher wins）。 */
	void noteObservedSeq(int seq) { m_sessionLastSeq = qMax(m_sessionLastSeq, seq); }

	// ---- 内联交互面板 ----
	/** 收掉所有内联交互面板（切会话、整表替换、清空会话时都要）。 */
	void clearInteractionPanels();
	/** mux 上 session/follow 的快照：给游标并用记录播种首屏（非当前会话忽略）。 */
	void onFollowSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore);
	/** 丢弃当前实例（会话被删除）：交给缓存丢弃 + 换空实例 + 重置分页显示。 */
	void discardCurrent();
	/** 清空当前列表内容（保留实例）。 */
	void clearCurrent();

	// ---- 预取 ----
	PrefetchOutcome onPrefetched(const QString& sessionId, const QJsonArray& events,
		int throughSeq, bool hasMore);

	// ---- 消息区 UI 反馈 ----
	void addSystemMessage(const QString& text);
	/** 切换会话期间的"正在载入"提示（只盖聊天区，鼠标穿透）。 */
	void showLoading();
	void hideLoading();
	/** 窗口尺寸变化时同步提示层的几何。 */
	void syncLoadingGeometry();

	// ---- 流式渲染与输入区 ----
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
	/** 输入区底部的"模型 / 思考深度"按当前会话刷新（空值=无会话选择）。 */
	void syncComposerSession(const QString& provider, const QString& model, const QString& effort);
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
	/** 分页用的原始事件数（写缓存元数据用）。 */
	int rawEventCount() const;
	bool hasMore() const;

	/** 控件缓存命中则恢复并接管分页态；未命中返回 false。 */
	bool restoreFromCache(const QString& sessionId);
	/** 拉起首屏：空实例 + 绑定 loader + 发请求。 */
	void startFreshFetch(const QString& sessionId);
	/** 用预取的一页点亮当前会话（并接管分页态，避免随后快照重建）。 */
	void showPrefetched(const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore);
	/** 把离屏构建好的列表换成当前列表。 */
	void swapInBuilt(MessageQuery* query);
	/** 把当前实例与分页元数据交给缓存。 */
	void handOffToCache(int observedLastSeq);
	void setLoadMoreVisible(bool visible);
	void showNoMoreToast();
	void enqueuePrebuild(const QString& sessionId);
	void processPrebuildQueue();

	// ---- 输入区 / 流式渲染（内部）----
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

	// 当前会话见过的最新事件 seq：缓存快照带上它，恢复缓存时用来判断
	// 缓存内容是否已被后来的事件超越（超越就得重建，没超越就秒开）。
	// 原先住在 DSHHub，随 mux 帧路由一起搬来——它是 mux 观测，不该由窗口持有。
	int m_sessionLastSeq = 0;

	// 内联交互面板（question/approval）。建出来时收进来，切会话/整表替换/
	// 清空会话时统一回收：谁挂进布局谁负责摘，所以容器跟着布局一起归这里。
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

	// 聊天区的滚轮平滑（补间动画）。它由滚动区持有（父对象是滚动区），
	// 这里只借用：钉滚动位置前先 stop()，并借 isAnimating() 判断"用户正在滚"。
	SmoothWheelScroller* m_wheelScroller = nullptr;
	// 用户"意图"上是否跟随底部：滚轮上滚即关，滚回底部 / 显式滚到底再开。
	// 光看滚动位置不行——流式输出时内容一直在长高，底部会从脚下溜走。
	bool m_followBottom = true;
};
