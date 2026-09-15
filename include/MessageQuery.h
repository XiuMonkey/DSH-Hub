#pragma once

// ------------------------------------------------------------------
// MessageQuery.h
// ------------------------------------------------------------------
// 会话消息列表（MessageQuery / MessageQueryBuilder）与它的历史加载器
// （HistoryLoader）。
//
// dsh 0.1.5 的历史模型（与旧版完全不同，改动集中在这里）：
//
//   1) 首屏不来自 RPC，而是来自 mux 上 session/follow 的首帧快照：
//        {type:"snapshot", cursor, records[], hasMore}
//      cursor 是"日志切割点"，records 是最近一页事件。HistoryLoader 只播种其中
//      最近 kSeedEventCap 条（快照一页可能有几十条消息，全量构建会明显拖慢切会话）。
//
//   2) 上翻更早的内容走 session/page（一元 RPC，必须带 throughSeq）：
//        {request:{address, throughSeq, beforeSeq?, maxMessages?}}
//      throughSeq = 快照的 cursor；beforeSeq = 当前内容里最早一条事件的 seq
//      （m_oldestSeq，排他上界）；maxMessages 数的是**消息**不是事件。
//      返回的记录**全部**比 beforeSeq 更早，所以整页插到顶部即可，
//      不需要旧版那种"比条数取差集"。
//
//   3) 游标可能来得晚：load() 在拿到游标前会挂起（m_loadPending），并起一个
//      看门狗。超时后若 DSHHub 喂过回落游标（session/list 的 projections.asOfSeq）
//      就直接用它发请求；否则明确报错（loadingChanged(false) + historyError），
//      而不是让 UI 一直转圈。
//
//   4) 缓存命中要能"秒开"：恢复缓存时 adoptCachedState() 把缓存建立时的
//      throughSeq/oldestSeq/内容最新 seq 装回 loader；随后到达的快照如果
//      不比缓存内容新，就不重建（seedFromSnapshot 的新鲜度判定）。
// ------------------------------------------------------------------

#include "AgentMessageUnit.h"
#include "SystemMessageUnit.h"
#include "UserMessageUnit.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <vector>

class QVBoxLayout;
class DshApiClient;
class HistoryManager;
class QScrollArea;

class MessageQuery
{
public:
	struct MessageUnit
	{
		int type = 0; // 0: user, 1: agent, 2: system
		AgentMessageUnit* agentUnit = nullptr;
		UserMessageUnit* userUnit = nullptr;
		SystemMessageUnit* systemUnit = nullptr;
		QWidget* container = nullptr;
	};

	std::vector<MessageUnit> messages;
	bool m_bulkFitting = false; // 批量构建期间抑制逐次高度拟合（见 setBulkFitting）

	MessageQuery();
	~MessageQuery();

	UserMessageUnit* addUserMessage(const QString& text, QVBoxLayout* layout);
	SystemMessageUnit* addSystemMessage(const QString& text, QVBoxLayout* layout);
	AgentMessageUnit* addAgentMessage(const QString& markdown, QVBoxLayout* layout,
		const QString& thinking = QString());
	AgentMessageUnit* lastAgentUnit() const;
	AgentMessageUnit* lastAgentUnitIfLast() const;
	void clear();

	// 把 DSH 历史事件渲染进当前消息列表
	void appendEvents(QVBoxLayout* layout, const QJsonArray& events);

	// —— 实况流式/事件 → 气泡更新的统一入口 ——
	// 历史回放（appendEvents）与实况帧共用同一套"事件→内容"的心智。
	// DSHHub 只负责会话路由与 streaming/timer 策略，这里返回结果告诉它该做什么。
	struct StreamFrameResult
	{
		enum Kind
		{
			Ignored,        // 事件不需要更新气泡（user/message、未知类型等）
			Streaming,      // 收到流式内容（chunk/tool…），控制器应启动定时器并置 streaming
			FinalMessage    // assistant/message 收尾：内容已合并/封存
		};
		Kind kind = Ignored;
		bool contentRouted = false; // 是否真的往气泡里写了内容（工具/文本等）
	};

	// event: 已解包的事件对象；layout: 新建气泡的宿主布局；
	// wasStreaming: 调用前控制器是否处于流式态（assistant/message 收尾时用于去重）
	StreamFrameResult applyStreamEvent(const QJsonObject& event, QVBoxLayout* layout, bool wasStreaming);

	// 从历史事件离线构建一个完整 MessageQuery，调用方负责后续 attach/释放
	static MessageQuery* fromEvents(const QJsonArray& events);

