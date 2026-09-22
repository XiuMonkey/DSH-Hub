#pragma once

// DSH（DeepSeek Harness）本地 API 的 Qt 客户端（对齐 dsh 0.1.5）：一元 RPC + 认证围栏 + 单条 mux WebSocket 上的逻辑流。
// 陷阱：/api 在浏览器认证栅栏之后（没有 cookie 一律 401，cookie 只能由 GET /?token=<启动令牌> 换到）；逻辑流 id 必须唯一，重复会让服务端 close(1008) 关掉整条 mux。

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QObject>
#include <QPointer>
#include <QRunnable>
#include <QUrl>
#include <functional>

class QJsonDocument;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QWebSocket;

// DSH API 客户端；典型用法：setBaseUrl()（内部换掉 cookie）→ openStreams() → callMethod() / 监听 muxFrameReceived。
class DshApiClient : public QObject
{
	Q_OBJECT

public:
	// RPC 业务错误：HTTP 成功但 result.ok == false 时使用。
	struct RpcError
	{
		QString code;       // 错误码，例如 session-not-found、MISSING_CREDENTIAL
		QString message;    // 错误描述
	};

	// 创建 QNetworkAccessManager 和 mux WebSocket，并连接内部信号。
	explicit DshApiClient(QObject* parent = nullptr);

	// 关闭 WebSocket 流并释放内部对象。
	~DshApiClient() override;

	// 设置 DSH 服务的基础 URL（例如 http://127.0.0.1:3080）；会取走 ?token= 并启动认证握手。
	void setBaseUrl(const QUrl& url);

	// 当前设置的 DSH 基础 URL。
	QUrl baseUrl() const;

	// 启动令牌（认证 URL 上的 ?token=）：重建主窗口（切主题）时必须一起带过去，否则新窗口换不到 cookie、界面永远停在初始化里。
	QString launchToken() const { return m_launchToken; }

	// 打开 mux 上的逻辑流；$events 收到 ready 帧后发出 connected()。
	void openStreams();

	// 关闭流通道（WebSocket + 它上面的全部逻辑流）。
	void closeStreams();

	// 跟随一个会话（在 mux 上开 session/follow，换会话时自动换流）；快照经 sessionSnapshotReady，之后的实时日志走 muxFrameReceived。
	void followSession(const QString& sessionId);

	// 关闭当前的 session/follow 逻辑流。
	void unfollowSession();

	// 判定标准：mux WebSocket 已打开，且 $events 逻辑流已收到 ready 帧。
	bool isConnected() const;

	// 发一元 RPC：payload 是本端点的 args 内容（这里负责包一层 {"args": …}，键名用描述符里的 wire 名）；endpoint 用斜杠（"session/list"，不是点号），返回裸数组的端点改用 callMethodValue。
	void callMethod(
		const QString& method,
		const QJsonObject& payload = {},
		std::function<void(const QJsonObject& value)> onSuccess = {},
		std::function<void(const RpcError& error)> onError = {});

	// 同 callMethod，但成功回调拿裸 JSON 值（端点返回数组时用它，例如 llm/listConfigurableProviders）。
	void callMethodValue(
		const QString& method,
		const QJsonObject& payload,
		std::function<void(const QJsonValue& value)> onSuccess,
		std::function<void(const RpcError& error)> onError = {});

	// 应答 mux 流收到的审批/提问帧：rpcId 用帧里的 rpcId，value 形如 { "sessionId": …, "approvalId": …, "outcome": "allowed-once" }。
	void respond(
		const QString& rpcId,
		const QJsonObject& value,
		std::function<void(const QJsonObject& receipt)> onSuccess = {},
		std::function<void(const RpcError& error)> onError = {});

signals:
	// mux 上 $events 逻辑流收到 ready 帧（事件源就绪）。
	void connected();

