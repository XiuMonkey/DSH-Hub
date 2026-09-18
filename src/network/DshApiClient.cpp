// ------------------------------------------------------------------
// DshApiClient.cpp
// ------------------------------------------------------------------
// DshApiClient 的实现。
//
// 职责：
//   - 维护 HTTP 请求和 WebSocket 连接；
//   - 自动为每个 RPC 请求生成 rpcId；
//   - 把 HTTP 响应解析成业务成功/失败回调；
//   - 把 WebSocket 收到的 JSON 帧原样通过信号抛给界面层。
// ------------------------------------------------------------------

#include "DshApiClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QWebSocket>

#include <QDebug>

DshApiClient::JsonParseRunnable::JsonParseRunnable(
	DshApiClient* client, QString rpcId, QByteArray body)
	: m_client(client)
	, m_rpcId(std::move(rpcId))
	, m_body(std::move(body))
{
}

// 在线程池 worker 中执行：只做纯 JSON 解析，不触碰任何回调；
// 完成后把文档回投到主线程的 handleParsedResponse() 再拆信封、调回调。
void DshApiClient::JsonParseRunnable::run()
{
	const QJsonDocument doc = QJsonDocument::fromJson(m_body);
	m_body.clear();
	if (m_client.isNull())
		return;
	QMetaObject::invokeMethod(m_client.data(),
		[client = m_client, rpcId = std::move(m_rpcId), doc]() {
			if (client)
				client->handleParsedResponse(rpcId, doc);
		},
		Qt::QueuedConnection);
}

/**
 * 构造函数。
 *
 * 初始化：
 * - QNetworkAccessManager：用于发送 HTTP POST 请求；
 * - QWebSocket m_stream：0.1.5 唯一的一条流通道 /api/remote.mux，
 *   所有逻辑流（$events、workspace/follow、session/follow）都跑在它上面。
 */
DshApiClient::DshApiClient(QObject* parent)
	: QObject(parent)
	, m_nam(new QNetworkAccessManager(this))
	, m_stream(new QWebSocket)
{
	connect(m_stream, &QWebSocket::textMessageReceived,
		this, &DshApiClient::onStreamTextMessage);

	connect(m_stream, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
		emit transportError(QStringLiteral("stream"), m_stream->errorString());
		qWarning().noquote() << "[DshApi] stream error:" << m_stream->errorString();
		});

	connect(m_stream, &QWebSocket::connected, this, [this] {
		m_streamConnected = true;
		m_reconnectDelayMs = 1000; // 连上了就把退避复位
		qInfo().noquote() << "[DshApi] mux connected -> opening $events + workspace/follow + session/control";
		// 三条常驻逻辑流：转发事件（含审批/提问）、工作区状态、会话实时控制状态
		m_eventsStreamId = nextStreamId(QStringLiteral("events"));
		m_workspaceStreamId = nextStreamId(QStringLiteral("workspace"));
		sendStreamOpen(QStringLiteral("$events"), m_eventsStreamId);
		sendStreamOpen(QStringLiteral("workspace/follow"), m_workspaceStreamId);
		// session/control：主机级实时控制流（会话投影变化、队列、作业）。
		// 输入区那行统计小灰字靠它做实时更新 —— 服务端现算现推，客户端不读任何缓存。
		m_controlStreamId = nextStreamId(QStringLiteral("control"));
		sendStreamOpen(QStringLiteral("session/control"), m_controlStreamId);
		// 换了连接代次，当前会话的 follow 流要重新开
		if (!m_followedSessionId.isEmpty()) {
			const QString sessionId = m_followedSessionId;
			m_followedSessionId.clear();
			m_sessionStreamId.clear();
			followSession(sessionId);
		}
		});

	connect(m_stream, &QWebSocket::disconnected, this, [this] {
		const int code = static_cast<int>(m_stream->closeCode());
		const QString reason = m_stream->closeReason();
		const bool wasConnected = m_streamConnected;

		m_streamConnected = false;
		m_eventsReady = false;
		qInfo().noquote() << "[DshApi] DSH stream disconnected closeCode=" << code
			<< "reason=" << reason << "wasConnected=" << wasConnected;

		// 断线必须自愈：服务端在收到重复 streamId / 非法帧时会主动关掉整条 mux
		// （close 1008），不重连的话所有逻辑流都再也开不出来。
		if (wasConnected)
			scheduleReconnect();
		});
}

/**
 * 析构函数。
 * 先关闭流通道，再释放 QWebSocket 对象。
 */
DshApiClient::~DshApiClient()
{
	m_destroyed = true;

	// 先断开 WebSocket 信号，避免析构过程中触发 lambda
	if (m_stream)
		disconnect(m_stream, nullptr, this, nullptr);

	closeStreams();
	delete m_stream;
}

