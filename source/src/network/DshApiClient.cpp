// DshApiClient.cpp
// DshApiClient 的实现。职责：维护 HTTP 请求和 WebSocket 连接；自动为每个 RPC 请求生成 rpcId；把 HTTP 响应解析成业务成功/失败回调；把 WebSocket 收到的 JSON 帧原样通过信号抛给界面层。

#include "network/DshApiClient.h"
#include "core/ConnectionManager.h"
#include "core/HostExports.h"
#include "common/util/CommonRegistry.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
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

// 在线程池 worker 中执行：只做纯 JSON 解析，不触碰任何回调；完成后把文档回投到主线程的 handleParsedResponse() 再拆信封、调回调。
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

// 构造函数：建 QNetworkAccessManager（发 HTTP POST）与 QWebSocket m_stream —— 后者是 0.1.5 唯一的一条流通道 /api/remote.mux，所有逻辑流（$events、workspace/follow、session/follow）都跑在它上面。
DshApiClient::DshApiClient(QObject* parent)
	: QObject(parent)
	, m_nam(new QNetworkAccessManager(this))
	, m_stream(new QWebSocket)
{
	dshRegister("DshApiClient.001",
		m_stream, &QWebSocket::textMessageReceived,
		this, &DshApiClient::onStreamTextMessage);

	dshRegister("DshApiClient.002",
		m_stream, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
			emit transportError(QStringLiteral("stream"), m_stream->errorString());
			qWarning().noquote() << "[DshApi] stream error:" << m_stream->errorString();
		});

	dshRegister("DshApiClient.003",
		m_stream, &QWebSocket::connected, this, [this] {
		m_streamConnected = true;
		m_reconnectDelayMs = 1000; // 连上了就把退避复位
		qInfo().noquote() << "[DshApi] mux connected -> opening $events + workspace/follow + session/control";
		// 三条常驻逻辑流：转发事件（含审批/提问）、工作区状态、会话实时控制状态
		m_eventsStreamId = nextStreamId(QStringLiteral("events"));
		m_workspaceStreamId = nextStreamId(QStringLiteral("workspace"));
		sendStreamOpen(QStringLiteral("$events"), m_eventsStreamId);
		sendStreamOpen(QStringLiteral("workspace/follow"), m_workspaceStreamId);
		// session/control：主机级实时控制流（会话投影变化、队列、作业）；输入区那行统计小灰字靠它做实时更新 —— 服务端现算现推，客户端不读任何缓存。
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

	dshRegister("DshApiClient.004",
		m_stream, &QWebSocket::disconnected, this, [this] {
		const int code = static_cast<int>(m_stream->closeCode());
		const QString reason = m_stream->closeReason();
		const bool wasConnected = m_streamConnected;

		m_streamConnected = false;
		m_eventsReady = false;
		qInfo().noquote() << "[DshApi] DSH stream disconnected closeCode=" << code
			<< "reason=" << reason << "wasConnected=" << wasConnected;

		// 断线必须自愈：服务端在收到重复 streamId / 非法帧时会主动关掉整条 mux（close 1008），不重连的话所有逻辑流都再也开不出来。
		if (wasConnected)
			scheduleReconnect();
	});

	// 接管态下的回填超时扫描：扩展拿到请求却不回填时，这是唯一能让调用方脱身的机制。
	// 用一个周期定时器而不是每条请求建一个 QTimer —— 首屏那批出站（session/list +
	// 十几条 session/page）一口气就是十几条，逐个建定时器不划算。
	m_takeoverSweep = new QTimer(this);
	m_takeoverSweep->setInterval(5000);
	dshRegister("DshApiClient.006",
		m_takeoverSweep, &QTimer::timeout, this, &DshApiClient::sweepTakeoverTimeouts);
}

// 析构：先关闭流通道，再释放 QWebSocket 对象。
DshApiClient::~DshApiClient()
{
	m_destroyed = true;

	// 先断开 WebSocket 信号，避免析构过程中触发 lambda
	if (m_stream)
		disconnect(m_stream, nullptr, this, nullptr);

	closeStreams();
	delete m_stream;
}

// 设置 DSH 服务基础 URL，后续所有 HTTP/WebSocket 请求都基于它拼接；dsh 0.1.5 起服务端打印的是认证 URL（http://127.0.0.1:<port>/?token=<令牌>），这里把令牌单独保存、基础 URL 保持干净，避免把 ?token= 拼到每个请求上。
void DshApiClient::setBaseUrl(const QUrl& url)
{
	// 接管态：这条地址指向的正是我们即将停掉（或已经停掉）的内置服务端，接下来还会在
	// baseUrlReady 的接线里顺带调 openStreams()。一句"忽略"就把 §3.1 里最容易漏的那条
	// 堵住了 —— 否则接管后仍会做 token→cookie 握手、连真 DSH。
	if (m_takenover) {
		qInfo().noquote() << "[DshApi] setBaseUrl ignored (backend taken over):"
			<< url.toString(QUrl::RemoveQuery);
		return;
	}

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

QUrl DshApiClient::baseUrl() const
{
	// D6=B：接管态下恒为空 —— 那条地址指向的 DSH 已经不在了，回显（Settings.cpp:361）只会
	// 误导用户；而唯一靠它干活的路径（切主题把地址带给新窗口）由 ServerManager 的进程级
	// 接管标记挡住，不会因为拿到空值就把内置服务端重新拉起来。
	if (m_takenover)
		return QUrl();
	return m_baseUrl;
}

// 用启动令牌换取认证 cookie：GET http://127.0.0.1:<port>/?token=<令牌> -> 303 + Set-Cookie: dsh-auth-<authority 哈希>=<签名值>；必须禁止自动跟随重定向，否则拿不到这一步的 Set-Cookie；成功后把 cookie 存进 m_authCookie 并继续 openStreams()。
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
	// 这一代握手的编号：回包时对不上就说明是"上一代（多半是重启前的旧地址）"的迟到回包，直接丢掉 —— 否则它会清掉新一轮的在途标记，甚至把旧服务端的 cookie 覆盖进来。
	const quint64 attempt = ++m_authAttempt;

	QUrl url = m_baseUrl;
	url.setPath(QStringLiteral("/"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("token"), m_launchToken);
	url.setQuery(query);

	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		QVariant::fromValue(QNetworkRequest::ManualRedirectPolicy));
	// 握手必须有超时：没有超时时连接会一直挂着，m_authInFlight 永远为真、后续重试全被挡掉（表现是"卡在没认证"再也起不来）。
	request.setTransferTimeout(5000);

	qInfo().noquote() << "[DshApi] auth handshake ->" << url.toString(QUrl::RemoveQuery) << "(token hidden)";
	QNetworkReply* reply = m_nam->get(request);
	// reply 用完即删，不进登记表
	connect(reply, &QNetworkReply::finished, this, [this, reply, attempt]() {
		reply->deleteLater();
		if (m_destroyed || attempt != m_authAttempt)
			return;
		m_authInFlight = false;

		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray header = reply->rawHeader("Set-Cookie");
		if (header.isEmpty()) {
			qWarning().noquote() << "[DshApi] auth handshake failed: HTTP" << status << reply->errorString();

			// HTTP 0（连接被拒/超时）＝传输层就没连上：服务端正在重启（扩展安装、插件市场装完、手动重启都会触发）时旧地址必然是这个结果，属于预期瞬态；这里只记日志 + 安排重连（ServerManager 给出新地址后会立刻重试），不往聊天区丢错误 —— 以前就是这么弹出一条"传输错误[auth]: DSH 认证失败：服务端未返回 Set-Cookie(HTTP 0)"的。
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

// 认证就绪：把挂起的 RPC 按顺序补发；补发走的还是 post()，此时 m_authenticated 已为真，不会再入队。
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
			call.onError(RpcError{ code, message });
	}
}

// 打开 WebSocket 事件流；尚未设置 baseUrl 就直接返回，带令牌但还没换到 cookie 时先做认证握手（握手成功会再回到这里）。
void DshApiClient::openStreams()
{
	// 接管态：流由扩展负责，宿主不开任何 mux。扩展开始喂数据的时机是它自己那次
	// Takenover(true)，所以这里不需要（也没有）回执。
	if (m_takenover) {
		qInfo().noquote() << "[DshApi] openStreams ignored (backend taken over)";
		return;
	}

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

// 生成一条逻辑流 id：每次 open 都必须唯一 —— 服务端在收到重复 streamId 时会 throw，而外层 catch 会 close(1008) 把整条 mux 关掉，所以复用 id（例如切会话时仍用 "session"）代价极高。
QString DshApiClient::nextStreamId(const QString& prefix)
{
	++m_streamSeq;
	return QStringLiteral("%1-%2").arg(prefix).arg(m_streamSeq);
}

// 断线重连：延迟一小段时间后重新认证并打开 mux（重连成功会重开 $events / workspace/follow，并重新跟随当前会话）。
void DshApiClient::scheduleReconnect()
{
	if (!m_reconnectTimer) {
		m_reconnectTimer = new QTimer(this);
		m_reconnectTimer->setSingleShot(true);
		dshRegister("DshApiClient.005",
			m_reconnectTimer, &QTimer::timeout, this, [this] {
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

void DshApiClient::closeStreams()
{
	qInfo().noquote() << "[DshApi] closing stream channel"
		<< (m_takenover ? QStringLiteral("(takenover: local bookkeeping only)") : QString());

	if (m_reconnectTimer)
		m_reconnectTimer->stop();

	// 接管态：不碰 WebSocket、也不通知扩展 —— 这条路径会被析构调用（~DshApiClient），
	// 那一刻回调进插件不安全。扩展要退场靠自己的 detachHost() 调 Takenover(false)。
	if (!m_takenover) {
		m_streamClosing = true;
		if (m_stream)
			m_stream->close();
	}

	m_streamConnected = false;
	m_eventsReady = false;
	m_clientId.clear();
	m_eventsStreamId.clear();
	m_workspaceStreamId.clear();
	m_controlStreamId.clear();
	m_sessionStreamId.clear();
	m_followedSessionId.clear();
}

// 在 mux 上打开一条逻辑流；帧格式（0.1.5）：{"type":"open","streamId":…,"endpoint":…,"payload":{"args":{…}}}
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

// 跟随一个会话：换掉旧的 session/follow 流，按 SessionFollowRequest 发地址；参数形状 {"request":{"address":{"kind":"session","sessionId":…}}}。
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
	// 0.1.5：模型增量是订阅式的 —— 不带这个参数，follow 流只发"已提交的持久事件"（assistant/message 整段），于是界面只能等输出完才显示；带上它，服务端会把 agent/assistant-stream 的实时分片以 {type:'assistant-stream', frame} 推过来。
	request.insert(QStringLiteral("assistantStream"), true);

	QJsonObject args;
	args.insert(QStringLiteral("request"), request);

	m_followedSessionId = sessionId;

	// 接管态：不开真流，改成把"界面要看哪个会话"告诉扩展 —— 它不知道这件事就没法喂历史。
	// 载荷用的是与 mux open 帧**同一份** args（endpoint + args），便于扩展照着 DSH 的形状做。
	if (m_takenover) {
		QJsonObject frame;
		frame.insert(QStringLiteral("endpoint"), QStringLiteral("session/follow"));
		frame.insert(QStringLiteral("args"), args);
		const QString rpcId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		// fire-and-forget：不入 m_pending、不等回填（回填了也只会记一条"不认识的 rpcId"）
		sendToSink(rpcId, QStringLiteral("$takeover/stream-open"), frame);
		return;
	}

	m_sessionStreamId = nextStreamId(QStringLiteral("session"));
	sendStreamOpen(QStringLiteral("session/follow"), m_sessionStreamId, args);
}

void DshApiClient::unfollowSession()
{
	// 接管态：通知扩展收掉 follow（同样是 fire-and-forget）
	if (m_takenover) {
		if (!m_followedSessionId.isEmpty()) {
			QJsonObject frame;
			frame.insert(QStringLiteral("endpoint"), QStringLiteral("session/follow"));
			QJsonObject args;
			frame.insert(QStringLiteral("args"), args);
			const QString rpcId = QUuid::createUuid().toString(QUuid::WithoutBraces);
			sendToSink(rpcId, QStringLiteral("$takeover/stream-cancel"), frame);
		}
		m_sessionStreamId.clear();
		m_followedSessionId.clear();
		return;
	}

	if (!m_sessionStreamId.isEmpty())
		sendStreamCancel(m_sessionStreamId);
	m_sessionStreamId.clear();
	m_followedSessionId.clear();
}

// 是否已连接 DSH：mux WebSocket 已打开，且 $events 逻辑流已收到 ready 帧。
bool DshApiClient::isConnected() const
{
	// D6=B：接管态下恒 true —— 传输归扩展所有，宿主这边没有任何"没连上"的判据。
	// 顺带压掉唯一那处调用（DSHHub.cpp:169 的"服务端已退出"提示）：接管时那个进程
	// 正是我们主动杀掉的，不该弹给用户。
	if (m_takenover)
		return true;
	return m_streamConnected && m_eventsReady;
}

// 发送一元 RPC 请求：请求体 {"type":"client-request","rpcId":<随机 UUID>,"method":<方法名>,"payload":{...}}，然后 POST 到 /api/<method>。
// 接管态的分流在 post() 里（本方法没有 HTTP 之前的副作用，只需汇到那一处）。
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

// 同 callMethod，但成功回调拿裸 JSON 值；用于返回数组的端点（例如 llm/listConfigurableProviders）。
// 接管态的分流在 post() 里（与 callMethod 同一处）。
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

// 应答审批/提问请求：dsh 0.1.5 的通道是 POST /api/$events/result，args 形状 = {"clientId": <$events 流 ready 帧给的 id>, "eventId": <waterfall 帧里的 eventId>, "outcome": {"kind":"result","value":{...}}}；传进来的 rpcId 就是帧里的 eventId（帧被翻译成旧形状时放在 rpcId 位置）。返回的回执是普通 server-response 信封，故直接走 callMethod；旧的 /api/respond 端点已随 0.1.5 删除。
void DshApiClient::respond(
	const QString& rpcId,
	const QJsonObject& value,
	std::function<void(const QJsonObject& receipt)> onSuccess,
	std::function<void(const RpcError& error)> onError)
{
	// ⚠️ 接管态必须绕过这个前置门槛：接管后不连 mux ⇒ m_clientId 永远为空 ⇒ 审批/提问的
	// 应答会被这里直接吞掉（只发一个 stream-not-ready 错误），扩展根本收不到。
	if (!m_takenover && m_clientId.isEmpty()) {
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
	// 接管态下 m_clientId 是空的：这里就**不带** clientId 交出去（扩展自己那份 clientId /
	// 会话映射由它自己补），而不是塞一个空串让它去猜。
	if (!m_clientId.isEmpty())
		args.insert(QStringLiteral("clientId"), m_clientId);
	args.insert(QStringLiteral("eventId"), rpcId);
	args.insert(QStringLiteral("outcome"), outcome);

	qInfo().noquote() << "[DshApi] respond -> $events/result eventId=" << rpcId;
	callMethod(QStringLiteral("$events/result"), args, onSuccess, onError);
}

// 把 HTTP 基础 URL 转换为对应的 WebSocket URL：http://127.0.0.1:3080 -> ws://127.0.0.1:3080/api/remote.mux，https://example.com -> wss://example.com/api/remote.mux。
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

// 发送 HTTP POST JSON 请求：先从请求体取出 rpcId，把回调保存到 m_pending 供响应回来时匹配，再异步 POST，并在 reply 上记录 rpcId，finished 时交给 onReplyFinished 统一处理。
void DshApiClient::post(
	const QString& path,
	const QJsonObject& body,
	std::function<void(const QJsonValue& value)> onSuccess,
	std::function<void(const RpcError& error)> onError)
{
	// ⚠️ 接管态：不走 HTTP —— 三个一元 RPC 方法（callMethod / callMethodValue / respond）
	// 最终都汇到这一个函数，所以分流转发写在这里就覆盖了它们三个。
	// 位置刻意在下面的**认证队列之前**：反过来的话，接管态下的请求会排进 m_authQueue 去等
	// 一个永远不会到来的 DSH 认证握手（m_authenticated 在接管态恒为假）。
	// 交给扩展的只有可序列化的部分：回填靠 rpcId，两个回调留在本类的 m_pending 里。
	if (m_takenover) {
		const QJsonObject payload = body.value(QStringLiteral("payload")).toObject();
		dispatchTakeoverCall(
			body.value(QStringLiteral("rpcId")).toString(),
			body.value(QStringLiteral("method")).toString(),
			payload.value(QStringLiteral("args")).toObject(),
			std::move(onSuccess),
			std::move(onError));
		return;
	}

	// 认证还没就绪（启动早期 / 服务端刚重启）：先挂起来，等握手完成再补发；直发必然 401（0.1.5 的 /api 在认证围栏后面），用户看到的就是"发出去的消息莫名失败"。ServerManager 重启服务端、扩展安装触发重启时都会走这里。
	if (!m_authenticated && !m_launchToken.isEmpty()) {
		static constexpr int kMaxQueuedCalls = 256;
		if (m_authQueue.size() >= kMaxQueuedCalls) {
			if (onError)
				onError(RpcError{ QStringLiteral("auth"),
					qtTrId("server_auth_queue_full_fmt").arg(kMaxQueuedCalls) });
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
	// reply 用完即删，不进登记表
	connect(reply, &QNetworkReply::finished, this, &DshApiClient::onReplyFinished);
}

// 所有 HTTP 请求的 finished 统一处理：从 reply 上取回 rpcId → 在 m_pending 中找到对应回调 → 先检查 HTTP 传输层错误 → 再解析 JSON 响应体 → 按 server-response 的 result.ok 判断业务成功/失败。
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
				});
		}
		reply->deleteLater();
		return;
	}

	// 传输成功：把响应体解析挪到线程池 worker（大回包如 session/page 的历史页可达几百 KB~1MB，避免 fromJson 卡主线程），完成后回投 handleParsedResponse。
	const QByteArray body = reply->readAll();
	reply->deleteLater();

	m_parsing.insert(rpcId, pending);
	auto* task = new JsonParseRunnable(this, rpcId, body);
	QThreadPool::globalInstance()->start(task);
}

// 主线程：处理线程池解析完成的 HTTP 响应（拆 result 信封并调用回调）。
// ⚠️ **接管路径的回填不走这里**：它第一步查的是 m_parsing，而接管路径的条目只登记在
//    m_pending 里 —— 复用它只会命中下面的 "stale parsed response" 分支，静默丢掉回调。
//    （接管路径的回填入口是 CompleteCall / FailCall。）
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
				});
		}
	}
}