	// 控件缓存支持：从布局中摘下但不销毁，之后可以重新 attach 回来
	void detachFromLayout(QVBoxLayout* layout);
	void attachToLayout(QVBoxLayout* layout);
	// 把离屏构建完成的"更早一页"older 插到本列表/布局顶部（layoutIndex 起），
	// 并接管 older 的所有权（调用后 older 不可再用）。用于"加载更多"。
	void prependQuery(QVBoxLayout* layout, MessageQuery* older, int layoutIndex);

	// 批量构建模式：开启期间新建的 Agent 气泡不逐次做高度拟合/排队 refit，
	// 由 MessageQueryBuilder 整页构建完成后统一关闭并拟合一次。
	void setBulkFitting(bool bulk) { m_bulkFitting = bulk; }

	// 把内部控件从当前父对象上解除，便于从临时离屏容器安全迁移
	void releaseWidgets();
};

// 离屏增量构建消息列表，避免一次性渲染大量历史控件卡顿。
class MessageQueryBuilder
{
public:
	MessageQueryBuilder();
	~MessageQueryBuilder();

	// 开始用 events 构建一个新的 MessageQuery
	void start(const QJsonArray& events);
	// 处理下一批；返回 true 表示仍在构建，false 表示已完成
	bool step(int batchSize = 5);
	// 取消并释放临时控件
	void cancel();
	bool isActive() const;
	// 取出构建完成的 MessageQuery，调用方负责后续 attach/释放
	MessageQuery* takeResult();

private:
	QWidget* m_holder = nullptr;
	QVBoxLayout* m_layout = nullptr;
	MessageQuery* m_query = nullptr;
	QJsonArray m_events;
	int m_index = 0;
	bool m_active = false;
};

// 历史消息加载器：负责 session.history 拉取、增量构建、加载更多
class HistoryLoader : public QObject
{
	Q_OBJECT

public:
	HistoryLoader(DshApiClient* api,
		MessageQuery* messages,
		QVBoxLayout* layout,
		HistoryManager* history,
		QScrollArea* scrollArea,
		QObject* parent = nullptr);

	void load(const QString& sessionId);
	void loadMore();
	void setMessages(MessageQuery* messages);
	void cancelBuild();

	/**
	 * session/follow 快照给的日志游标（= session/page 需要的 throughSeq）。
	 *
	 * 0.1.5 的分页端点必须带 throughSeq，而它只能从 follow 快照拿到，
	 * 所以首屏请求会在拿到游标前挂起（m_loadPending），由本函数唤醒。
	 */
	void setStreamCursor(int cursor);

	/**
	 * 用 session/follow 的快照记录直接播种首屏历史（省掉一次 session/page 往返）。
	 * @param sessionId 快照所属会话；不是当前会话时忽略（切走后迟到的帧）。
	 * @param cursor    该会话的 throughSeq。
	 * @param records   SessionFollowFrame 的 records（{type:"event",event:{…}}）。
	 * @param hasMore   服务端是否还有更早的历史。
	 */
	void seedFromSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore);

	/** 当前分页游标（follow 快照给的 throughSeq）；缓存快照要用。 */
	int streamCursor() const { return m_throughSeq; }

	/** 当前内容里最早一条事件的 seq（"加载更多"的 beforeSeq）；缓存快照要用。 */
	int oldestSeq() const { return m_oldestSeq; }

	/**
	 * follow 快照迟迟不来时用来当 throughSeq 的回落值。
	 *
	 * 来源是 session/list 行的 projections.asOfSeq（DSHHub 在切会话时喂进来）。
	 * 0 表示没有回落值：这时看门狗超时只能报错，而不能发请求
	 * （服务端要求 throughSeq 必填，缺了会 gateway/input-invalid）。
	 */
	void setFallbackCursor(int cursor);

	/**
	 * 用缓存的时间点接管分页状态。
	 *
	 * 恢复缓存（内容仍有效）时调用：把游标/最早 seq/内容最新 seq 设成缓存建立时的值
	 * 并把 m_seeded 置真，这样随后到达的 follow 快照只要不比缓存内容新，就不会触发
	 * 重建 —— 保住"缓存命中即秒开"；一旦快照比缓存新，seedFromSnapshot 会照常重建。
	 *
	 * @param contentLastSeq 缓存内容里最新一条事件的 seq（含游标之后的实时事件），
	 *                       新鲜度以它为界。
	 */
	void adoptCachedState(int throughSeq, int oldestSeq, int contentLastSeq,
		int eventCount, bool hasMore);

	/**
	 * 用"预取的一页事件"播种首屏（数据来自本地，不经过流）。
	 *
	 * 与 seedFromSnapshot 的区别：
	 *   * 不做新鲜度判定——调用方只在"控件缓存未命中"时才走这里；
	 *   * 直接走分批构建（startBuild），而不是一次性贴预构建的控件树：
	 *     整树一次性冷布局会造成单帧阻塞（实测 116 条 ≈ 307ms），
	 *     分批构建则是每轮事件循环 5 条，不卡帧。
	 */
	void seedFromPrefetched(const QString& sessionId, const QJsonArray& events,
		int throughSeq, bool hasMore);

	// 只把 loader 绑定到某会话（恢复缓存、不重新拉取时使用）：
	// 不发起请求、不渲染；之后点击"加载更多"即可直接以该会话分页。
	void adoptSession(const QString& sessionId)
	{
		if (m_sessionId != sessionId) {
			m_builder.cancel();
			m_sessionId = sessionId;
			m_loading = false;
			m_reachedEnd = false;
			m_seeded = false;
			m_loadPending = false;
			m_throughSeq = 0;
			m_oldestSeq = 0;
			m_contentLastSeq = 0;
		}
	}

