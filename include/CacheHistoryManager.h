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

	// 标记该缓存只是预取的部分消息，打开后仍需要继续加载完整历史
	void markPartialCache(const QString& sessionId);
	bool isPartialCache(const QString& sessionId) const;

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
	QHash<QString, MessageQuery*> m_messageCache;
	QHash<QString, QJsonArray> m_prefetchedHistory;
	QSet<QString> m_partialCacheSessions;
};

class HistoryManager
{
public:
	bool isLoading() const { return m_historyLoading; }
	void setLoading(bool loading) { m_historyLoading = loading; }

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
	bool m_historyLoading = false;
	int m_historyLimit = 20;
	bool m_historyHasMore = false;
	int m_historyEventCount = 0;
	bool m_historyLoadMoreRequested = false;
};