// ==================================================================
// 后端接管：宿主侧的开关、下发与回填
// ==================================================================
// 分工（见 VirtualClass/VirtualApiTakeover.h）：
//   插件 → 宿主：VirtualApiHost（本类实现）—— Takenover / CompleteCall / FailCall
//   宿主 → 插件：VirtualApiSink（插件根对象实现，登记在 kApiSink）—— OnOutboundRequest
// 出站请求只把**可序列化**的部分交给扩展；两个回调留在本类的 m_pending 里，
// 扩展回传结果后由本类触发原来那个回调（std::function 不是 metatype，传不出去）。

// 取当前登记为接收端的扩展根对象。每次现取而不是缓存：注册表的值是 QPointer，插件被
// unload 时自动置空；也可能被后装载的另一个扩展顶掉（多扩展并发接管是本期明确暂缓的事）。
VirtualApiSink* DshApiClient::apiSink() const
{
	QObject* const root =
		CommonRegistry::instance().FindFromRegistry(QString::fromUtf8(DshHostIndex::kApiSink)).data();
	if (!root)
		return nullptr;
	return qobject_cast<VirtualApiSink*>(root);
}

// 把一次请求交给扩展。返回 false = 没有接收端（未装载扩展 / 扩展已被卸载），调用方应当
// 当场把这条请求失败掉，而不是让它挂到超时。
bool DshApiClient::sendToSink(const QString& rpcId, const QString& method, const QJsonObject& args)
{
	VirtualApiSink* const sink = apiSink();
	if (!sink) {
		qWarning().noquote() << "[DshApi] 接管态出站失败：注册表里没有实现 VirtualApiSink 的扩展"
			" method=" << method << "rpcId=" << rpcId;
		return false;
	}

	const QByteArray rpcIdUtf8 = rpcId.toUtf8();
	const QByteArray methodUtf8 = method.toUtf8();
	const QByteArray argsUtf8 = QJsonDocument(args).toJson(QJsonDocument::Compact);

	qInfo().noquote() << "[DshApi] 接管态出站 ->" << method << "rpcId=" << rpcId;
	// ⚠️ 三个指针只在这次调用期间有效（接口契约），扩展要自己拷贝
	sink->OnOutboundRequest(rpcIdUtf8.constData(), methodUtf8.constData(), argsUtf8.constData());
	return true;
}