	// 兼容旧消费者：把 0.1.5 的新帧翻译成旧形状再发出（session 事件 / approval-requested / question-requested），rpcId 用 0.1.5 的 eventId。
	void muxFrameReceived(const QJsonObject& frame);

	// workspace/follow 的 baseline 帧：工作区清单 + 完整归档集合。
	void workspaceSnapshotReady(const QJsonArray& items, const QJsonArray& archivedSessionIds);

	// workspace/follow 的四类增量帧：upsert / remove / order / archived（order 与 archived 是整体替换）。
	void workspaceUpserted(const QJsonObject& workspace);
	void workspaceRemoved(const QString& workspaceId);
	void workspaceReordered(const QStringList& workspaceIds);
	void workspaceArchiveChanged(const QJsonArray& archivedSessionIds);

	// session/follow 的 snapshot 帧：一次冷读的初始记录 + 游标（cursor 即 session/page 需要的 throughSeq）。
	void sessionSnapshotReady(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore);

	// 快照帧里带的会话投影：服务端按"全量折叠"直接从日志算出，所以没被 Agent 附着的会话也准（session/list 行给的是投影缓存里的检查点，可能为空或很旧）。
	void sessionProjectionsReady(const QString& sessionId, int asOfSeq, const QJsonObject& values);

	// session/control 流的 baseline：每个活会话一份现算的投影快照，键是 sessionId、值是 {asOfSeq, values}。
	void sessionProjectionsBaselineReady(const QJsonObject& projectionsBySession);

	// session/control 流的实时帧：某个会话的某个投影键变了（现算现推，小灰字靠它实时跳且全程不读缓存）。
	void sessionProjectionChanged(const QString& sessionId, const QString& key,
		const QJsonValue& value, int seq);

	// 传输层错误；context 例如 "stream" 或 HTTP 请求。
	void transportError(const QString& context, const QString& message);

private:
	// 一个尚未收到响应的 HTTP RPC（在响应返回时按 rpcId 匹配回调）。
	struct PendingCall
	{
		QString path;       // 请求的 API 路径，例如 /api/session/list
		std::function<void(const QJsonValue& value)> onSuccess;
		std::function<void(const RpcError& error)> onError;
	};

	// 认证还没就绪时先挂起来的 RPC：服务端刚重启那一两秒里直发必然 401，挂起来等握手完成再按顺序补发。
	struct QueuedCall
	{
		QString path;
		QJsonObject body;
		std::function<void(const QJsonValue&)> onSuccess;
		std::function<void(const RpcError&)> onError;
	};

	// 在线程池里解析 HTTP 响应体（大 JSON 不在主线程 fromJson），解析完成后经 queued invokeMethod 回投主线程的 handleParsedResponse()。
	class JsonParseRunnable : public QRunnable
	{
	public:
		JsonParseRunnable(DshApiClient* client, QString rpcId, QByteArray body);
		void run() override;

	private:
		QPointer<DshApiClient> m_client; // 仅用于把解析结果回投主线程
		QString m_rpcId;
		QByteArray m_body;
	};

	// 主线程处理解析完成后的响应（拆 result 信封并调用成功/失败回调）。
	void handleParsedResponse(const QString& rpcId, const QJsonDocument& doc);

	// 把 HTTP URL 转换成对应的 WebSocket URL（http→ws、https→wss）。
	QUrl makeUrl(const QString& path) const;

	// 发送一个 HTTP POST JSON 请求；内部把 PendingCall 记入 m_pending 等响应。
	void post(
		const QString& path,
		const QJsonObject& body,
		std::function<void(const QJsonValue& value)> onSuccess,
		std::function<void(const RpcError& error)> onError);

	// 用启动令牌换认证 cookie：GET http://127.0.0.1:<port>/?token=<令牌> 返回 303 + Set-Cookie: dsh-auth-<authority 哈希>，之后所有 /api 请求与 WebSocket 握手都必须带上它，否则一律 401；成功后自动 openStreams()。
	void startAuthHandshake();

private slots:
	// QNetworkReply::finished 的统一处理槽。
	void onReplyFinished();

private:
	// mux WebSocket 收到一条文本消息：按 type/streamId 路由。
	void onStreamTextMessage(const QString& message);

