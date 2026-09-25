#pragma once

// 会话消息列表（MessageQuery / MessageQueryBuilder）与它的历史加载器 HistoryLoader。
// 0.1.5 历史模型：首屏优先取 mux 上 session/follow 的首帧快照（cursor/records/hasMore，只播种最近
// kSeedEventCap 条）；上翻走一元 RPC session/page（必带 throughSeq；beforeSeq=当前最早 seq 且为排他上界；
// maxMessages 数的是消息不是事件）。

#include "chat/AgentMessageUnit.h"
#include "chat/SystemMessageUnit.h"
#include "chat/UserMessageUnit.h"

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
	void appendEvents(QVBoxLayout* layout, const QJsonArray& events);

	// 实况流式/事件 → 气泡更新的统一入口：与历史回放共用"事件→内容"心智，DSHHub 只管路由与 streaming/timer 策略
	struct StreamFrameResult
	{
		enum Kind
		{
			Ignored,        // 事件不需要更新气泡（user/message、未知类型等）
			Streaming,      // 收到流式内容（chunk/tool…），控制器应启动定时器并置 streaming
			FinalMessage    // assistant/message 收尾：内容已合并/封存
		};
		Kind kind = Ignored;
	};

	// event：已解包的事件对象；layout：新建气泡的宿主布局；wasStreaming：调用前控制器是否流式态（收尾去重）
	StreamFrameResult applyStreamEvent(const QJsonObject& event, QVBoxLayout* layout, bool wasStreaming);
	// 从历史事件离线构建一个完整 MessageQuery；调用方负责后续 attach/释放
	static MessageQuery* fromEvents(const QJsonArray& events);

	// 控件缓存支持：从布局中摘下但不销毁，之后可以重新 attach 回来
	void detachFromLayout(QVBoxLayout* layout);
	void attachToLayout(QVBoxLayout* layout);
	// 把离屏构建好的"更早一页"插到本列表/布局顶部（layoutIndex 起）并接管 older 的所有权
	// （调用后 older 不可再用）
	void prependQuery(QVBoxLayout* layout, MessageQuery* older, int layoutIndex);

	// 批量构建模式：开启期间新建的 Agent 气泡不逐次做高度拟合/排队 refit，整页构建完成后统一关闭并拟合一次
	void setBulkFitting(bool bulk) { m_bulkFitting = bulk; }
	// 把内部控件从当前父对象上解除，便于从临时离屏容器安全迁移
	void releaseWidgets();
};

// 离屏增量构建消息列表，避免一次性渲染大量历史控件卡顿
class MessageQueryBuilder
{
public:
	MessageQueryBuilder();
	~MessageQueryBuilder();

	void start(const QJsonArray& events);
	// 处理下一批；true=仍在构建，false=已完成
	bool step(int batchSize = 5);
	// 取消并释放临时控件
	void cancel();
	bool isActive() const;
	// 取出构建完成的 MessageQuery；调用方负责后续 attach/释放
	MessageQuery* takeResult();

private:
	QWidget* m_holder = nullptr;
	QVBoxLayout* m_layout = nullptr;
	MessageQuery* m_query = nullptr;
	QJsonArray m_events;
	int m_index = 0;
	bool m_active = false;
};

// 历史消息加载器：session/page 拉取、增量构建、加载更多与分页游标管理
class HistoryLoader : public QObject
{
	Q_OBJECT

public:
	HistoryLoader(DshApiClient* api, MessageQuery* messages, QVBoxLayout* layout,
		HistoryManager* history, QScrollArea* scrollArea, QObject* parent = nullptr);

	void load(const QString& sessionId);
	void loadMore();
	void setMessages(MessageQuery* messages);
	void cancelBuild();

	// follow 快照给的日志游标（= session/page 需要的 throughSeq）；拿到前首屏请求挂起（m_loadPending），由本函数唤醒
	void setStreamCursor(int cursor);
	// 用 follow 快照的 records 直接播种首屏（省掉一次 session/page 往返）；sessionId 不是当前会话时忽略（迟到的帧）
	void seedFromSnapshot(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore);
	// 当前分页游标（follow 快照给的 throughSeq）；缓存快照要用
	int streamCursor() const { return m_throughSeq; }
	// 当前内容里最早一条事件的 seq（"加载更多"的 beforeSeq）；缓存快照要用
	int oldestSeq() const { return m_oldestSeq; }
	// follow 快照迟迟不来时当 throughSeq 用的回落值（session/list 的 projections.asOfSeq）；0=没有，只能报错
	void setFallbackCursor(int cursor);
	// 恢复缓存时接管分页状态（游标/最早 seq/内容最新 seq + m_seeded）：快照不比缓存内容新就不重建，
	// 保住"缓存命中即秒开"
	void adoptCachedState(int throughSeq, int oldestSeq, int contentLastSeq, int eventCount, bool hasMore);
	// 用"预取的一页事件"（本地数据）播种首屏：不做新鲜度判定，且走分批构建而非一次性贴控件树（避免单帧阻塞）
	void seedFromPrefetched(const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore);
	// 只把 loader 绑定到某会话（恢复缓存、不重新拉取时使用）：不发起请求、不渲染；之后点"加载更多"即可分页
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
	void loadMoreButtonVisibleChanged(bool visible);
	void noMoreHistory();
	void historyError(const QString& code, const QString& message);
	void incrementalBuildReady(MessageQuery* query);
	// 首屏内容真正上屏（分批构建完成、控件已挂进实时布局）；DSHHub 借此收掉启动遮罩，不能在"开始构建"时就收
	void firstHistoryArrived();

private:
	void continueBuild();
	void startBuild(const QJsonArray& events);
	// "加载更多"的离屏分批构建：更早的一页先在离屏 builder 里分片渲染，完成后一次性插到活布局顶部
	void startPrepend(const QJsonArray& olderEvents);
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
	// "已到历史顶部"：只由服务端回包驱动；勿用 m_history->hasMore() 门控（该值在部分流程与服务端不符，
	// 会误报"没有更多"）
	bool m_reachedEnd = false;

	// 0.1.5 分页游标（来自 follow 快照）：快照没到就挂起首屏请求（m_loadPending），到了直接播种（m_seeded）
	int m_throughSeq = 0;
	// 当前已加载内容里最早一条事件的 seq：上翻一页时作为 session/page 的 beforeSeq
	int m_oldestSeq = 0;
	// follow 快照没到时的 throughSeq 回落值（session/list 的 projections.asOfSeq）
	int m_fallbackCursor = 0;
	// 当前内容的"最新位置"：缓存恢复时用来判断缓存是否已被后来事件超越（缓存里可能有游标之后的实时事件）
	int m_contentLastSeq = 0;
	// 等游标的看门狗：超时后用回落值发请求，或明确报错而不是一直转圈
	QTimer* m_cursorWatchdog = nullptr;
	bool m_loadPending = false;
	bool m_seeded = false;
	bool m_loading = false;

	// "加载更多"离屏构建状态
	bool m_prependPending = false;
	QPointer<QWidget> m_pendingAnchor;      // 插入前视口顶部附近的锚点控件
	int m_pendingKeep = 0;                  // 锚点相对视口顶部的偏移
	bool m_pendingAnchorValid = false;

	// 耗时记录：增量构建从 startBuild 到构建完成
	QElapsedTimer m_buildTimer;
	int m_buildEventCount = 0;
};
