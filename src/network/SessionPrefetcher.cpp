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
	qInfo().noquote() << "[SessionPrefetcher] prefetch started sessionId=" << sessionId << " maxMessages=" << maxMessages;

	auto future = std::make_shared<std::future<QJsonArray>>(
		std::async(std::launch::async, fetchSessionHistory, baseUrl, sessionId, maxMessages));

	m_futures.insert(sessionId, future);
	if (!m_pollTimer->isActive())
		m_pollTimer->start();
}

void SessionPrefetcher::pollFutures()
{
	for (auto it = m_futures.begin(); it != m_futures.end();) {
		if (it.value()->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			const QJsonArray events = it.value()->get();
			qInfo().noquote() << "[SessionPrefetcher] history fetched sessionId=" << it.key() << " events=" << events.size();
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