/**
 * 设置 DSH 服务基础 URL。
 * 后续所有 HTTP 和 WebSocket 请求都会基于这个地址拼接。
 *
 * dsh 0.1.5 起服务端打印的是认证 URL（http://127.0.0.1:<port>/?token=<令牌>）：
 * 这里把令牌单独保存，基础 URL 保持干净，避免把 ?token= 拼到每个请求上。
 */
void DshApiClient::setBaseUrl(const QUrl& url)
{
	m_launchToken.clear();
	const QUrlQuery query(url);
	if (query.hasQueryItem(QStringLiteral("token")))
		m_launchToken = query.queryItemValue(QStringLiteral("token"));

	m_baseUrl = url;
	m_baseUrl.setQuery(QString());
	m_baseUrl.setFragment(QString());

	m_authCookie.clear();
	m_authenticated = false;

	qInfo().noquote() << "[DshApi] baseUrl set:" << m_baseUrl.toString()
		<< "launchToken:" << (m_launchToken.isEmpty() ? QStringLiteral("(none)") : QStringLiteral("present"));
}

/** 返回当前设置的基础 URL。 */
QUrl DshApiClient::baseUrl() const
{
	return m_baseUrl;
}

/**
 * 用启动令牌换取认证 cookie。
 *
 * GET http://127.0.0.1:<port>/?token=<令牌>
 *   -> 303 + Set-Cookie: dsh-auth-<authority 哈希>=<签名值>
 * 必须禁止自动跟随重定向，否则拿不到这一步的 Set-Cookie。
 * 成功后把 cookie 存进 m_authCookie 并继续 openStreams()。
 */
void DshApiClient::startAuthHandshake()
{
	if (m_destroyed || m_authInFlight)
		return;

	if (m_launchToken.isEmpty()) {
		m_authenticated = true;
		openStreams();
		return;
	}

	m_authInFlight = true;
	// 这一代握手的编号：回包时对不上就说明是"上一代（多半是重启前的旧地址）"的迟到回包，
	// 直接丢掉 —— 否则它会清掉新一轮的在途标记，甚至把旧服务端的 cookie 覆盖进来。
	const quint64 attempt = ++m_authAttempt;

	QUrl url = m_baseUrl;
	url.setPath(QStringLiteral("/"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("token"), m_launchToken);
	url.setQuery(query);

	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		QVariant::fromValue(QNetworkRequest::ManualRedirectPolicy));
	// 握手必须有超时：没有超时时连接会一直挂着，m_authInFlight 永远为真、
	// 后续重试全被挡掉（表现是"卡在没认证"再也起不来）。
	request.setTransferTimeout(5000);

	qInfo().noquote() << "[DshApi] auth handshake ->" << url.toString(QUrl::RemoveQuery) << "(token hidden)";
	QNetworkReply* reply = m_nam->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, attempt]() {
		reply->deleteLater();
		if (m_destroyed || attempt != m_authAttempt)
			return;
		m_authInFlight = false;

		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray header = reply->rawHeader("Set-Cookie");
		if (header.isEmpty()) {
			qWarning().noquote() << "[DshApi] auth handshake failed: HTTP" << status << reply->errorString();

			// HTTP 0（连接被拒/超时）＝传输层就没连上：服务端正在重启（扩展安装、插件市场
			// 装完、手动重启都会触发）时旧地址必然是这个结果，属于预期瞬态。
			// 这里只记日志 + 安排重连（ServerManager 给出新地址后会立刻重试），
			// **不往聊天区丢错误** —— 以前就是这么弹出一条
			// "传输错误[auth]: DSH 认证失败：服务端未返回 Set-Cookie(HTTP 0)" 的。
			if (status == 0) {
				qInfo().noquote() << "[DshApi] auth handshake 瞬态失败（服务端多半在重启），稍后重试";
				scheduleReconnect();
				return;
			}

			// 服务端真的答了却没有 Set-Cookie（404/500 之类）：这是配置问题，照旧报出来
			failAuthQueue(QStringLiteral("auth"),
				qtTrId("server_auth_failed_fmt").arg(status));
			emit transportError(QStringLiteral("auth"),
				qtTrId("server_auth_failed_fmt").arg(status));
			return;
		}

		int end = header.indexOf(';');
		if (end < 0)
			end = header.size();
		m_authCookie = QString::fromUtf8(header.left(end)).trimmed();
		m_authenticated = true;
		qInfo().noquote() << "[DshApi] auth cookie acquired (" << m_authCookie.size() << "chars )";

		// 认证就绪：把等待中的 RPC 按顺序补发（服务端重启窗口里点发送的那些）
		flushAuthQueue();
		openStreams();
		});
}

/**
 * 认证就绪：把挂起的 RPC 按顺序补发。
 * 补发走的还是 post()，此时 m_authenticated 已为真，不会再入队。
 */
