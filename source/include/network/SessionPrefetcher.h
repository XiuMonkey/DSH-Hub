#pragma once

// 首屏预取：进会话时直接用“已经拿到的那一页历史”点亮界面，不等 session/follow 的往返。
// 陷阱：必须走一元 session/page 而不是流 —— session/follow 会在快照后 promote 该会话
// （激活 Agent + 常驻 follower），给 N 个会话开流等于激活 N 个会话。
// 请求复用 DshApiClient（自带认证 cookie 与 args 信封），避免自建 QNAM 在认证栅栏下 401。

#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QString>

class DshApiClient;

class SessionPrefetcher : public QObject
{
	Q_OBJECT

public:
	explicit SessionPrefetcher(QObject* parent = nullptr);

	// 绑定 API 客户端（发出请求前设置即可）。
	void setApi(DshApiClient* api);

	// 预取一个会话的最近一页历史；throughSeq 取 session/list 行的 projections.asOfSeq。
	void prefetch(const QString& sessionId, int throughSeq, int maxMessages = 20);

signals:
	// 某个会话的一页历史已到；prefetchFailed 仅用于排查，不影响主流程
	void historyFetched(const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore);
	void prefetchFailed(const QString& sessionId, const QString& code, const QString& message);

private:
	DshApiClient* m_api = nullptr;
	QSet<QString> m_inFlight;
};