// 接管路径的一元 RPC 入口：入表（回调 + 超时期限）后交给扩展。
void DshApiClient::dispatchTakeoverCall(const QString& rpcId, const QString& method,
	const QJsonObject& args, std::function<void(const QJsonValue&)> onSuccess,
	std::function<void(const RpcError&)> onError)
{
	if (rpcId.isEmpty()) {
		// 理论上到不了这里（rpcId 由 callMethod / callMethodValue 生成）；真到不了就当失败处理
		qWarning().noquote() << "[DshApi] 接管态出站请求缺 rpcId，已丢弃 method=" << method;
		if (onError)
			onError(RpcError{ QStringLiteral("takenover-no-rpcid"),
				QStringLiteral("outbound request has no rpcId") });
		return;
	}

	PendingCall pending;
	// path 在接管路径上只用于日志：写成 "takeover:<方法名>"，一眼能分辨它没走过 HTTP
	pending.path = QStringLiteral("takeover:") + method;
	pending.onSuccess = std::move(onSuccess);
	pending.onError = std::move(onError);
	pending.takeover = true;
	pending.takeoverDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kTakeoverCallTimeoutMs;
	m_pending.insert(rpcId, pending);

	if (!sendToSink(rpcId, method, args)) {
		// 没有接收端：当场用错误收尾（此刻条目已在表里，failPending 会摘掉它）
		failPending(rpcId, QStringLiteral("takenover-no-sink"),
			QStringLiteral("no client extension implements VirtualApiSink"));
	}
}

