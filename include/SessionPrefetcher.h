#pragma once

// ------------------------------------------------------------------
// SessionPrefetcher.h
// ------------------------------------------------------------------
// 在后台线程预取会话历史，避免打开会话时等待网络请求。
// 使用 std::async + QNetworkAccessManager 在 worker 线程中阻塞等待响应，
// 完成后通过信号把结果交回主线程。
// ------------------------------------------------------------------

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <future>
#include <memory>

class QTimer;

class SessionPrefetcher : public QObject
{
	Q_OBJECT

public:
	explicit SessionPrefetcher(QObject* parent = nullptr);

	// 后台预取某个会话的最后 maxMessages 条历史
	void prefetchHistory(const QUrl& baseUrl, const QString& sessionId, int maxMessages);

signals:
	void historyFetched(const QString& sessionId, const QJsonArray& events);

private:
	void pollFutures();

	QTimer* m_pollTimer = nullptr;
	QHash<QString, std::shared_ptr<std::future<QJsonArray>>> m_futures;
};