	// 在 mux 上打开一条逻辑流（frame: {type:"open",streamId,endpoint,payload:{args}}）。
	void sendStreamOpen(const QString& endpoint, const QString& streamId, const QJsonObject& args = {});

	// 取消一条逻辑流（frame: {type:"cancel",streamId}）。
	void sendStreamCancel(const QString& streamId);

	// 四条逻辑流的 item 分发：$events（ready/emit/waterfall）、workspace/follow、session/follow、session/control。
	void handleEventsItem(const QJsonValue& value);
	void handleWorkspaceItem(const QJsonValue& value);
	void handleSessionItem(const QJsonValue& value);
	void handleControlItem(const QJsonValue& value);

	// 把一条会话日志事件翻成旧的 {payload:{type:"session/event",…}} 帧发出。
	void emitSessionEventFrame(const QString& sessionId, const QJsonObject& event);

	// 断线后安排一次 mux 重连（重连成功会重开各流并重新跟随当前会话）。
	void scheduleReconnect();

	// 生成一条逻辑流 id（每次 open 都必须唯一；服务端遇重复 id 会 close(1008) 关掉整条 mux）。
	QString nextStreamId(const QString& prefix);

	// 认证就绪后把挂起的 RPC 按顺序补发。
	void flushAuthQueue();
	// 认证硬失败时把挂起的 RPC 用错误回掉（不能让调用方永远挂着）。
	void failAuthQueue(const QString& code, const QString& message);

private:
	QNetworkAccessManager* m_nam = nullptr; // 用于发送 HTTP 请求
	QWebSocket* m_stream = nullptr;         // /api/remote.mux（0.1.5 唯一的一条流通道）
	QUrl m_baseUrl;                         // DSH 服务基础地址（已剥离 ?token=）
	QHash<QString, PendingCall> m_pending;  // rpcId -> 待处理请求
	QHash<QString, PendingCall> m_parsing;  // rpcId -> 响应体已在后台解析、待回投的请求
	QString m_launchToken;                  // 启动令牌（认证 URL 上的 ?token=）
	QString m_authCookie;                   // 兑换到的认证 cookie（name=value）
	bool m_authenticated = false;           // 认证 cookie 是否已就绪
	bool m_authInFlight = false;            // 认证请求是否在途
	// 认证"代次"：服务端重启后旧地址那次握手会晚一步失败，靠它丢掉过期回包（不清新一轮的在途标记、也不覆盖 cookie）
	quint64 m_authAttempt = 0;
	QList<QueuedCall> m_authQueue;          // 认证未就绪期间的请求（见 QueuedCall）

	QString m_clientId;                     // $events 流 ready 帧给的客户端 id（应答要带上）
	QString m_eventsStreamId;               // 逻辑流 id：$events
	QString m_workspaceStreamId;            // 逻辑流 id：workspace/follow
	QString m_controlStreamId;              // 逻辑流 id：session/control（主机级实时控制流）
	QString m_sessionStreamId;              // 逻辑流 id：session/follow（跟随当前会话）
	QString m_followedSessionId;            // 当前 session/follow 跟的是哪个会话
	bool m_eventsReady = false;             // $events 收到 ready 帧
	bool m_streamConnected = false;         // mux WebSocket 已打开
	int m_streamSeq = 0;                    // 逻辑流 id 自增号（服务端拒绝重复 id，且重复会关掉整条 mux）
	bool m_streamClosing = false;           // 正在主动关闭：此时断线不触发重连
	QTimer* m_reconnectTimer = nullptr;     // 断线后的重连定时器
	int m_reconnectDelayMs = 1000;          // 重连退避（重连成功后复位）

	bool m_destroyed = false;               // 正在析构，忽略后续回调
};