// 按 rpcId 取出挂着的一条请求并用错误收尾。返回 false = 这条 rpcId 不在表里。
bool DshApiClient::failPending(const QString& rpcId, const QString& code, const QString& message)
{
	const auto it = m_pending.constFind(rpcId);
	if (it == m_pending.constEnd())
		return false;

	PendingCall pending = it.value();
	m_pending.erase(it);

	// ⚠️ 回填路径也要做析构保护：HTTP 路径上那道 m_destroyed 检查（onReplyFinished）在
	// 接管路径上不存在，而回填时机改由扩展决定 ⇒ "调用方已析构而回调才到"的概率更高。
	if (m_destroyed)
		return true;

	qWarning().noquote() << "[DshApi] 接管态请求失败 path=" << pending.path
		<< "rpcId=" << rpcId << "code=" << code << "message=" << message;
	if (pending.onError)
		pending.onError(RpcError{ code, message });
	return true;
}

// 把还挂着的接管请求全部用错误收尾（扩展交还后端时用）。HTTP 路径的条目不动。
void DshApiClient::failTakeoverPending(const QString& code, const QString& message)
{
	QStringList pendingIds;
	for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
		if (it.value().takeover)
			pendingIds.append(it.key());
	}

	for (const QString& rpcId : pendingIds)
		failPending(rpcId, code, message);
}

