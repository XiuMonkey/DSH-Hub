#include "chat/CacheHistoryManager.h"

#include "chat/MessageQuery.h"

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

	// 缓存被新内容整体替换：旧的分页快照不再适用
	m_cacheMeta.remove(sessionId);
	if (m_messageCache.contains(sessionId))
		delete m_messageCache.take(sessionId);
	m_messageCache.insert(sessionId, messages);

	qInfo().noquote() << "[CacheManager] cached messages sessionId=" << sessionId
		<< " count=" << messages->messages.size();
}

MessageQuery* CacheManager::takeCachedMessages(const QString& sessionId)
{
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

void CacheManager::storeCacheMeta(const QString& sessionId, int rawEventCount, bool hasMore,
	int throughSeq, int oldestSeq, int lastSeq)
{
	if (sessionId.isEmpty())
		return;
	m_cacheMeta.insert(sessionId, CacheMeta{ rawEventCount, hasMore, throughSeq, oldestSeq, lastSeq });
}

bool CacheManager::takeCacheMeta(const QString& sessionId, int* rawEventCount, bool* hasMore,
	int* throughSeq, int* oldestSeq, int* lastSeq)
{
	const auto it = m_cacheMeta.constFind(sessionId);
	if (it == m_cacheMeta.constEnd())
		return false;
	if (rawEventCount)
		*rawEventCount = it->rawEventCount;
	if (hasMore)
		*hasMore = it->hasMore;
	if (throughSeq)
		*throughSeq = it->throughSeq;
	if (oldestSeq)
		*oldestSeq = it->oldestSeq;
	if (lastSeq)
		*lastSeq = it->lastSeq;
	m_cacheMeta.erase(it);
	return true;
}

void CacheManager::cacheOrDiscardCurrentSession(const QString& sessionId, MessageQuery* messages, QVBoxLayout* layout)
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

void CacheManager::storePrefetchedHistory(const QString& sessionId, const PrefetchedHistory& history)
{
	if (sessionId.isEmpty() || history.events.isEmpty())
		return;

	m_prefetchedHistory.insert(sessionId, history);
	qInfo().noquote() << "[CacheManager] store prefetched history sessionId=" << sessionId
		<< "events=" << history.events.size() << "throughSeq=" << history.throughSeq
		<< "hasMore=" << history.hasMore;
}

bool CacheManager::takePrefetchedHistory(const QString& sessionId, PrefetchedHistory* history)
{
	const auto it = m_prefetchedHistory.find(sessionId);
	if (it == m_prefetchedHistory.end())
		return false;

	if (history)
		*history = it.value();

	qInfo().noquote() << "[CacheManager] take prefetched history sessionId=" << sessionId
		<< "events=" << it.value().events.size();
	m_prefetchedHistory.erase(it);
	return true;
}

void CacheManager::clearAll()
{
	qInfo().noquote() << "[CacheManager] clear all caches";
	qDeleteAll(m_messageCache);
	m_messageCache.clear();
	m_cacheMeta.clear();
	m_prefetchedHistory.clear();
}
