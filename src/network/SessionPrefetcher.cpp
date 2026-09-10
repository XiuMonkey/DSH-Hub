#include "SessionPrefetcher.h"

#include <QDebug>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>

#include <chrono>

namespace
{
	QJsonArray fetchSessionHistory(const QUrl& baseUrl, const QString& sessionId, int maxMessages)
	{
		QNetworkAccessManager nam;

		QUrl url = baseUrl;
		url.setPath(QStringLiteral("/api/session.history"));

		QNetworkRequest request(url);
		request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

		QJsonObject payload;
		payload.insert(QStringLiteral("sessionId"), sessionId);
		payload.insert(QStringLiteral("maxMessages"), maxMessages);

		QJsonObject body;
		body.insert(QStringLiteral("type"), QStringLiteral("client-request"));
		body.insert(QStringLiteral("rpcId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
		body.insert(QStringLiteral("method"), QStringLiteral("session.history"));
		body.insert(QStringLiteral("payload"), payload);

		QNetworkReply* reply = nam.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

		QEventLoop loop;
		QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();

		if (reply->error() != QNetworkReply::NoError) {
			qWarning().noquote() << "[SessionPrefetcher] history request failed sessionId=" << sessionId << " error=" << reply->errorString();
			delete reply;
			return {};
		}

		const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
		delete reply;

		const QJsonObject result = root.value(QStringLiteral("result")).toObject();
		if (!result.value(QStringLiteral("ok")).toBool()) {
			qWarning().noquote() << "[SessionPrefetcher] history result not ok sessionId=" << sessionId;
			return {};
		}

		return result.value(QStringLiteral("value")).toObject()
			.value(QStringLiteral("events")).toArray();
	}
}

SessionPrefetcher::SessionPrefetcher(QObject* parent)
	: QObject(parent)
{
	m_pollTimer = new QTimer(this);
	m_pollTimer->setInterval(50);
	connect(m_pollTimer, &QTimer::timeout, this, &SessionPrefetcher::pollFutures);
}

void SessionPrefetcher::prefetchHistory(const QUrl& baseUrl, const QString& sessionId, int maxMessages)
{
	// 同一会话已有预取在途时直接忽略：重复 insert 会覆盖旧 future，
	// 其析构会阻塞主线程直到该次 HTTP 结束（std::async 的 future 析构会等待）。
	if (m_futures.contains(sessionId))
		return;

	qInfo().noquote() << "[SessionPrefetcher] prefetch started sessionId=" << sessionId << " maxMessages=" << maxMessages;

	auto future = std::make_shared<std::future<QJsonArray>>(
		std::async(std::launch::async, fetchSessionHistory, baseUrl, sessionId, maxMessages));

	m_futures.insert(sessionId, future);
	QElapsedTimer started;
	started.start();
	m_started.insert(sessionId, started);
	if (!m_pollTimer->isActive())
		m_pollTimer->start();
}

void SessionPrefetcher::pollFutures()
{
	for (auto it = m_futures.begin(); it != m_futures.end();) {
		if (it.value()->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			const QJsonArray events = it.value()->get();
			const qint64 elapsedMs = m_started.contains(it.key())
				? m_started.value(it.key()).elapsed()
				: -1;
			m_started.remove(it.key());
			qInfo().noquote() << "[SessionPrefetcher] history fetched sessionId=" << it.key()
				<< " events=" << events.size()
				<< " prefetchMs=" << elapsedMs;
			emit historyFetched(it.key(), events);
			it = m_futures.erase(it);
		}
		else {
			++it;
		}
	}

	if (m_futures.isEmpty())
		m_pollTimer->stop();
}