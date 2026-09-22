#pragma once

// 会话缓存与历史状态：CacheManager 缓存整棵已渲染的消息控件树及其分页/新鲜度快照，
// HistoryManager 只记当前会话的历史加载状态（页大小 / 是否还有更早内容 / 计数）。

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

	// 记录缓存建立时的分页/新鲜度快照；判“缓存还能不能用”必须靠游标而非内容条数（首屏已改为 session/follow 快照）
	void storeCacheMeta(const QString& sessionId, int rawEventCount, bool hasMore,
		int throughSeq, int oldestSeq, int lastSeq);
	bool takeCacheMeta(const QString& sessionId, int* rawEventCount, bool* hasMore,
		int* throughSeq, int* oldestSeq, int* lastSeq);

	// 有 sessionId 则缓存当前会话，否则销毁空消息容器
	void cacheOrDiscardCurrentSession(const QString& sessionId,
		MessageQuery* messages,
		QVBoxLayout* layout);
	MessageQuery* restoreCachedSession(const QString& sessionId, QVBoxLayout* layout);

	// 该会话是否已有缓存控件树；决定预取回来的事件还要不要预构建
	bool hasCachedMessages(const QString& sessionId) const;

	// 预取的一页事件（只存 JSON，很轻）：控件缓存未命中时用它立即点亮，随后 follow 快照不比它新就不重建
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
	struct CacheMeta
	{
		int rawEventCount = 0; // 来源原始事件数
		bool hasMore = false;  // 服务端是否还有更早内容
		int throughSeq = 0;    // 缓存建立时的 follow 游标（session/page 的 throughSeq）
		int oldestSeq = 0;     // 缓存内容里最早一条事件的 seq（“加载更多”的 beforeSeq）
		int lastSeq = 0;       // 缓存内容里最新一条事件的 seq（对比新快照的 cursor 判断是否过期）
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