// 回填超时扫描。未接管路径靠 HTTP 的 transferTimeout/认证硬失败兜底；接管路径上唯一的
// 兜底就是这个 —— 扩展不回填 = 调用方永久挂着、界面停在加载中，而且没有任何报错。
void DshApiClient::sweepTakeoverTimeouts()
{
	if (!m_takenover || m_pending.isEmpty())
		return;

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	QStringList expired;
	for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
		const PendingCall& pending = it.value();
		if (pending.takeover && pending.takeoverDeadlineMs > 0 && now >= pending.takeoverDeadlineMs)
			expired.append(it.key());
	}

	for (const QString& rpcId : expired) {
		failPending(rpcId, QStringLiteral("takenover-timeout"),
			QStringLiteral("backend extension did not answer within %1 ms")
				.arg(kTakeoverCallTimeoutMs));
	}
}

// 进入接管态的一次性收尾：把"内置 DSH 那边已经开始的事"全部作废。
// ⚠️ 装配顺序决定了接管到来时内置服务端**已经起来过**（见 misc/API_TAKEOVER_PLAN.zh-CN.md
//    D8=C）：baseUrlReady 可能已经发过、setBaseUrl 已经调过、认证握手与开流可能已经开始。
//    这一段就是那批连带项的处置。
void DshApiClient::enterTakenoverState()
{
	// ① 认证握手：判废在途的那一代（握手回包按 m_authAttempt 校验，代次一对不上就被丢掉，
	//    不会再把 cookie 写回来、也不会清掉新一轮的在途标记），清掉 cookie 与队列状态。
	++m_authAttempt;
	m_authInFlight = false;
	m_authenticated = false;
	m_authCookie.clear();

	// ② 物理断开 mux：它连的正是即将被停掉的内置服务端。先置 m_streamClosing 再关，
	//    否则 disconnected 处理会把它当成"意外断线"并安排重连。
	if (m_reconnectTimer)
		m_reconnectTimer->stop();
	m_streamClosing = true;
	if (m_stream)
		m_stream->close();
	closeStreams();   // 接管态分支：只清本地账本，不碰 WebSocket

	// ③ 启动早期那批"还在等认证"的请求（post() 的 m_authQueue）：它们既等不到认证、
	//    也不会自己走进接管路径。按新后端重新下发，否则用户点的那一下就是永久挂起。
	if (!m_authQueue.isEmpty()) {
		const QList<QueuedCall> queued = m_authQueue;
		m_authQueue.clear();
		qInfo().noquote() << "[DshApi] 接管态：把" << queued.size() << "条等待认证的请求改投扩展";
		for (const QueuedCall& call : queued) {
			const QJsonObject payload = call.body.value(QStringLiteral("payload")).toObject();
			dispatchTakeoverCall(
				call.body.value(QStringLiteral("rpcId")).toString(),
				call.body.value(QStringLiteral("method")).toString(),
				payload.value(QStringLiteral("args")).toObject(),
				call.onSuccess,
				call.onError);
		}
	}

	// ④ 超时兜底开跑
	if (m_takeoverSweep)
		m_takeoverSweep->start();
}

