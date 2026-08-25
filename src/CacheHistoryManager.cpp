#include "CacheHistoryManager.h"

#include "MessageQuery.h"

#include <QVBoxLayout>
#include <QtAlgorithms>

CacheManager::~CacheManager()
{
    clearAll();
}

void CacheManager::cacheSessionMessages(const QString &sessionId, MessageQuery *messages)
{
    if (sessionId.isEmpty() || !messages)
        return;

    if (m_messageCache.contains(sessionId))
        delete m_messageCache.take(sessionId);

    m_messageCache.insert(sessionId, messages);
}

MessageQuery *CacheManager::takeCachedMessages(const QString &sessionId)
{
    m_partialCacheSessions.remove(sessionId);
    return m_messageCache.take(sessionId);
}

void CacheManager::markPartialCache(const QString &sessionId)
{
    if (!sessionId.isEmpty())
        m_partialCacheSessions.insert(sessionId);
}

bool CacheManager::isPartialCache(const QString &sessionId) const
{
    return m_partialCacheSessions.contains(sessionId);
}

void CacheManager::cacheOrDiscardCurrentSession(const QString &sessionId,
                                                MessageQuery *messages,
                                                QVBoxLayout *layout)
{
    if (!messages)
        return;

    if (sessionId.isEmpty()) {
        messages->clear();
        delete messages;
        return;
    }

    if (layout)
        messages->detachFromLayout(layout);
    cacheSessionMessages(sessionId, messages);
}

MessageQuery *CacheManager::restoreCachedSession(const QString &sessionId, QVBoxLayout *layout)
{
    MessageQuery *messages = takeCachedMessages(sessionId);
    if (!messages)
        return nullptr;

    if (layout)
        messages->attachToLayout(layout);
    return messages;
}

bool CacheManager::hasCachedMessages(const QString &sessionId) const
{
    return m_messageCache.contains(sessionId);
}

void CacheManager::storePrefetchedHistory(const QString &sessionId, const QJsonArray &events)
{
    if (!sessionId.isEmpty())
        m_prefetchedHistory.insert(sessionId, events);
}

QJsonArray CacheManager::takePrefetchedHistory(const QString &sessionId)
{
    return m_prefetchedHistory.take(sessionId);
}

void CacheManager::clearAll()
{
    qDeleteAll(m_messageCache);
    m_messageCache.clear();
    m_prefetchedHistory.clear();
    m_partialCacheSessions.clear();
}
