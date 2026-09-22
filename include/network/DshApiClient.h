#pragma once

// ------------------------------------------------------------------
// DshApiClient.h
// ------------------------------------------------------------------
// DSH（DeepSeek Harness）本地 API 的轻量级 Qt 客户端（对齐 dsh 0.1.5）。
//
// 三件事：
//
// 1) 一元 RPC：POST /api/<namespace>/<method>
//      信封固定为 {type:"client-request", rpcId, method, payload:{args:{…}}}
//      —— payload 必须恰好只有一个 args 对象，键名是描述符里的 wire 名；
//      callMethod() 负责包这一层（调用方只给 args 内容），
//      callMethodValue() 用于返回**裸数组**的端点。
//      服务端答案统一是 {type:"server-response", rpcId, result:{ok, value|error}}。
//
// 2) 认证（0.1.5 新增）：/api 走浏览器认证栅栏
//      启动令牌只出现在服务端打印的认证 URL（http://127.0.0.1:<port>/?token=…），
//      用 GET 该 URL 换回 authority 绑定的 cookie（dsh-auth-<hash>=…），
//      之后所有 HTTP 请求与 WebSocket 握手都要带它，否则一律 401。
//      setBaseUrl() 取出令牌 → startAuthHandshake() 换 cookie → openStreams() 开流。
//
// 3) 流：单条 WebSocket /api/remote.mux，上面跑若干"逻辑流"
//      客户端 → 服务端：{type:"open", streamId, endpoint, payload:{args}} / {type:"cancel", streamId}
//      服务端 → 客户端：{type:"item", streamId, value} / {type:"end"} / {type:"error", error}
//      本客户端开三条：
//        $events          转发事件；首帧 {type:"ready", clientId, host}；
//                         waterfall 帧（审批/提问）需要回执 —— respond() 发
//                         POST /api/$events/result，args={clientId, eventId, outcome:{kind:"result", value}}
//        workspace/follow 工作区状态：baseline / upsert / remove / order / archived
//        session/follow   当前会话：首帧 snapshot（cursor + records + hasMore）播种首屏，
//                         之后 event 帧是实时日志
//      注意：逻辑流 id 必须唯一（服务端遇到重复 id 会 close(1008) 关掉整条 mux），
//      所以每次 open 都用 nextStreamId()；断线后由 scheduleReconnect() 自愈并重开三条流。
//
// 旧的 /api/events.mux、/api/events.host、POST /api/respond 在 0.1.5 已不存在，
// 本客户端不支持 0.1.1 及更早的服务端。
// ------------------------------------------------------------------

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

/**
 * DSH API 客户端类。
 *
 * 典型用法：
 * @code
 * DshApiClient api;
 * api.setBaseUrl(QUrl("http://127.0.0.1:3080"));
 * api.openStreams();
 *
 * api.callMethod("host.describe", {},
 *     [](const QJsonObject &value) { qDebug() << value; },
 *     [](const DshApiClient::RpcError &err) { qWarning() << err.message; });
 * @endcode
 *
 * 事件使用方式：
 * @code
 * connect(&api, &DshApiClient::muxFrameReceived, ...);
 * @endcode
 */
class DshApiClient : public QObject
{
	Q_OBJECT

public:
	/**
	 * RPC 业务错误信息。
	 * 当 HTTP 请求成功，但 DSH 返回 result.ok == false 时使用。
	 */
	struct RpcError
	{
		QString code;       // 错误码，例如 session-not-found、MISSING_CREDENTIAL
		QString message;    // 错误描述
	};

	/**
	 * 构造函数。
	 * 创建 QNetworkAccessManager 和两个 QWebSocket 对象，并连接内部信号。
	 */
	explicit DshApiClient(QObject* parent = nullptr);

	/**
	 * 析构函数。
	 * 关闭 WebSocket 流并释放内部对象。
	 */
	~DshApiClient() override;

	/**
	 * 设置 DSH 服务的基础 URL。
	 * 例如 http://127.0.0.1:3080
	 */
	void setBaseUrl(const QUrl& url);

	/** 返回当前设置的 DSH 基础 URL。 */
	QUrl baseUrl() const;