// 交还后端的一次性收尾。⚠️ 刻意**不**重启内置 DSH 服务端：那个进程在接管时已经被停掉了，
// 而"要不要把它拉回来"是用户的决定（重启客户端，或设置里保存一次服务端设置触发 restart()）。
void DshApiClient::leaveTakenoverState()
{
	if (m_takeoverSweep)
		m_takeoverSweep->stop();

	failTakeoverPending(QStringLiteral("takenover-released"),
		QStringLiteral("backend takeover released by the extension"));

	// 回到普通状态：断线重连逻辑恢复有效（接管期间它是被主动关闭掉的）
	m_streamClosing = false;
}

void DshApiClient::Takenover(bool on)
{
	// 拒绝接管：注册表里没有实现接口二的扩展。装载期已经判定过一次（ClientExtension 只在
	// cast 成功时才登记 kApiSink），但插件可能刚被卸载、或者调用方根本不是那个扩展
	// —— 放行的话接管态下所有出站都会石沉大海，比拒绝难查得多。
	if (on && !apiSink()) {
		qWarning().noquote() << "[DshApi] 拒绝接管：没有客户端扩展实现 VirtualApiSink"
			"（kApiSink 未登记或该扩展已被卸载）";
		return;
	}

	const bool changed = (on != m_takenover);
	m_takenover = on;

	if (changed) {
		if (on)
			enterTakenoverState();
		else
			leaveTakenoverState();
	}

	// 幂等：宿主侧的动作（停内置 DSH 进程、关三条 DSH 专属旁路）本来就是幂等的，所以重复拨到
	// 同一个状态也照发一次 —— 漏发的代价远大于多发。切主题会让**新窗口**的 m_api 接管一次
	// （那个实例的 changed 为真），只有插件自己重复调用才会走到"没变也通知"这一支。
	emit takeoverChanged(on);

	qInfo().noquote() << "[DshApi] 后端接管" << (on ? "开启" : "关闭")
		<< "changed=" << changed;
}