void DshApiClient::flushAuthQueue()
{
	if (m_authQueue.isEmpty())
		return;

	qInfo().noquote() << "[DshApi] flush queued RPCs:" << m_authQueue.size();
	const QList<QueuedCall> queued = m_authQueue;
	m_authQueue.clear();
	for (const QueuedCall& call : queued)
		post(call.path, call.body, call.onSuccess, call.onError);
}

/** 认证硬失败：挂起的 RPC 用错误回掉，别让调用方永远等下去。 */
void DshApiClient::failAuthQueue(const QString& code, const QString& message)
{
	if (m_authQueue.isEmpty())
		return;

	const QList<QueuedCall> queued = m_authQueue;
	m_authQueue.clear();
	for (const QueuedCall& call : queued) {
		if (call.onError)
			call.onError(RpcError{ code, message, {} });
	}
}

/**
 * 打开 mux 和 host 两条 WebSocket 事件流。
 * 如果尚未设置 baseUrl，则直接返回。
 * 带令牌但尚未换到 cookie 时先做认证握手（握手成功会再回到这里）。
 */
void DshApiClient::openStreams()
{
	if (m_baseUrl.isEmpty()) {
		qWarning().noquote() << "[DshApi] openStreams ignored: baseUrl is empty";
		return;
	}

	if (!m_authenticated && !m_launchToken.isEmpty()) {
		qInfo().noquote() << "[DshApi] openStreams deferred: authenticating first";
		startAuthHandshake();
		return;
	}

	// 避免重复打开/残留旧连接
	closeStreams();

	QNetworkRequest streamRequest(makeUrl(QStringLiteral("/api/remote.mux")));
	if (!m_authCookie.isEmpty())
		streamRequest.setRawHeader("Cookie", m_authCookie.toUtf8());

	// 主动打开：清掉"正在关闭"标记，断线重连逻辑才继续有效
	m_streamClosing = false;
	m_stream->open(streamRequest);
	qInfo().noquote() << "[DshApi] opening mux stream:" << m_baseUrl.toString();
}

/**
 * 生成一条逻辑流 id：每次 open 都必须唯一。
 * 服务端在收到重复 streamId 时会 throw，而外层 catch 会 close(1008) 把整条 mux 关掉，
 * 所以复用 id（例如切会话时仍用 "session"）代价极高。
 */
QString DshApiClient::nextStreamId(const QString& prefix)
{
	++m_streamSeq;
	return QStringLiteral("%1-%2").arg(prefix).arg(m_streamSeq);
}

/**
 * 断线重连：延迟一小段时间后重新认证并打开 mux（重连成功会重开
 * $events / workspace/follow，并重新跟随当前会话）。
 */
void DshApiClient::scheduleReconnect()
{
	if (!m_reconnectTimer) {
		m_reconnectTimer = new QTimer(this);
		m_reconnectTimer->setSingleShot(true);
		connect(m_reconnectTimer, &QTimer::timeout, this, [this] {
			if (m_destroyed || m_baseUrl.isEmpty() || m_streamClosing)
				return;
			qInfo().noquote() << "[DshApi] reconnecting mux…";
			// 启动令牌是每进程常量，但 cookie 可能随连接代次失效，重连时重换一次更稳
			m_authenticated = false;
			m_authCookie.clear();
			openStreams();
			});
	}

	if (m_reconnectTimer->isActive())
		return;

	const int delayMs = m_reconnectDelayMs;
	m_reconnectDelayMs = qMin(m_reconnectDelayMs * 2, 8000);
	qInfo().noquote() << "[DshApi] mux reconnect scheduled in" << delayMs << "ms";
	m_reconnectTimer->start(delayMs);
}

/**
 * 关闭流通道，并重置连接/逻辑流状态标记。
 */
void DshApiClient::closeStreams()
{
	qInfo().noquote() << "[DshApi] closing stream channel";

	if (m_reconnectTimer)
		m_reconnectTimer->stop();

	m_streamClosing = true;
	if (m_stream)
		m_stream->close();

	m_streamConnected = false;
	m_eventsReady = false;
	m_clientId.clear();
	m_eventsStreamId.clear();
	m_workspaceStreamId.clear();
	m_controlStreamId.clear();
	m_sessionStreamId.clear();
	m_followedSessionId.clear();
}

/**
 * 在 mux 上打开一条逻辑流。
 * 帧格式（0.1.5）：{"type":"open","streamId":…,"endpoint":…,"payload":{"args":{…}}}
 */
void DshApiClient::sendStreamOpen(const QString& endpoint, const QString& streamId, const QJsonObject& args)
{
	if (!m_streamConnected || !m_stream || streamId.isEmpty()) {
		qWarning().noquote() << "[DshApi] cannot open stream" << endpoint << "(mux not connected)";
		return;
	}

	QJsonObject payload;
	payload.insert(QStringLiteral("args"), args);

	QJsonObject frame;
	frame.insert(QStringLiteral("type"), QStringLiteral("open"));
	frame.insert(QStringLiteral("streamId"), streamId);
	frame.insert(QStringLiteral("endpoint"), endpoint);
	frame.insert(QStringLiteral("payload"), payload);

	qInfo().noquote() << "[DshApi] stream open ->" << endpoint << "streamId=" << streamId;
	m_stream->sendTextMessage(QString::fromUtf8(QJsonDocument(frame).toJson(QJsonDocument::Compact)));
}

