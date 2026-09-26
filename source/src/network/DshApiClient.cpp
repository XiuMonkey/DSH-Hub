// HTTP 请求 + mux WebSocket：生成 rpcId、拆信封后回调成败

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

DshApiClient::JsonParseRunnable::JsonParseRunnable(DshApiClient* client, QString rpcId, QByteArray body)
	: m_client(client)
	, m_rpcId(std::move(rpcId))
	, m_body(std::move(body))
{
}

// 只做纯 JSON 解析、不碰回调，完成后回投主线程
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

// 只有 /api/remote.mux 一条 mux，各逻辑流都跑在它上面
DshApiClient::DshApiClient(QObject* parent)
	: QObject(parent)
	, m_nam(new QNetworkAccessManager(this))
	, m_stream(new QWebSocket)
{
	dshRegister("DshApiClient.001", m_stream, &QWebSocket::textMessageReceived,
		this, &DshApiClient::onStreamTextMessage);
	dshRegister("DshApiClient.002", m_stream, &QWebSocket::errorOccurred, this,
		[this](QAbstractSocket::SocketError) {
			emit transportError(QStringLiteral("stream"), m_stream->errorString());
			qWarning().noquote() << "[DshApi] stream error:" << m_stream->errorString();
		});

	dshRegister("DshApiClient.003", m_stream, &QWebSocket::connected, this, [this] {
		m_streamConnected = true;
		m_reconnectDelayMs = 1000; // 连上即复位退避
		qInfo().noquote() << "[DshApi] mux connected -> opening $events + workspace/follow + session/control";
		m_eventsStreamId = nextStreamId(QStringLiteral("events"));
		m_workspaceStreamId = nextStreamId(QStringLiteral("workspace"));
		sendStreamOpen(QStringLiteral("$events"), m_eventsStreamId);
		sendStreamOpen(QStringLiteral("workspace/follow"), m_workspaceStreamId);
		m_controlStreamId = nextStreamId(QStringLiteral("control"));
		sendStreamOpen(QStringLiteral("session/control"), m_controlStreamId);
		if (!m_followedSessionId.isEmpty()) {
			const QString sessionId = m_followedSessionId;
			m_followedSessionId.clear();
			m_sessionStreamId.clear();
			followSession(sessionId);
		}
	});
	dshRegister("DshApiClient.004", m_stream, &QWebSocket::disconnected, this, [this] {
		const int code = static_cast<int>(m_stream->closeCode());
		const QString reason = m_stream->closeReason();
		const bool wasConnected = m_streamConnected;
		m_streamConnected = false;
		m_eventsReady = false;
		qInfo().noquote() << "[DshApi] DSH stream disconnected closeCode=" << code
			<< "reason=" << reason << "wasConnected=" << wasConnected;
		// 必须自愈：服务端遇重复 streamId 会 close(1008) 关掉整条 mux
		if (wasConnected)
			scheduleReconnect();
	});

	// 接管态回填超时扫描：扩展不回填时唯一能让调用方脱身的机制
	m_takeoverSweep = new QTimer(this);
	m_takeoverSweep->setInterval(5000);
	dshRegister("DshApiClient.006", m_takeoverSweep, &QTimer::timeout, this, &DshApiClient::sweepTakeoverTimeouts);
}

DshApiClient::~DshApiClient()
{
	m_destroyed = true;
	if (m_stream)
		disconnect(m_stream, nullptr, this, nullptr);
	closeStreams();
	delete m_stream;
}

// 服务端给的是带 ?token= 的认证 URL；令牌单独存，基础 URL 保持干净
void DshApiClient::setBaseUrl(const QUrl& url)
{
	// 接管态忽略它：否则仍会做 token→cookie 握手、连真 DSH
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
	// 接管态下恒为空：那条 DSH 已经不在了，回显只会误导用户
	if (m_takenover)
		return QUrl();
	return m_baseUrl;
}

// 用启动令牌换 cookie；必须禁止自动跟随重定向，否则拿不到 Set-Cookie
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
	// 握手代次：对不上的迟到回包直接丢，否则会覆盖新 cookie
	const quint64 attempt = ++m_authAttempt;

	QUrl url = m_baseUrl;
	url.setPath(QStringLiteral("/"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("token"), m_launchToken);
	url.setQuery(query);

	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		QVariant::fromValue(QNetworkRequest::ManualRedirectPolicy));
	// 必须有超时：否则 m_authInFlight 永远为真、后续重试全被挡掉
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

			// HTTP 0 = 传输层没连上，服务端重启期间属预期：只记日志 + 重连
			if (status == 0) {
				qInfo().noquote() << "[DshApi] auth handshake 瞬态失败（服务端多半在重启），稍后重试";
				scheduleReconnect();
				return;
			}

			failAuthQueue(QStringLiteral("auth"), qtTrId("server_auth_failed_fmt").arg(status));
			emit transportError(QStringLiteral("auth"), qtTrId("server_auth_failed_fmt").arg(status));
			return;
		}

		int end = header.indexOf(';');
		if (end < 0)
			end = header.size();
		m_authCookie = QString::fromUtf8(header.left(end)).trimmed();
		m_authenticated = true;
		qInfo().noquote() << "[DshApi] auth cookie acquired (" << m_authCookie.size() << "chars )";
		// 认证就绪：把等待中的 RPC 按顺序补发
		flushAuthQueue();
		openStreams();
		});
}