void DshApiClient::CompleteCall(const char* rpcId, const char* resultJson)
{
	if (m_destroyed)
		return;

	const QString id = QString::fromUtf8(rpcId ? rpcId : "");
	if (id.isEmpty()) {
		qWarning().noquote() << "[DshApi] CompleteCall ignored: empty rpcId";
		return;
	}

	const auto it = m_pending.constFind(id);
	if (it == m_pending.constEnd()) {
		// 正常情形之一：回填的是一个 fire-and-forget 的 rpcId（$takeover/stream-open 那两条
		// 流控制请求本来就没入表）。所以这里只是警告，不算错误。
		qWarning().noquote() << "[DshApi] CompleteCall: 不认识的 rpcId（重复回填 / 流控制的 id）:"
			<< id;
		return;
	}

	PendingCall pending = it.value();
	m_pending.erase(it);
	if (!pending.takeover)
		qWarning().noquote() << "[DshApi] CompleteCall 命中一条非接管态的请求: path=" << pending.path;

	QJsonParseError parseError{};
	const QByteArray json = QByteArray(resultJson ? resultJson : "");
	QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
	// 走了"包一层数组"的回退就为真：取值时必须把那一层拆掉（否则裸标量会变成单元素数组）。
	bool unwrapScalar = false;
	if (parseError.error != QJsonParseError::NoError) {
		// QJsonDocument 只接受"对象或数组"作为顶层：裸标量（12 / "x" / true）在这里必然
		// 解析失败，而 callMethodValue 的成功值可以是**裸值**。包一层数组再取回唯一的元素
		// 即可 —— 顶层标量本来就不是合法 JSON 文档，所以这个回退没有歧义。
		// （resultJson 为空串也走这条：`[]` 解析成功，取回一个 undefined 值。）
		parseError = QJsonParseError{};
		const QJsonDocument wrapped =
			QJsonDocument::fromJson(QByteArray("[") + json + QByteArray("]"), &parseError);
		if (parseError.error != QJsonParseError::NoError) {
			qWarning().noquote() << "[DshApi] CompleteCall: resultJson 不是合法 JSON rpcId=" << id
				<< "error=" << parseError.errorString();
			if (pending.onError)
				pending.onError(RpcError{ QStringLiteral("takenover-bad-result"),
					parseError.errorString() });
			return;
		}
		doc = wrapped;
		unwrapScalar = true;
	}

	// 三种结果语义在这里汇合（与未接管路径逐字对应）：
	//   callMethod       —— 存进来的回调已经包了 toObject()（callMethod 里那个 lambda）
	//   callMethodValue  —— 存进来的就是裸值回调
	//   respond          —— 存进来的是收据回调（内部再 toObject()）
	// 所以本方法本身不需要区分它们是哪一种，照存进去的那个回调喂就行。
	const QJsonValue value = unwrapScalar ? doc.array().at(0)
		: doc.isArray() ? QJsonValue(doc.array())
		: doc.isObject() ? QJsonValue(doc.object())
		: QJsonValue();

	qInfo().noquote() << "[DshApi] 接管态回填 path=" << pending.path << "rpcId=" << id;
	if (pending.onSuccess)
		pending.onSuccess(value);
}

