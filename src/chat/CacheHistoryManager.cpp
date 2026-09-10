#include "CacheHistoryManager.h"

#include "MessageQuery.h"

#include <QDebug>
#include <QVBoxLayout>
#include <QtAlgorithms>

CacheManager::~CacheManager()
{
	clearAll();
}

void CacheManager::cacheSessionMessages(const QString& sessionId, MessageQuery* messages)
{
	if (sessionId.isEmpty() || !messages)
		return;

	// 缓存被新内容整体替换：旧标记/快照不再适用
	m_dirtyCacheSessions.remove(sessionId);
	m_cacheMeta.remove(sessionId);

	if (m_messageCache.contains(sessionId))
		delete m_messageCache.take(sessionId);

	m_messageCache.insert(sessionId, messages);
	qInfo().noquote() << "[CacheManager] cached messages sessionId=" << sessionId
		<< " count=" << messages->messages.size();
}

MessageQuery* CacheManager::takeCachedMessages(const QString& sessionId)
{
	m_dirtyCacheSessions.remove(sessionId);
	m_cacheMeta.remove(sessionId);
	MessageQuery* messages = m_messageCache.take(sessionId);
	if (messages) {
		qInfo().noquote() << "[CacheManager] take cached messages sessionId=" << sessionId
			<< " count=" << messages->messages.size();
	}
	else {
		qInfo().noquote() << "[CacheManager] cache miss sessionId=" << sessionId;
	}
	return messages;
}

void CacheManager::markDirtyCache(const QString& sessionId)
{
	// 后台会话每个事件都会进这里：只在该会话首次变脏时插集合+记日志，
	// 避免每个事件刷一条日志。
	if (!sessionId.isEmpty() && !m_dirtyCacheSessions.contains(sessionId)) {
		m_dirtyCacheSessions.insert(sessionId);
		qInfo().noquote() << "[CacheManager] mark dirty cache sessionId=" << sessionId;
	}
}

bool CacheManager::isDirtyCache(const QString& sessionId) const
{
	return m_dirtyCacheSessions.contains(sessionId);
}

void CacheManager::storeCacheMeta(const QString& sessionId, int rawEventCount, bool hasMore)
{
	if (sessionId.isEmpty())
		return;
	m_cacheMeta.insert(sessionId, CacheMeta{ rawEventCount, hasMore });
}

bool CacheManager::takeCacheMeta(const QString& sessionId, int* rawEventCount, bool* hasMore)
{
	const auto it = m_cacheMeta.constFind(sessionId);
	if (it == m_cacheMeta.constEnd())
		return false;
	if (rawEventCount)
		*rawEventCount = it->rawEventCount;
	if (hasMore)
		*hasMore = it->hasMore;
	m_cacheMeta.erase(it);
	return true;
}

void CacheManager::cacheOrDiscardCurrentSession(const QString& sessionId,
	MessageQuery* messages,
	QVBoxLayout* layout)
{
	if (!messages)
		return;

	if (sessionId.isEmpty()) {
		qInfo().noquote() << "[CacheManager] discard current session (no sessionId)";
		messages->clear();
		delete messages;
		return;
	}

	if (layout)
		messages->detachFromLayout(layout);
	cacheSessionMessages(sessionId, messages);
}

MessageQuery* CacheManager::restoreCachedSession(const QString& sessionId, QVBoxLayout* layout)
{
	MessageQuery* messages = takeCachedMessages(sessionId);
	if (!messages)
		return nullptr;

	qInfo().noquote() << "[CacheManager] restore cached session sessionId=" << sessionId;
	if (layout)
		messages->attachToLayout(layout);
	return messages;
}

bool CacheManager::hasCachedMessages(const QString& sessionId) const
{
	return m_messageCache.contains(sessionId);
}

void CacheManager::storePrefetchedHistory(const QString& sessionId, const QJsonArray& events)
{
	if (sessionId.isEmpty())
		return;

	m_prefetchedHistory.insert(sessionId, events);
	qInfo().noquote() << "[CacheManager] store prefetched history sessionId=" << sessionId
		<< " events=" << events.size();
}

QJsonArray CacheManager::takePrefetchedHistory(const QString& sessionId)
{
	const QJsonArray events = m_prefetchedHistory.take(sessionId);
	qInfo().noquote() << "[CacheManager] take prefetched history sessionId=" << sessionId
		<< " events=" << events.size();
	return events;
}

void CacheManager::clearAll()
{
	qInfo().noquote() << "[CacheManager] clear all caches";
	qDeleteAll(m_messageCache);
	m_messageCache.clear();
	m_prefetchedHistory.clear();
	m_dirtyCacheSessions.clear();
	m_cacheMeta.clear();
}