signals:
	void loadingChanged(bool loading);
	void loadMoreButtonVisibleChanged(bool visible);
	void noMoreHistory();
	void historyError(const QString& code, const QString& message);
	void incrementalBuildReady(MessageQuery* query);
	// 首屏内容真正上屏（分批构建完成、控件已挂进实时布局）。
	// DSHHub 借此收掉启动遮罩——不能在"开始构建"时就收，否则会先露出空白聊天区。
	void firstHistoryArrived();

private:
	void continueBuild();
	void startBuild(const QJsonArray& events);
	// “加载更多”的离屏分批构建：更早的一页先在离屏 builder 里分片渲染，
	// 完成后一次性插到活布局顶部，避免主线程一次渲染超大页。
	void startPrepend(const QJsonArray& olderEvents);
	// 首屏构建与“加载更多”老页构建的公共入口
	void startPageBuild(const QString& logLabel, const QJsonArray& events, bool prepend);
	// 老页插入后按插入前记录的锚点校正滚动（分多次直到几何稳定）
	void applyPendingAnchor();

	DshApiClient* m_api = nullptr;
	MessageQuery* m_messages = nullptr;
	QVBoxLayout* m_layout = nullptr;
	HistoryManager* m_history = nullptr;
	QScrollArea* m_scrollArea = nullptr;
	MessageQueryBuilder m_builder;
	QString m_sessionId;
	QString m_buildSessionId;
	int m_loadGeneration = 0;
	int m_buildGeneration = 0;
	// “已到历史顶部”标志：只由服务端回包驱动——
	// 某次请求返回条数 ≤ 已有条数时置 true；拉到了更多则置 false。
	// 不要用 m_history->hasMore() 做门控（部分流程里该值与服务端实际不符，
	// 会导致“明明还有更多却一直提示没有更多”）。
	bool m_reachedEnd = false;

	// 0.1.5 分页状态：throughSeq 来自 session/follow 快照；
	// 快照还没到就先挂起首屏请求（m_loadPending），
	// 快照到了就直接播种（m_seeded 之后不再走首屏 RPC）。
	int m_throughSeq = 0;
	// 当前已加载内容里最早一条事件的 seq：上翻一页时作为 session/page 的 beforeSeq。
	int m_oldestSeq = 0;
	// follow 快照没到时的 throughSeq 回落值（session/list 的 projections.asOfSeq）
	int m_fallbackCursor = 0;
	// 当前内容的"最新位置"：缓存恢复时用来判断缓存是否已被后来事件超越
	// （缓存内容里可能有游标之后的实时事件，所以单独记一个数）
	int m_contentLastSeq = 0;
	// 等游标的看门狗：超时后用回落值发请求，或明确报错而不是一直转圈
	QTimer* m_cursorWatchdog = nullptr;
	bool m_loadPending = false;
	bool m_seeded = false;

	bool m_loading = false;

	// “加载更多”离屏构建状态
	bool m_prependPending = false;
	QPointer<QWidget> m_pendingAnchor;      // 插入前视口顶部附近的锚点控件
	int m_pendingKeep = 0;                  // 锚点相对视口顶部的偏移
	bool m_pendingAnchorValid = false;

	// 耗时记录：增量构建从 startBuild 到构建完成
	QElapsedTimer m_buildTimer;
	int m_buildEventCount = 0;
};