/** 取消一条逻辑流：{"type":"cancel","streamId":…} */
void DshApiClient::sendStreamCancel(const QString& streamId)
{
	if (!m_streamConnected || !m_stream || streamId.isEmpty())
		return;

	QJsonObject frame;
	frame.insert(QStringLiteral("type"), QStringLiteral("cancel"));
	frame.insert(QStringLiteral("streamId"), streamId);
	m_stream->sendTextMessage(QString::fromUtf8(QJsonDocument(frame).toJson(QJsonDocument::Compact)));
}

/**
 * 跟随一个会话：换掉旧的 session/follow 流，按 SessionFollowRequest 发地址。
 * 参数形状：{"request":{"address":{"kind":"session","sessionId":…}}}
 */
void DshApiClient::followSession(const QString& sessionId)
{
	if (sessionId.isEmpty() || sessionId == m_followedSessionId)
		return;

	unfollowSession();

	QJsonObject address;
	address.insert(QStringLiteral("kind"), QStringLiteral("session"));
	address.insert(QStringLiteral("sessionId"), sessionId);

	QJsonObject request;
	request.insert(QStringLiteral("address"), address);
	// 0.1.5：模型增量是**订阅式**的——不带这个参数，follow 流只发"已提交的持久事件"
	// （assistant/message 整段），于是界面只能等输出完才显示。带上它，服务端会把
	// agent/assistant-stream 的实时分片以 {type:'assistant-stream', frame} 推过来。
	request.insert(QStringLiteral("assistantStream"), true);

	QJsonObject args;
	args.insert(QStringLiteral("request"), request);

	m_followedSessionId = sessionId;
	m_sessionStreamId = nextStreamId(QStringLiteral("session"));
	sendStreamOpen(QStringLiteral("session/follow"), m_sessionStreamId, args);
}

/** 关闭当前的 session/follow 流。 */
void DshApiClient::unfollowSession()
{
	if (!m_sessionStreamId.isEmpty())
		sendStreamCancel(m_sessionStreamId);
	m_sessionStreamId.clear();
	m_followedSessionId.clear();
}

/**
 * 返回当前是否已连接 DSH。
 * 判定：mux WebSocket 已打开，且 $events 逻辑流已收到 ready 帧。
 */
bool DshApiClient::isConnected() const
{
	return m_streamConnected && m_eventsReady;
}

/**
 * 发送一元 RPC 请求。
 *
 * 构造的请求体：
 * {
 *   "type": "client-request",
 *   "rpcId": "<随机 UUID>",
 *   "method": "<方法名>",
 *   "payload": { ... }
 * }
 *
 * 然后 POST 到 /api/<method>。
 */
void DshApiClient::callMethod(
	const QString& method,
	const QJsonObject& payload,
	std::function<void(const QJsonObject& value)> onSuccess,
	std::function<void(const RpcError& error)> onError)
{
	const QString rpcId = QUuid::createUuid().toString(QUuid::WithoutBraces);

	// dsh 0.1.5: payload 必须恰好只有一个 args 对象，键名是描述符里的 wire 名
	QJsonObject argsPayload;
	argsPayload.insert(QStringLiteral("args"), payload);

	QJsonObject body;
	body.insert(QStringLiteral("type"), QStringLiteral("client-request"));
	body.insert(QStringLiteral("rpcId"), rpcId);
	body.insert(QStringLiteral("method"), method);
	body.insert(QStringLiteral("payload"), argsPayload);

	qInfo().noquote() << "[DshApi] RPC ->" << method << " rpcId=" << rpcId;

	post(QStringLiteral("/api/") + method, body,
		[onSuccess](const QJsonValue& value) {
			if (onSuccess)
				onSuccess(value.toObject());
		},
		std::move(onError));
}

/**
 * 同 callMethod，但成功回调拿裸 JSON 值。
 * 用于返回数组的端点（例如 llm/listConfigurableProviders）。
 */
void DshApiClient::callMethodValue(
	const QString& method,
	const QJsonObject& payload,
	std::function<void(const QJsonValue& value)> onSuccess,
	std::function<void(const RpcError& error)> onError)
{
	const QString rpcId = QUuid::createUuid().toString(QUuid::WithoutBraces);

	QJsonObject argsPayload;
	argsPayload.insert(QStringLiteral("args"), payload);

	QJsonObject body;
	body.insert(QStringLiteral("type"), QStringLiteral("client-request"));
	body.insert(QStringLiteral("rpcId"), rpcId);
	body.insert(QStringLiteral("method"), method);
	body.insert(QStringLiteral("payload"), argsPayload);

	qInfo().noquote() << "[DshApi] RPC ->" << method << " rpcId=" << rpcId;

	post(QStringLiteral("/api/") + method, body, std::move(onSuccess), std::move(onError));
}