// 补发走的还是 post()，此时已认证，不会再入队
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

// 带令牌但没换到 cookie 时先握手（成功后回到这里）
void DshApiClient::openStreams()
{
	// 接管态流归扩展负责，宿主不开 mux
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

	closeStreams();
	QNetworkRequest streamRequest(makeUrl(QStringLiteral("/api/remote.mux")));
	if (!m_authCookie.isEmpty())
		streamRequest.setRawHeader("Cookie", m_authCookie.toUtf8());
	// 清掉"正在关闭"标记，重连逻辑才继续有效
	m_streamClosing = false;
	m_stream->open(streamRequest);
	qInfo().noquote() << "[DshApi] opening mux stream:" << m_baseUrl.toString();
}

// 流 id 每次 open 必须唯一：重复 streamId 会让服务端 close(1008)
QString DshApiClient::nextStreamId(const QString& prefix)
{
	++m_streamSeq;
	return QStringLiteral("%1-%2").arg(prefix).arg(m_streamSeq);
}

void DshApiClient::scheduleReconnect()
{
	if (!m_reconnectTimer) {
		m_reconnectTimer = new QTimer(this);
		m_reconnectTimer->setSingleShot(true);
		dshRegister("DshApiClient.005", m_reconnectTimer, &QTimer::timeout, this,
			[this] {
				if (m_destroyed || m_baseUrl.isEmpty() || m_streamClosing)
					return;
				qInfo().noquote() << "[DshApi] reconnecting mux…";
				// cookie 可能随连接代次失效，重连时重换一次更稳
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
	// 接管态不碰 WebSocket、不通知扩展：本路径会被析构调用
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

// open 帧：type / streamId / endpoint / payload.args
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

void DshApiClient::sendStreamCancel(const QString& streamId)
{
	if (!m_streamConnected || !m_stream || streamId.isEmpty())
		return;

	QJsonObject frame;
	frame.insert(QStringLiteral("type"), QStringLiteral("cancel"));
	frame.insert(QStringLiteral("streamId"), streamId);
	m_stream->sendTextMessage(QString::fromUtf8(QJsonDocument(frame).toJson(QJsonDocument::Compact)));
}

// 参数形状：request.address = {kind, sessionId}
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
	// 不带它 follow 只发已提交的整段消息；带上才有实时分片
	request.insert(QStringLiteral("assistantStream"), true);
	QJsonObject args;
	args.insert(QStringLiteral("request"), request);

	m_followedSessionId = sessionId;

	// 接管态不开真流，只把"要看哪个会话"告诉扩展（载荷同 open 帧）
	if (m_takenover) {
		QJsonObject frame;
		frame.insert(QStringLiteral("endpoint"), QStringLiteral("session/follow"));
		frame.insert(QStringLiteral("args"), args);
		const QString rpcId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		// fire-and-forget：不入 m_pending、不等回填
		sendToSink(rpcId, QStringLiteral("$takeover/stream-open"), frame);
		return;
	}

	m_sessionStreamId = nextStreamId(QStringLiteral("session"));
	sendStreamOpen(QStringLiteral("session/follow"), m_sessionStreamId, args);
}

void DshApiClient::unfollowSession()
{
	// 接管态：通知扩展收掉 follow
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

// = mux 已打开且 $events 已收到 ready 帧
bool DshApiClient::isConnected() const
{
	// 接管态恒 true：传输归扩展，宿主没有"没连上"的判据
	if (m_takenover)
		return true;
	return m_streamConnected && m_eventsReady;
}

// POST /api/<method>；接管态的分流在 post() 里
void DshApiClient::callMethod(const QString& method, const QJsonObject& payload,
	std::function<void(const QJsonObject& value)> onSuccess,
	std::function<void(const RpcError& error)> onError)
{
	const QString rpcId = QUuid::createUuid().toString(QUuid::WithoutBraces);

	// payload 必须恰好只有一个 args 对象
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

// 同 callMethod，但成功回调拿裸 JSON 值
void DshApiClient::callMethodValue(const QString& method, const QJsonObject& payload,
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

// 通道 POST /api/$events/result；rpcId 就是帧里的 eventId
void DshApiClient::respond(const QString& rpcId, const QJsonObject& value,
	std::function<void(const QJsonObject& receipt)> onSuccess,
	std::function<void(const RpcError& error)> onError)
{
	// 接管态必须绕过：接管后 m_clientId 恒空，应答会被吞掉
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
	// 接管态 m_clientId 为空：干脆不带 clientId，而不是塞空串
	if (!m_clientId.isEmpty())
		args.insert(QStringLiteral("clientId"), m_clientId);
	args.insert(QStringLiteral("eventId"), rpcId);
	args.insert(QStringLiteral("outcome"), outcome);

	qInfo().noquote() << "[DshApi] respond -> $events/result eventId=" << rpcId;
	callMethod(QStringLiteral("$events/result"), args, onSuccess, onError);
}

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

// 回调存进 m_pending 供回包匹配，rpcId 记在 reply 上
void DshApiClient::post(const QString& path, const QJsonObject& body,
	std::function<void(const QJsonValue& value)> onSuccess,
	std::function<void(const RpcError& error)> onError)
{
	// 接管态分流写在这里：callMethod / callMethodValue / respond 都汇到本函数
	// 位置必须在认证队列之前，否则会排进 m_authQueue 等永不来的握手
	if (m_takenover) {
		const QJsonObject payload = body.value(QStringLiteral("payload")).toObject();
		dispatchTakeoverCall(body.value(QStringLiteral("rpcId")).toString(),
			body.value(QStringLiteral("method")).toString(),
			payload.value(QStringLiteral("args")).toObject(),
			std::move(onSuccess), std::move(onError));
		return;
	}

	// 认证没就绪时直发必然 401，先挂起等握手完成后补发
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
	m_pending.insert(rpcId, PendingCall{ path, std::move(onSuccess), std::move(onError), });

	QUrl url = m_baseUrl;
	url.setPath(path);
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	// /api 需要认证 cookie，缺失时服务端直接 401
	if (!m_authCookie.isEmpty())
		request.setRawHeader("Cookie", m_authCookie.toUtf8());
	QNetworkReply* reply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
	reply->setProperty("rpcId", rpcId);
	connect(reply, &QNetworkReply::finished, this, &DshApiClient::onReplyFinished);
}

void DshApiClient::onReplyFinished()
{
	QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
	if (!reply)
		return;

	// 析构期间忽略回调，避免调用已析构的接收方
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
		qWarning().noquote() << "[DshApi] HTTP transport error path=" << pending.path
			<< " rpcId=" << rpcId << " error=" << reply->errorString();
		if (pending.onError) {
			pending.onError(RpcError{ QStringLiteral("transport"), reply->errorString(), });
		}
		reply->deleteLater();
		return;
	}

	// 解析挪到线程池：大回包 fromJson 会卡主线程
	const QByteArray body = reply->readAll();
	reply->deleteLater();

	m_parsing.insert(rpcId, pending);
	auto* task = new JsonParseRunnable(this, rpcId, body);
	QThreadPool::globalInstance()->start(task);
}

// 主线程：拆 result 信封并调回调。
// 接管路径的回填不走这里（它只登记在 m_pending），复用会静默丢回调
void DshApiClient::handleParsedResponse(const QString& rpcId, const QJsonDocument& doc)
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
			pending.onError(RpcError{ errorObj.value(QStringLiteral("code")).toString(),
				errorObj.value(QStringLiteral("message")).toString(), });
		}
	}
}

// 分工：插件 → 宿主走本类实现的 VirtualApiHost；宿主 → 插件走 VirtualApiSink::OnOutboundRequest，
// 回填只能靠 rpcId（std::function 不是 metatype，传不出去）

// 现取不缓存：注册表的值是 QPointer，插件 unload 时会自动置空
VirtualApiSink* DshApiClient::apiSink() const
{
	QObject* const root = CommonRegistry::instance().FindFromRegistry(QString::fromUtf8(DshHostIndex::kApiSink)).data();
	if (!root)
		return nullptr;
	return qobject_cast<VirtualApiSink*>(root);
}

// 返回 false = 没有接收端，调用方应当场失败掉这条请求
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
	// 三个指针只在本次调用期间有效，扩展要自己拷贝
	sink->OnOutboundRequest(rpcIdUtf8.constData(), methodUtf8.constData(), argsUtf8.constData());
	return true;
}

void DshApiClient::dispatchTakeoverCall(const QString& rpcId, const QString& method,
	const QJsonObject& args, std::function<void(const QJsonValue&)> onSuccess,
	std::function<void(const RpcError&)> onError)
{
	if (rpcId.isEmpty()) {
		qWarning().noquote() << "[DshApi] 接管态出站请求缺 rpcId，已丢弃 method=" << method;
		if (onError)
			onError(RpcError{ QStringLiteral("takenover-no-rpcid"),
				QStringLiteral("outbound request has no rpcId") });
		return;
	}

	PendingCall pending;
	// path 只用于日志，标明它没走过 HTTP
	pending.path = QStringLiteral("takeover:") + method;
	pending.onSuccess = std::move(onSuccess);
	pending.onError = std::move(onError);
	pending.takeover = true;
	pending.takeoverDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kTakeoverCallTimeoutMs;
	m_pending.insert(rpcId, pending);

	if (!sendToSink(rpcId, method, args)) {
		// 没有接收端：当场用错误收尾
		failPending(rpcId, QStringLiteral("takenover-no-sink"),
			QStringLiteral("no client extension implements VirtualApiSink"));
	}
}

bool DshApiClient::failPending(const QString& rpcId, const QString& code, const QString& message)
{
	const auto it = m_pending.constFind(rpcId);
	if (it == m_pending.constEnd())
		return false;

	PendingCall pending = it.value();
	m_pending.erase(it);

	// 接管路径没有 HTTP 那道 m_destroyed 检查，更可能"已析构而回调才到"
	if (m_destroyed)
		return true;

	qWarning().noquote() << "[DshApi] 接管态请求失败 path=" << pending.path
		<< "rpcId=" << rpcId << "code=" << code << "message=" << message;
	if (pending.onError)
		pending.onError(RpcError{ code, message });
	return true;
}

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

// 接管路径唯一的兜底：扩展不回填 = 调用方永久挂着且无任何报错
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

// 进入接管态的一次性收尾：把内置 DSH 那边已经开始的事全部作废。
// 接管到来时内置服务端已起来过：baseUrlReady / 认证握手 / 开流可能都已发生
void DshApiClient::enterTakenoverState()
{
	// ① 认证握手：判废在途那一代，清掉 cookie 与在途标记
	++m_authAttempt;
	m_authInFlight = false;
	m_authenticated = false;
	m_authCookie.clear();

	// ② 先置 m_streamClosing 再关 mux，否则会被当成意外断线并重连
	if (m_reconnectTimer)
		m_reconnectTimer->stop();
	m_streamClosing = true;
	if (m_stream)
		m_stream->close();
	closeStreams();  // 只清本地账本

	// ③ 还在等认证的请求不会走接管路径，按新后端重新下发
	if (!m_authQueue.isEmpty()) {
		const QList<QueuedCall> queued = m_authQueue;
		m_authQueue.clear();
		qInfo().noquote() << "[DshApi] 接管态：把" << queued.size() << "条等待认证的请求改投扩展";
		for (const QueuedCall& call : queued) {
			const QJsonObject payload = call.body.value(QStringLiteral("payload")).toObject();
			dispatchTakeoverCall(call.body.value(QStringLiteral("rpcId")).toString(),
				call.body.value(QStringLiteral("method")).toString(),
				payload.value(QStringLiteral("args")).toObject(),
				call.onSuccess, call.onError);
		}
	}

	// ④ 超时扫断开跑
	if (m_takeoverSweep)
		m_takeoverSweep->start();
}

// 刻意不重启内置 DSH：要不要拉回来由用户决定
void DshApiClient::leaveTakenoverState()
{
	if (m_takeoverSweep)
		m_takeoverSweep->stop();

	failTakeoverPending(QStringLiteral("takenover-released"),
		QStringLiteral("backend takeover released by the extension"));

	m_streamClosing = false;
}

// waterfall 是需要回执的审批/提问，翻成旧帧交给 UI
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
		// 帧上的 agentId 就是 SessionId，请求体没带时补上
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
		QStringList workspaceIds;
		for (const auto& value : frame.value(QStringLiteral("workspaceIds")).toArray())
			workspaceIds.append(value.toString());
		emit workspaceReordered(workspaceIds);
		return;
	}
}

// baseline 是活会话的现算投影快照，之后靠 projection 帧推送；冷会话由 follow 快照兜底
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
}

// 记录翻成旧的 session/event 帧，复用既有渲染路径
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

		// 快照还带一份全量投影（projectionMode: all）
		const QJsonObject projections = frame.value(QStringLiteral("projections")).toObject();
		if (!projections.isEmpty()) {
			emit sessionProjectionsReady(sessionId,
				projections.value(QStringLiteral("asOfSeq")).toInt(),
				projections.value(QStringLiteral("values")).toObject());
		}

		// 整包交给 HistoryLoader 批量播种，否则历史会被渲染两遍
		emit sessionSnapshotReady(sessionId, cursor, records, hasMore);
		return;
	}

	if (type == QStringLiteral("event")) {
		const QJsonObject event = frame.value(QStringLiteral("event")).toObject();
		if (!event.isEmpty())
			emitSessionEventFrame(sessionId, event);
		return;
	}

	// chunk 是 LLM 原始增量，翻成 assistant/chunk 后与历史回放共用渲染路径
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
