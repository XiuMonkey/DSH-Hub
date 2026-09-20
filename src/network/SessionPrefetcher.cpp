#include "network/SessionPrefetcher.h"

#include "network/DshApiClient.h"
#include "common/session/SessionCommands.h"

#include <QDebug>

SessionPrefetcher::SessionPrefetcher(QObject* parent)
	: QObject(parent)
{
}

void SessionPrefetcher::setApi(DshApiClient* api)
{
	m_api = api;
}

bool SessionPrefetcher::isPending(const QString& sessionId) const
{
	return m_inFlight.contains(sessionId);
}

/**
 * 预取一个会话的最近一页历史。
 *
 * 走 DshApiClient::callMethod（异步）：请求排队 → 响应到达 → 线程池解析 JSON →
 * 主线程回调。所以这里只需要发出去，不需要线程也不需要阻塞等待。
 */
void SessionPrefetcher::prefetch(const QString& sessionId, int throughSeq, int maxMessages)
{
	if (!m_api || sessionId.isEmpty())
		return;

	// throughSeq 是服务端必填项；没有它（例如刚创建、还没产生投影的会话）就不预取
	if (throughSeq <= 0)
		return;

	if (m_inFlight.contains(sessionId))
		return;

	m_inFlight.insert(sessionId);

	m_api->callMethod(
		QStringLiteral("session/page"),
		SessionCommands::sessionPage(sessionId, throughSeq, maxMessages),
		[this, sessionId, throughSeq](const QJsonObject& value) {
			m_inFlight.remove(sessionId);

			const QJsonArray events = SessionCommands::eventsFromRecords(
				value.value(QStringLiteral("records")).toArray());
			const bool hasMore = value.value(QStringLiteral("hasMore")).toBool();

			qInfo().noquote() << "[SessionPrefetcher] prefetched sessionId=" << sessionId
				<< "events=" << events.size() << "throughSeq=" << throughSeq
				<< "hasMore=" << hasMore;

			emit historyFetched(sessionId, events, throughSeq, hasMore);
		},
		[this, sessionId](const DshApiClient::RpcError& error) {
			m_inFlight.remove(sessionId);
			qWarning().noquote() << "[SessionPrefetcher] prefetch failed sessionId=" << sessionId
				<< "code=" << error.code << "message=" << error.message;
			emit prefetchFailed(sessionId, error.code, error.message);
		});
}

void SessionPrefetcher::forget(const QString& sessionId)
{
	m_inFlight.remove(sessionId);
}