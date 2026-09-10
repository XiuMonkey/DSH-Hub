#pragma once

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
	void setUsingPrefetched(bool usingPrefetched);
	void setMessages(MessageQuery* messages);
	void cancelBuild();

	// 是否正在拉取历史或在增量构建中（用于避免预取结果重复塞入）
	bool isLoading() const { return m_loading || m_builder.isActive(); }

	// 只把 loader 绑定到某会话（恢复"无需重拉"的预构建预览时使用）：
	// 不发起请求、不渲染；之后点击"加载更多"即可直接以该会话分页。
	void adoptSession(const QString& sessionId)
	{
		if (m_sessionId != sessionId) {
			m_builder.cancel();
			m_sessionId = sessionId;
			m_loading = false;
			m_usingPrefetched = false;
			m_reachedEnd = false;
		}
	}

signals:
	void loadingChanged(bool loading);
	void loadMoreButtonVisibleChanged(bool visible);
	void noMoreHistory();
	void historyError(const QString& code, const QString& message);
	void incrementalBuildReady(MessageQuery* query);
	// 一次全新加载（页面为空）的历史已从服务端返回，即将开始构建。
	// DSHHub 借此在首会话内容还在分批渲染时提前结束初始化遮罩。
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
	bool m_usingPrefetched = false;

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