	/**
	 * 启动令牌（认证 URL 上的 ?token=）。
	 *
	 * 0.1.5 起 /api 在浏览器认证围栏之后：没有 cookie 一律 401，而 cookie 只能由
	 * GET /?token=<启动令牌> 换到。宿主需要"另起一个窗口但仍连同一个服务端"时
	 * （主题切换就是重建主窗口），必须把令牌一起带过去，否则新窗口认证不了、
	 * 拿不到任何数据，界面会永远停在初始化里。
	 */
	QString launchToken() const { return m_launchToken; }

	/**
	 * 打开两条服务端推送 WebSocket 流：
	 *   /api/events.mux
	 *   /api/events.host
	 * 两条流都连接成功后会发出 connected() 信号。
	 */
	void openStreams();

	/**
	 * 关闭流通道（WebSocket + 它上面的全部逻辑流）。
	 */
	void closeStreams();

	/**
	 * 跟随一个会话：在 mux 上开 session/follow 逻辑流（换会话时自动换流）。
	 *
	 * 快照帧（初始记录 + cursor）通过 sessionSnapshotReady 发出，
	 * 之后的实时日志事件经 muxFrameReceived 交给现有渲染路径。
	 */
	void followSession(const QString& sessionId);

	/** 关闭当前的 session/follow 逻辑流。 */
	void unfollowSession();

	/**
	 * 返回当前是否已连接 DSH。
	 * 判定标准：mux WebSocket 已打开，且 $events 逻辑流已收到 ready 帧。
	 */
	bool isConnected() const;

	/**
	 * 发送一个一元 RPC 请求。
	 *
	 * dsh 0.1.5 起 endpoint 是 `<namespace>/<method>`（斜杠，不再是点号），
	 * 且请求体的 payload 必须恰好只有一个 `args` 对象，其键名是描述符里的 wire 名：
	 *   payload = { "args": { <wire 名>: <值>, … } }
	 * 所以本函数的 @p payload 形状是 **args 的内容**，由这里负责包一层 args。
	 *
	 * 例如：
	 *   callMethod("session/list",     { {"_request", {}} });   // wire 名 _request
	 *   callMethod("session/create",   { {"request", {{"cwd", path}}} });
	 *   callMethod("agentPresets/list", {});                    // 无参端点 args 为空
	 *
	 * 返回值是裸数组的端点（如 llm/listConfigurableProviders）用 callMethodValue。
	 *
	 * 客户端会自动生成 rpcId。
	 *
	 * @param method    DSH endpoint，例如 "session/list"、"session/prompt"
	 * @param payload   该端点的 args 对象（见上）
	 * @param onSuccess 成功后回调，参数是 result.value（对象）
	 * @param onError   失败后回调，参数是 RpcError
	 */
	void callMethod(
		const QString& method,
		const QJsonObject& payload = {},
		std::function<void(const QJsonObject& value)> onSuccess = {},
		std::function<void(const RpcError& error)> onError = {});

	/**
	 * 同 callMethod，但成功回调拿裸 JSON 值（端点返回数组时用它，
	 * 例如 llm/listConfigurableProviders -> [{provider, displayName, settingsNs, …}]）。
	 */
	void callMethodValue(
		const QString& method,
		const QJsonObject& payload,
		std::function<void(const QJsonValue& value)> onSuccess,
		std::function<void(const RpcError& error)> onError = {});

	/**
	 * 应答从 mux 流收到的审批/提问帧。
	 *
	 * @param rpcId     服务端帧里的 rpcId
	 * @param value     应答内容，例如：
	 *                  { "sessionId": "...", "approvalId": "...", "outcome": "allowed-once" }
	 * @param onSuccess 应答被接受后回调，参数是 { "accepted": true }
	 * @param onError   应答被拒绝或传输失败后回调
	 */
	void respond(
		const QString& rpcId,
		const QJsonObject& value,
		std::function<void(const QJsonObject& receipt)> onSuccess = {},
		std::function<void(const RpcError& error)> onError = {});

signals:
	/** mux 上 $events 逻辑流收到 ready 帧（事件源就绪）后发出。 */
	void connected();