/**
 * 应答审批/提问请求。
 *
 * dsh 0.1.5 的通道是 POST /api/$events/result，args 形状：
 * {
 *   "clientId": "<$events 流 ready 帧给的 id>",
 *   "eventId":  "<waterfall 帧里的 eventId>",
 *   "outcome":  { "kind": "result", "value": { ... } }
 * }
 * 传进来的 @p rpcId 就是帧里的 eventId（帧被翻译成旧形状时放在 rpcId 位置）。
 *
 * 返回的回执是普通的 server-response 信封，因此直接走 callMethod；
 * 旧的 /api/respond 端点已随 0.1.5 删除。
 */
void DshApiClient::respond(
	const QString& rpcId,
	const QJsonObject& value,
	std::function<void(const QJsonObject& receipt)> onSuccess,
	std::function<void(const RpcError& error)> onError)
{
	// dsh 0.1.5：/api/respond 已被删除。审批/提问是 $events 流上的 waterfall 帧，
	// 回执要 POST /api/$events/result，args = {clientId, eventId, outcome:{kind:"result", value}}。
	// 传进来的 rpcId 就是帧里的 eventId。
	if (m_clientId.isEmpty()) {
		qWarning().noquote() << "[DshApi] respond ignored: $events stream not ready";
		if (onError) {
			RpcError error;
			error.code = QStringLiteral("stream-not-ready");
			error.message = qtTrId("server_stream_not_ready");
			onError(error);
		}
		return;
	}

	QJsonObject outcome;
	outcome.insert(QStringLiteral("kind"), QStringLiteral("result"));
	outcome.insert(QStringLiteral("value"), value);

	QJsonObject args;
	args.insert(QStringLiteral("clientId"), m_clientId);
	args.insert(QStringLiteral("eventId"), rpcId);
	args.insert(QStringLiteral("outcome"), outcome);

	qInfo().noquote() << "[DshApi] respond -> $events/result eventId=" << rpcId;
	callMethod(QStringLiteral("$events/result"), args, onSuccess, onError);
}

/**
 * 把 HTTP 基础 URL 转换为对应的 WebSocket URL。
 *
 * 例如：
 *   http://127.0.0.1:3080  ->  ws://127.0.0.1:3080/api/remote.mux
 *   https://example.com     ->  wss://example.com/api/remote.mux
 */
QUrl DshApiClient::makeUrl(const QString& path) const
{
	QUrl url = m_baseUrl;
	url.setPath(path);
	if (url.scheme() == QStringLiteral("https"))
		url.setScheme(QStringLiteral("wss"));
	else
		url.setScheme(QStringLiteral("ws"));
	return url;
}

/**
 * 发送 HTTP POST JSON 请求。
 *
 * 1. 从请求体里取出 rpcId；
 * 2. 把回调保存到 m_pending，供响应回来时匹配；
 * 3. 使用 QNetworkAccessManager 发起异步 POST；
 * 4. 在 reply 上记录 rpcId，finished 时交给 onReplyFinished 统一处理。
 */
void DshApiClient::post(
	const QString& path,
	const QJsonObject& body,
	std::function<void(const QJsonValue& value)> onSuccess,
	std::function<void(const RpcError& error)> onError)
{
	// 认证还没就绪（启动早期 / 服务端刚重启）：先挂起来，等握手完成再补发。
	// 直发必然 401（0.1.5 的 /api 在认证围栏后面），用户看到的就是
	// "发出去的消息莫名失败"。ServerManager 重启服务端、扩展安装触发重启时都会走这里。
	if (!m_authenticated && !m_launchToken.isEmpty()) {
		static constexpr int kMaxQueuedCalls = 256;
		if (m_authQueue.size() >= kMaxQueuedCalls) {
			if (onError)
				onError(RpcError{ QStringLiteral("auth"),
					qtTrId("server_auth_queue_full_fmt").arg(kMaxQueuedCalls), {} });
			return;
		}

		m_authQueue.append(QueuedCall{ path, body, std::move(onSuccess), std::move(onError) });
		qInfo().noquote() << "[DshApi] RPC queued until authenticated:" << path
			<< "queued=" << m_authQueue.size();
		if (!m_authInFlight)
			startAuthHandshake();
		return;
	}

	const QString rpcId = body.value(QStringLiteral("rpcId")).toString();
	m_pending.insert(rpcId, PendingCall{
		path,
		std::move(onSuccess),
		std::move(onError),
		});

	QUrl url = m_baseUrl;
	url.setPath(path);

	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader,
		QStringLiteral("application/json"));
	// dsh 0.1.5: /api 需要认证 cookie，缺失时服务端直接 401
	if (!m_authCookie.isEmpty())
		request.setRawHeader("Cookie", m_authCookie.toUtf8());

	QNetworkReply* reply = m_nam->post(
		request,
		QJsonDocument(body).toJson(QJsonDocument::Compact));

	reply->setProperty("rpcId", rpcId);
	connect(reply, &QNetworkReply::finished, this, &DshApiClient::onReplyFinished);
}