void DshApiClient::FailCall(const char* rpcId, const char* code, const char* message)
{
	if (m_destroyed)
		return;

	const QString id = QString::fromUtf8(rpcId ? rpcId : "");
	if (id.isEmpty()) {
		qWarning().noquote() << "[DshApi] FailCall ignored: empty rpcId";
		return;
	}

	QString errorCode = QString::fromUtf8(code ? code : "");
	if (errorCode.isEmpty())
		errorCode = QStringLiteral("takenover-error");

	failPending(id, errorCode, QString::fromUtf8(message ? message : ""));
}

// $events 逻辑流的一项：ready -> 记下 clientId 并宣告 connected；emit -> 单向事件（当前 UI 用不到，留好分发点）；waterfall -> 需要回执的审批/提问，翻成旧帧形状交给 UI（rpcId 位置放 eventId）。
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
}

// session/control：主机级实时控制流。帧形状（服务端 SessionControlController）：{type:"baseline", value:{queues, jobs, projections:{<sessionId>:{asOfSeq, values}}}}、{type:"projection", sessionId, key, value, seq}（某个会话的某个投影键变了）、{type:"queue"|"jobs", …}（本客户端还没有消费方）。
// baseline 里的投影是"活会话"的现算快照（registry.snapshot），之后的 projection 帧是变化推送 —— 全程服务端现算现推，客户端不读任何缓存；baseline 只覆盖当前活着的会话，只是打开看看、还没附着 Agent 的会话不在里面，那种情况由 session/follow 快照里的投影兜底（同样是服务端从日志现折叠）。
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

// session/follow：snapshot（初始记录 + cursor）/ event（实时日志事件）；记录按顺序翻成旧的 session/event 帧，复用既有渲染路径。
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

		// 快照帧还带着一份"全量折叠"的会话投影（projectionMode: all，直接从日志算）：冷会话也能立刻拿到整条日志的累计值，交给窗口侧去显示（会话统计小灰字）。
		const QJsonObject projections = frame.value(QStringLiteral("projections")).toObject();
		if (!projections.isEmpty()) {
			emit sessionProjectionsReady(sessionId,
				projections.value(QStringLiteral("asOfSeq")).toInt(),
				projections.value(QStringLiteral("values")).toObject());
		}

		// 快照记录不逐条走流式渲染：整包交给 HistoryLoader 批量播种（sessionSnapshotReady -> seedFromSnapshot），否则历史会被渲染两遍。
		emit sessionSnapshotReady(sessionId, cursor, records, hasMore);
		return;
	}

	if (type == QStringLiteral("event")) {
		const QJsonObject event = frame.value(QStringLiteral("event")).toObject();
		if (!event.isEmpty())
			emitSessionEventFrame(sessionId, event);
		return;
	}

	// 0.1.5 的实时分片：{type:'assistant-stream', frame:{type:'start'|'chunk'|'end',…}}。其中 chunk 帧的 chunk 就是 LLM 的原始增量（text-delta / reasoning-delta / block-start|end / usage），翻成既有的 assistant/chunk 事件后，渲染路径与历史回放完全共用 —— 于是"逐字输出"在实时流上就自然成立了。
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