	/**
	 * 兼容旧消费者的事件帧。
	 *
	 * 0.1.5 的线上帧形状变了，这里把新帧**翻译成旧形状**再发出，
	 * 好让 DSHHub::handleMuxFrame 与 InteractionHandler 保持不动：
	 *   session/follow 的 event/snapshot 记录
	 *     -> { payload: { type:"session/event", sessionId, event } }
	 *   $events 的 waterfall approval/request
	 *     -> { rpcId:<eventId>, payload:{ type:"approval/requested", … } }
	 *   $events 的 waterfall user-questions/request
	 *     -> { rpcId:<eventId>, payload:{ type:"question/requested", … } }
	 * rpcId 用的是 0.1.5 的 eventId，respond() 会把它发回 /api/$events/result。
	 */
	void muxFrameReceived(const QJsonObject& frame);

	/** workspace/follow 的 baseline 帧：工作区清单 + 完整归档集合。 */
	void workspaceSnapshotReady(const QJsonArray& items, const QJsonArray& archivedSessionIds);

	/** workspace/follow 的 upsert 帧：新增/更新一个工作区。 */
	void workspaceUpserted(const QJsonObject& workspace);

	/** workspace/follow 的 remove 帧：删掉一个工作区。 */
	void workspaceRemoved(const QString& workspaceId);

	/** workspace/follow 的 order 帧：工作区排序整体替换（侧栏拖拽排序）。 */
	void workspaceReordered(const QStringList& workspaceIds);

	/** workspace/follow 的 archived 帧：归档集合整体替换。 */
	void workspaceArchiveChanged(const QJsonArray& archivedSessionIds);

	/**
	 * session/follow 的 snapshot 帧：一次冷读的初始记录 + 游标。
	 * @param cursor 该会话日志的切割点，即 session/page 需要的 throughSeq。
	 */
	void sessionSnapshotReady(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore);

	/**
	 * session/follow 快照帧里带的会话投影（帧里的 projections: {asOfSeq, values}）。
	 *
	 * 为什么值得单独发：快照这条路上服务端用的是"全量折叠"（projectionMode: all），
	 * 即**直接从日志算**每个投影单位的值。于是"没被 Agent 附着"的会话（DSH Hub 里
	 * 只是打开来看的那些）也能立刻拿到整条日志的累计值 —— 而 session/list 行给的是
	 * 投影缓存里的检查点，可能为空或很旧（缓存按 200 事件/5s 落盘）。
	 * 目前只服务输入区下方那行会话统计（见 DSHHub::refreshSessionStats）。
	 *
	 * @param asOfSeq 这些值反映的最后一条事件的 seq。
	 * @param values  各投影键的客户端可见值（sessionStats / tokenUsage …）。
	 */
	void sessionProjectionsReady(const QString& sessionId, int asOfSeq, const QJsonObject& values);

	/**
	 * session/control 流的 baseline：每个"活着的"会话一份现算的投影快照。
	 * 键是 sessionId，值是 {asOfSeq, values}（与其它载体同一套形状）。
	 */
	void sessionProjectionsBaselineReady(const QJsonObject& projectionsBySession);

	/**
	 * session/control 流的实时帧：某个会话的某个投影键变了（服务端现算现推）。
	 * 小灰字靠它做到"每步实时跳"，而且全程不读任何缓存。
	 */
	void sessionProjectionChanged(const QString& sessionId, const QString& key,
		const QJsonValue& value, int seq);

	/**
	 * 传输层错误信号。
	 * @param context 出错来源，例如 "stream" 或 HTTP 请求
	 * @param message 错误描述
	 */
	void transportError(const QString& context, const QString& message);

private:
	/**
	 * 保存一个尚未收到响应的 HTTP RPC 请求。
	 * 用于在响应返回时匹配回调。
	 */
	struct PendingCall
	{
		QString path;       // 请求的 API 路径，例如 /api/session/list
		std::function<void(const QJsonValue& value)> onSuccess;
		std::function<void(const RpcError& error)> onError;
	};