/**
 * 所有 HTTP 请求的 finished 统一处理函数。
 *
 * 处理步骤：
 * 1. 从 reply 上取回 rpcId；
 * 2. 在 m_pending 中找到对应的回调；
 * 3. 先检查 HTTP 传输层错误；
 * 4. 再解析 JSON 响应体；
 * 5. 按 server-response 的 result.ok 判断业务成功/失败。
 */
void DshApiClient::onReplyFinished()
{
	QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
	if (!reply)
		return;

	// 析构期间忽略回调，避免调用已经析构的 MainWindow 等接收方
	if (m_destroyed) {
		reply->deleteLater();
		return;
	}

	const QString rpcId = reply->property("rpcId").toString();
	const auto it = m_pending.constFind(rpcId);
	if (it == m_pending.constEnd()) {
		qWarning().noquote() << "[DshApi] unknown rpcId:" << rpcId;
		reply->deleteLater();
		return;
	}

	PendingCall pending = it.value();
	m_pending.erase(it);

	if (reply->error() != QNetworkReply::NoError) {
		qWarning().noquote() << "[DshApi] HTTP transport error path=" << pending.path << " rpcId=" << rpcId << " error=" << reply->errorString();
		if (pending.onError) {
			pending.onError(RpcError{
				QStringLiteral("transport"),
				reply->errorString(),
				QJsonObject(),
				});
		}
		reply->deleteLater();
		return;
	}

	// 传输成功：把响应体解析挪到线程池 worker（大回包如 session/page 的历史页
	// 可达几百 KB~1MB，避免 fromJson 卡主线程），完成后回投 handleParsedResponse。
	const QByteArray body = reply->readAll();
	reply->deleteLater();

	m_parsing.insert(rpcId, pending);
	auto* task = new JsonParseRunnable(this, rpcId, body);
	QThreadPool::globalInstance()->start(task);
}

/**
 * 主线程：处理线程池解析完成的 HTTP 响应（拆 result 信封并调用回调）。
 */
void DshApiClient::handleParsedResponse(
	const QString& rpcId, const QJsonDocument& doc)
{
	if (m_destroyed)
		return;

	const auto it = m_parsing.constFind(rpcId);
	if (it == m_parsing.constEnd()) {
		qWarning().noquote() << "[DshApi] stale parsed response rpcId=" << rpcId;
		return;
	}
	PendingCall pending = it.value();
	m_parsing.erase(it);

	const QJsonObject root = doc.object();

	qInfo().noquote() << "[DshApi] HTTP response path=" << pending.path << " rpcId=" << rpcId;

	const QJsonObject result = root.value(QStringLiteral("result")).toObject();
	if (result.value(QStringLiteral("ok")).toBool()) {
		if (pending.onSuccess)
			pending.onSuccess(result.value(QStringLiteral("value")));
	}
	else {
		const QJsonObject errorObj = result.value(QStringLiteral("error")).toObject();
		if (pending.onError) {
			pending.onError(RpcError{
				errorObj.value(QStringLiteral("code")).toString(),
				errorObj.value(QStringLiteral("message")).toString(),
				errorObj.value(QStringLiteral("details")).toObject(),
				});
		}
	}
}

/**
 * mux WebSocket 收到文本消息。
 * 把 JSON 解析成 QJsonObject 后通过 muxFrameReceived 信号发出。
 */
 /**
  * $events 逻辑流的一项。
  * ready -> 记下 clientId 并宣告 connected；
  * emit -> 单向事件（当前 UI 用不到，留好分发点）；
  * waterfall -> 需要回执的审批/提问，翻成旧帧形状交给 UI（rpcId 位置放 eventId）。
  */
