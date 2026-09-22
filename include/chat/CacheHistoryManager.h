#pragma once

// ------------------------------------------------------------------
// CacheHistoryManager.h
// ------------------------------------------------------------------
// 缓存与历史状态：
//   - CacheManager：会话控件缓存（整棵已渲染的消息控件树）+ 它的分页/新鲜度快照
//   - HistoryManager：当前会话的历史加载状态（页大小 / 是否还有更早内容 / 计数）
//
// 注意：预取历史数据缓存（m_prefetchedHistory 及 store/takePrefetchedHistory）
// 已随"首屏改用 session/follow 快照"一起删除。
// ------------------------------------------------------------------

#include <QHash>
#include <QJsonArray>
#include <QString>

class MessageQuery;
class QVBoxLayout;

class CacheManager
{
public:
	CacheManager() = default;
	~CacheManager();

	void cacheSessionMessages(const QString& sessionId, MessageQuery* messages);
	MessageQuery* takeCachedMessages(const QString& sessionId);

	// 缓存建立时记录分页/新鲜度快照。
	//
	// 0.1.5 起首屏来自 session/follow 快照（不再是"重拉同一个 maxMessages 尾窗口"），
	// 所以判"缓存还能不能用"要靠游标，而不是靠内容条数是否相等：
	//   throughSeq 缓存建立时的 follow 游标（session/page 的 throughSeq）
	//   oldestSeq  缓存内容里最早一条事件的 seq（"加载更多"的 beforeSeq）
	//   lastSeq    缓存内容里最新一条事件的 seq（对比新快照的 cursor 判断是否过期）
	// 三者齐全时，恢复缓存可以完全跳过重拉与二次渲染（秒开），
	// 而"加载更多"依旧可用。
	void storeCacheMeta(const QString& sessionId, int rawEventCount, bool hasMore,
		int throughSeq, int oldestSeq, int lastSeq);
	bool takeCacheMeta(const QString& sessionId, int* rawEventCount, bool* hasMore,
		int* throughSeq, int* oldestSeq, int* lastSeq);

	// 缓存当前会话：有 sessionId 则缓存，否则销毁空消息容器
	void cacheOrDiscardCurrentSession(const QString& sessionId,
		MessageQuery* messages,
		QVBoxLayout* layout);
	// 取出缓存会话并重新挂到布局
	MessageQuery* restoreCachedSession(const QString& sessionId, QVBoxLayout* layout);

	// 该会话是否已有缓存控件树（决定预取回来还要不要预构建）
	bool hasCachedMessages(const QString& sessionId) const;

	// ---- 预取的历史事件（0.1.5 版）----
	//
	// 启动/会话集合变化后，对每个可见会话发一次一元 session/page，把"最近一页"
	// 存这里；进入会话而控件缓存未命中时用它**立即点亮**（不必等 follow 往返），
	// 随后 follow 快照会因"不比已有内容新"而不重建。
	// 与控件树的区别：这里只存事件（JSON），很轻；控件树才是重的东西。
	struct PrefetchedHistory
	{
		QJsonArray events;      // 裸事件对象（已从 records 里取出内层 event）
		int throughSeq = 0;     // 取这页用的游标（来自 session/list 的 projections.asOfSeq）
		bool hasMore = false;   // 服务端是否还有更早内容
	};

	void storePrefetchedHistory(const QString& sessionId, const PrefetchedHistory& history);
	bool takePrefetchedHistory(const QString& sessionId, PrefetchedHistory* history);

	void clearAll();

private:
	// 缓存建立时的分页快照：rawEventCount=来源原始事件数，hasMore=是否还有更早内容，
	// throughSeq/oldestSeq/lastSeq 见 storeCacheMeta 的说明。
	struct CacheMeta
	{
		int rawEventCount = 0;
		bool hasMore = false;
		int throughSeq = 0;
		int oldestSeq = 0;
		int lastSeq = 0;
	};

	QHash<QString, MessageQuery*> m_messageCache;
	QHash<QString, CacheMeta> m_cacheMeta;
	QHash<QString, PrefetchedHistory> m_prefetchedHistory;
};

class HistoryManager
{
public:
	int limit() const { return m_historyLimit; }
	void setLimit(int limit) { m_historyLimit = limit; }

	bool hasMore() const { return m_historyHasMore; }
	void setHasMore(bool hasMore) { m_historyHasMore = hasMore; }

	int eventCount() const { return m_historyEventCount; }
	void setEventCount(int count) { m_historyEventCount = count; }

	bool loadMoreRequested() const { return m_historyLoadMoreRequested; }
	void setLoadMoreRequested(bool requested) { m_historyLoadMoreRequested = requested; }

	void reset()
	{
		m_historyLimit = 20;
		m_historyHasMore = false;
		m_historyEventCount = 0;
		m_historyLoadMoreRequested = false;
	}

private:
	int m_historyLimit = 20;
	bool m_historyHasMore = false;
	int m_historyEventCount = 0;
	bool m_historyLoadMoreRequested = false;
};