	/**
	 * 认证还没就绪时先挂起来的 RPC（见 post()）。
	 *
	 * 为什么要挂：0.1.5 的 /api 在认证围栏后面，"没有 cookie 就直发"必然 401。
	 * 服务端刚重启（扩展安装、插件市场装完、手动重启都会触发）那一两秒里，
	 * 用户点发送就会撞上这个窗口。挂起来等握手完成再按顺序补发即可。
	 */
	struct QueuedCall
	{
		QString path;
		QJsonObject body;
		std::function<void(const QJsonValue&)> onSuccess;
		std::function<void(const RpcError&)> onError;
	};

	/**
	 * 在线程池里解析 HTTP 响应体的任务（大 JSON 回包不在主线程 fromJson）。
	 * 解析完成后通过 queued invokeMethod 回投主线程的 handleParsedResponse()。
	 */
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

	/**
	 * 主线程处理解析完成后的响应（拆 result 信封并调用成功/失败回调）。
	 */
	void handleParsedResponse(const QString& rpcId, const QJsonDocument& doc);

	/**
	 * 把 HTTP URL 转换成对应的 WebSocket URL。
	 * http -> ws，https -> wss。
	 */
	QUrl makeUrl(const QString& path) const;

	/**
	 * 发送一个 HTTP POST JSON 请求。
	 * 内部会把 PendingCall 保存到 m_pending，等待响应。
	 */
	void post(
		const QString& path,
		const QJsonObject& body,
		std::function<void(const QJsonValue& value)> onSuccess,
		std::function<void(const RpcError& error)> onError);

	/**
	 * dsh 0.1.5 认证握手：用启动令牌换认证 cookie。
	 *
	 * 服务端打印的认证 URL 形如 http://127.0.0.1:<port>/?token=<令牌>；
	 * GET 该地址返回 303 + Set-Cookie: dsh-auth-<authority 哈希>=<签名值>，
	 * 之后所有 /api 请求与 WebSocket 握手都必须带上该 cookie，否则一律 401。
	 * 握手成功后自动调用 openStreams()。
	 */
	void startAuthHandshake();

private slots:
	/** QNetworkReply::finished 的统一处理槽。 */
	void onReplyFinished();

private:
	/**
	 * mux WebSocket 收到一条文本消息：按 type/streamId 路由。
	 */
	void onStreamTextMessage(const QString& message);

	/** 在 mux 上打开一条逻辑流（frame: {type:"open",streamId,endpoint,payload:{args}}）。 */
	void sendStreamOpen(const QString& endpoint, const QString& streamId, const QJsonObject& args = {});

	/** 取消一条逻辑流（frame: {type:"cancel",streamId}）。 */
	void sendStreamCancel(const QString& streamId);

	/** $events 逻辑流的一项：ready / emit / waterfall。 */
	void handleEventsItem(const QJsonValue& value);

	/** workspace/follow 逻辑流的一项：baseline / upsert / remove / order / archived。 */
	void handleWorkspaceItem(const QJsonValue& value);

	/** session/follow 逻辑流的一项：snapshot / event。 */
	void handleSessionItem(const QJsonValue& value);

	/** session/control 逻辑流的一项：baseline / projection（queue、jobs 忽略）。 */
	void handleControlItem(const QJsonValue& value);

	/** 把一条会话日志事件翻成旧的 {payload:{type:"session/event",…}} 帧发出。 */
	void emitSessionEventFrame(const QString& sessionId, const QJsonObject& event);

	/**
	 * 安排一次 mux 重连（断线后自动恢复；重连成功会重开 $events/workspace/follow
	 * 并重新跟随当前会话）。
	 */
	void scheduleReconnect();

	/** 生成一条逻辑流 id（每次 open 都必须唯一）。 */
	QString nextStreamId(const QString& prefix);

	/** 认证就绪后把挂起的 RPC 按顺序补发。 */
	void flushAuthQueue();
	/** 认证硬失败时把挂起的 RPC 用错误回掉（不能让调用方永远挂着）。 */
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
	// 认证"代次"：服务端重启后旧地址那次握手会晚一步失败，
	// 靠它把过期回包丢掉，不让它清掉新一轮的在途标记、也不让它覆盖 cookie
	quint64 m_authAttempt = 0;
	QList<QueuedCall> m_authQueue;          // 认证未就绪期间的请求（见 QueuedCall）

	// ---- /api/remote.mux 上的逻辑流 ----
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