void DshApiClient::handleEventsItem(const QJsonValue& value)
{
	const QJsonObject item = value.toObject();
	const QString type = item.value(QStringLiteral("type")).toString();

	if (type == QStringLiteral("ready")) {
		m_clientId = item.value(QStringLiteral("clientId")).toString();
		m_eventsReady = true;
		qInfo().noquote() << "[DshApi] $events ready, clientId=" << m_clientId;
		emit connected();
		return;
	}

	if (type == QStringLiteral("emit")) {
		qInfo().noquote() << "[DshApi] emit event:" << item.value(QStringLiteral("event")).toString();
		return;
	}

	if (type == QStringLiteral("waterfall")) {
		const QString event = item.value(QStringLiteral("event")).toString();
		const QString eventId = item.value(QStringLiteral("eventId")).toString();
		const QString agentId = item.value(QStringLiteral("agentId")).toString();

		QJsonObject payload = item.value(QStringLiteral("request")).toObject();
		// 0.1.5 的请求体不一定带 sessionId；帧上的 agentId 就是 SessionId，补上
		if (!payload.contains(QStringLiteral("sessionId")) && !agentId.isEmpty())
			payload.insert(QStringLiteral("sessionId"), agentId);

		QString legacyType;
		if (event == QStringLiteral("approval/request"))
			legacyType = QStringLiteral("approval/requested");
		else if (event == QStringLiteral("user-questions/request"))
			legacyType = QStringLiteral("question/requested");

		if (legacyType.isEmpty())
			return;

		payload.insert(QStringLiteral("type"), legacyType);

		QJsonObject legacyFrame;
		legacyFrame.insert(QStringLiteral("rpcId"), eventId);
		legacyFrame.insert(QStringLiteral("payload"), payload);
		emit muxFrameReceived(legacyFrame);
	}
}

/** workspace/follow：baseline / upsert / remove / order / archived。 */
void DshApiClient::handleWorkspaceItem(const QJsonValue& value)
{
	const QJsonObject frame = value.toObject();
	const QString type = frame.value(QStringLiteral("type")).toString();

	if (type == QStringLiteral("baseline")) {
		const QJsonObject baseline = frame.value(QStringLiteral("value")).toObject();
		emit workspaceSnapshotReady(
			baseline.value(QStringLiteral("items")).toArray(),
			baseline.value(QStringLiteral("archivedSessionIds")).toArray());
		return;
	}

	if (type == QStringLiteral("upsert")) {
		emit workspaceUpserted(frame.value(QStringLiteral("workspace")).toObject());
		return;
	}

	if (type == QStringLiteral("remove")) {
		emit workspaceRemoved(frame.value(QStringLiteral("workspaceId")).toString());
		return;
	}

	if (type == QStringLiteral("archived")) {
		emit workspaceArchiveChanged(frame.value(QStringLiteral("archivedSessionIds")).toArray());
		return;
	}

	if (type == QStringLiteral("order")) {
		// 侧栏拖拽排序：order 帧给的是完整顺序
		QStringList workspaceIds;
		for (const auto& value : frame.value(QStringLiteral("workspaceIds")).toArray())
			workspaceIds.append(value.toString());
		emit workspaceReordered(workspaceIds);
		return;
	}

	// 其余帧（当前协议里没有）忽略
}

/**
 * session/control：主机级实时控制流。
 *
 * 帧形状（服务端 SessionControlController）：
 *   {type:"baseline",   value:{queues, jobs, projections:{<sessionId>:{asOfSeq, values}}}}
 *   {type:"projection", sessionId, key, value, seq}   // 某个会话的某个投影键变了
 *   {type:"queue"|"jobs", …}                          // 本客户端还没有消费方
 *
 * baseline 里的投影是"活会话"的现算快照（registry.snapshot），之后的 projection 帧是
 * 变化推送 —— 全程服务端现算现推，客户端不读任何缓存。
 * 注意 baseline 只覆盖当前活着的会话：只是打开看看、还没附着 Agent 的会话不在里面，
 * 那种情况由 session/follow 快照里的投影兜底（同样是服务端从日志现折叠）。
 */
void DshApiClient::handleControlItem(const QJsonValue& value)
{
	const QJsonObject frame = value.toObject();
	const QString type = frame.value(QStringLiteral("type")).toString();

	if (type == QStringLiteral("baseline")) {
		const QJsonObject baseline = frame.value(QStringLiteral("value")).toObject();
		const QJsonObject projections = baseline.value(QStringLiteral("projections")).toObject();
		qInfo().noquote() << "[DshApi] session/control baseline sessions=" << projections.size();
		if (!projections.isEmpty())
			emit sessionProjectionsBaselineReady(projections);
		return;
	}

	if (type == QStringLiteral("projection")) {
		const QString sessionId = frame.value(QStringLiteral("sessionId")).toString();
		const QString key = frame.value(QStringLiteral("key")).toString();
		if (sessionId.isEmpty() || key.isEmpty())
			return;

		emit sessionProjectionChanged(sessionId, key,
			frame.value(QStringLiteral("value")),
			frame.value(QStringLiteral("seq")).toInt());
		return;
	}

	// queue / jobs：忽略
}

/**
 * session/follow：snapshot（初始记录 + cursor）/ event（实时日志事件）。
 * 记录按顺序翻成旧的 session/event 帧，复用既有渲染路径。
 */
