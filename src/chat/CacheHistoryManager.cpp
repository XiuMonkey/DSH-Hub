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

	if (m_messageCache.contains(sessionId))
		delete m_messageCache.take(sessionId);

	m_messageCache.insert(sessionId, messages);
	qInfo().noquote() << "[CacheManager] cached messages sessionId=" << sessionId
		<< " count=" << messages->messages.size();
}

MessageQuery* CacheManager::takeCachedMessages(const QString& sessionId)
{
	m_partialCacheSessions.remove(sessionId);
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

void CacheManager::markPartialCache(const QString& sessionId)
{
	if (!sessionId.isEmpty()) {
		m_partialCacheSessions.insert(sessionId);
		qInfo().noquote() << "[CacheManager] mark partial cache sessionId=" << sessionId;
	}
}

bool CacheManager::isPartialCache(const QString& sessionId) const
{
	return m_partialCacheSessions.contains(sessionId);
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
	m_partialCacheSessions.clear();
}