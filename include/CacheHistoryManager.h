#pragma once

// ------------------------------------------------------------------
// CacheHistoryManager.h
// ------------------------------------------------------------------
// 缓存与历史管理器：
//   - CacheManager：管理会话控件缓存和预取历史数据缓存
//   - HistoryManager：管理历史加载状态
// ------------------------------------------------------------------

#include <QHash>
#include <QJsonArray>
#include <QSet>
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
	bool hasCachedMessages(const QString& sessionId) const;

	// 标记该会话的缓存可能已过期：缓存建立之后又有后台事件流入
	// （会话在别处继续运行）。恢复这类缓存时必须重新拉取历史。
	void markDirtyCache(const QString& sessionId);
	bool isDirtyCache(const QString& sessionId) const;

	// 缓存建立时记录分页快照（来源原始事件数 + 是否还有更早内容）。
	// 恢复缓存（且未 dirty）时用它播种分页状态即可跳过重拉与二次渲染——
	// 缓存内容与重拉请求是同一个 maxMessages 尾窗口，内容一致时重拉纯属浪费。
	void storeCacheMeta(const QString& sessionId, int rawEventCount, bool hasMore);
	bool takeCacheMeta(const QString& sessionId, int* rawEventCount, bool* hasMore);

	// 缓存当前会话：有 sessionId 则缓存，否则销毁空消息容器
	void cacheOrDiscardCurrentSession(const QString& sessionId,
		MessageQuery* messages,
		QVBoxLayout* layout);
	// 取出缓存会话并重新挂到布局
	MessageQuery* restoreCachedSession(const QString& sessionId, QVBoxLayout* layout);

	void storePrefetchedHistory(const QString& sessionId, const QJsonArray& events);
	QJsonArray takePrefetchedHistory(const QString& sessionId);

	void clearAll();

private:
	// 缓存建立时的分页快照：rawEventCount=来源原始事件数，hasMore=是否还有更早内容
	struct CacheMeta
	{
		int rawEventCount = 0;
		bool hasMore = false;
	};

	QHash<QString, MessageQuery*> m_messageCache;
	QHash<QString, QJsonArray> m_prefetchedHistory;
	QSet<QString> m_dirtyCacheSessions;  // 缓存建立后又有后台事件流入
	QHash<QString, CacheMeta> m_cacheMeta;
};

class HistoryManager
{
public:
	int limit() const { return m_historyLimit; }
	void setLimit(int limit) { m_historyLimit = limit; }
	void increaseLimit(int delta) { m_historyLimit += delta; }

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