void DshApiClient::handleSessionItem(const QJsonValue& value)
{
	const QJsonObject frame = value.toObject();
	const QString type = frame.value(QStringLiteral("type")).toString();
	const QString sessionId = m_followedSessionId;

	if (type == QStringLiteral("snapshot")) {
		const int cursor = frame.value(QStringLiteral("cursor")).toInt();
		const QJsonArray records = frame.value(QStringLiteral("records")).toArray();
		const bool hasMore = frame.value(QStringLiteral("hasMore")).toBool();

		qInfo().noquote() << "[DshApi] session snapshot sessionId=" << sessionId
			<< "cursor=" << cursor << "records=" << records.size() << "hasMore=" << hasMore;

		// 快照帧还带着一份"全量折叠"的会话投影（projectionMode: all，直接从日志算）：
		// 冷会话也能立刻拿到整条日志的累计值，交给窗口侧去显示（会话统计小灰字）。
		const QJsonObject projections = frame.value(QStringLiteral("projections")).toObject();
		if (!projections.isEmpty()) {
			emit sessionProjectionsReady(sessionId,
				projections.value(QStringLiteral("asOfSeq")).toInt(),
				projections.value(QStringLiteral("values")).toObject());
		}

		// 快照记录不逐条走流式渲染：整包交给 HistoryLoader 批量播种
		// （sessionSnapshotReady -> seedFromSnapshot），否则历史会被渲染两遍。
		emit sessionSnapshotReady(sessionId, cursor, records, hasMore);
		return;
	}

	if (type == QStringLiteral("event")) {
		const QJsonObject event = frame.value(QStringLiteral("event")).toObject();
		if (!event.isEmpty())
			emitSessionEventFrame(sessionId, event);
		return;
	}

	// 0.1.5 的实时分片：{type:'assistant-stream', frame:{type:'start'|'chunk'|'end',…}}。
	// 其中 chunk 帧的 chunk 就是 LLM 的原始增量（text-delta / reasoning-delta /
	// block-start|end / usage），翻成既有的 assistant/chunk 事件后，渲染路径与
	// 历史回放完全共用——于是"逐字输出"在实时流上就自然成立了。
	if (type == QStringLiteral("assistant-stream")) {
		const QJsonObject streamFrame = frame.value(QStringLiteral("frame")).toObject();
		if (streamFrame.value(QStringLiteral("type")).toString() == QStringLiteral("chunk")) {
			const QJsonObject chunk = streamFrame.value(QStringLiteral("chunk")).toObject();
			if (!chunk.isEmpty()) {
				QJsonObject event;
				event.insert(QStringLiteral("type"), QStringLiteral("assistant/chunk"));
				QJsonObject data;
				data.insert(QStringLiteral("chunk"), chunk);
				event.insert(QStringLiteral("data"), data);
				emitSessionEventFrame(sessionId, event);
			}
		}
	}
}

/** 把一条日志事件翻成旧的 {payload:{type:"session/event",sessionId,event}} 帧。 */
void DshApiClient::emitSessionEventFrame(const QString& sessionId, const QJsonObject& event)
{
	QJsonObject payload;
	payload.insert(QStringLiteral("type"), QStringLiteral("session/event"));
	payload.insert(QStringLiteral("sessionId"), sessionId);
	payload.insert(QStringLiteral("event"), event);

	QJsonObject legacyFrame;
	legacyFrame.insert(QStringLiteral("payload"), payload);
	emit muxFrameReceived(legacyFrame);
}

void DshApiClient::onStreamTextMessage(const QString& message)
{
	const QJsonObject frame = QJsonDocument::fromJson(message.toUtf8()).object();
	const QString streamType = frame.value(QStringLiteral("type")).toString();
	const QString streamId = frame.value(QStringLiteral("streamId")).toString();
	if (frame.isEmpty()) {
		qWarning().noquote() << "[DshApi] empty frame received";
		return;
	}
	if (!frame.isEmpty())
		// 按逻辑流分发（m_eventsStreamId / m_workspaceStreamId / m_sessionStreamId）
		if (streamType == QStringLiteral("item")) {
			const QJsonValue streamValue = frame.value(QStringLiteral("value"));
			if (streamId == m_eventsStreamId)
				handleEventsItem(streamValue);
			else if (streamId == m_workspaceStreamId)
				handleWorkspaceItem(streamValue);
			else if (streamId == m_controlStreamId)
				handleControlItem(streamValue);
			else if (streamId == m_sessionStreamId)
				handleSessionItem(streamValue);
		}
		else if (streamType == QStringLiteral("end")) {
			qInfo().noquote() << "[DshApi] stream ended:" << streamId;
		}
		else if (streamType == QStringLiteral("error")) {
			const QJsonObject streamError = frame.value(QStringLiteral("error")).toObject();
			const QString code = streamError.value(QStringLiteral("code")).toString();
			const QString detail = streamError.value(QStringLiteral("message")).toString();
			qWarning().noquote() << "[DshApi] stream error" << streamId << code << detail;
			emit transportError(QStringLiteral("stream"), QStringLiteral("%1: %2").arg(code, detail));
		}
		else {
			qWarning().noquote() << "[DshApi] unknown mux frame type:" << streamType;
		}
}