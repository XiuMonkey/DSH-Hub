#pragma once

// 首屏预取：进会话时直接用"已经拿到的那一页历史"点亮界面，不等 session/follow 的往返。
// 做法是 session/list 应用后对每个可见会话发一次**一元** session/page，结果经
// historyFetched 交给 DSHHub 入库（CacheManager）并可选地预构建控件树。
// 为什么不用流：session/page 是纯冷读，而 session/follow 会在快照后 **promote** 该会话
// （激活 Agent + 常驻 follower）—— 给 N 个会话开流等于激活 N 个会话。所以永远走一元。
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

	// 该会话是否已有结果或在途（避免重复请求）。
	bool isPending(const QString& sessionId) const;

	// 预取一个会话的最近一页历史。throughSeq 取 session/list 行的 projections.asOfSeq。
	void prefetch(const QString& sessionId, int throughSeq, int maxMessages = 20);

	// 会话已打开/删除时清掉在途标记（迟到的结果仍会送去入库）。
	void forget(const QString& sessionId);

signals:
	// 某个会话的一页历史已到。
	void historyFetched(const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore);

	// 预取失败（仅排查用，不影响主流程）。
	void prefetchFailed(const QString& sessionId, const QString& code, const QString& message);

private:
	DshApiClient* m_api = nullptr;
	QSet<QString> m_inFlight;
};
