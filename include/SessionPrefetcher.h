#pragma once

// ------------------------------------------------------------------
// SessionPrefetcher.h
// ------------------------------------------------------------------
// 首屏预取（0.1.5 版）。
//
// 目的：进入某个会话时不必等 session/follow 的往返 + 服务端读日志，直接用"已经
// 拿到的那一页历史"点亮界面。
//
// 做法：session/list 应用后，对每个可见会话发一次**一元** `session/page`：
//   { request: { address, throughSeq: 该行 projections.asOfSeq, maxMessages: 20 } }
// 全部并发（QNetworkAccessManager 天然多路复用），结果经 historyFetched 交给
// DSHHub 入库（CacheManager）并可选地预构建控件树。
//
// 为什么不用流：`session/page` 是纯冷读（文档原话 "without activating an Agent"），
// 而 `session/follow` 会在快照交付后 **promote** 该会话（激活 Agent + 常驻 follower）
// ——给 N 个会话开流等于激活 N 个会话，代价远大于一次读。所以预取永远走一元。
//
// 与旧版（已删除的 std::async + 自建 QNAM 版）的三点差别：
//   1) 复用 DshApiClient（自带认证 cookie 与 args 信封）——旧版自建 QNAM，
//      在 0.1.5 的认证栅栏下会直接 401；
//   2) 不再"每会话一个线程 + QEventLoop 阻塞等待"，request→解析→回调都在既有
//      链路里异步完成；
//   3) 端点从 session.history 换成 session/page，游标取 session/list 的 asOfSeq。
// ------------------------------------------------------------------

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

	/** 绑定 API 客户端（发出请求前设置即可）。 */
	void setApi(DshApiClient* api);

	/** 该会话是否已有结果或在途（避免重复请求）。 */
	bool isPending(const QString& sessionId) const;

	/**
	 * 预取一个会话的最近一页历史。
	 * @param sessionId   会话 id
	 * @param throughSeq  session/list 行的 projections.asOfSeq（该会话的日志切割点）
	 * @param maxMessages 页大小（数的是消息，不是事件）
	 */
	void prefetch(const QString& sessionId, int throughSeq, int maxMessages = 20);

	/** 会话已打开/删除时清掉在途标记（迟到的结果仍会送去入库）。 */
	void forget(const QString& sessionId);

signals:
	/** 某个会话的一页历史已到。 */
	void historyFetched(const QString& sessionId, const QJsonArray& events, int throughSeq, bool hasMore);

	/** 预取失败（仅排查用，不影响主流程）。 */
	void prefetchFailed(const QString& sessionId, const QString& code, const QString& message);

private:
	DshApiClient* m_api = nullptr;
	QSet<QString> m_inFlight;
};
