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

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUuid>
#include <QWebSocket>

#include <QDebug>

/**
 * 构造函数。
 *
 * 初始化：
 * - QNetworkAccessManager：用于发送 HTTP POST 请求；
 * - QWebSocket m_mux：连接 /api/events.mux；
 * - QWebSocket m_host：连接 /api/events.host。
 *
 * 同时连接 WebSocket 的文本消息、错误、连接、断开信号。
 */
DshApiClient::DshApiClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_mux(new QWebSocket)
    , m_host(new QWebSocket)
{
    connect(m_mux, &QWebSocket::textMessageReceived,
            this, &DshApiClient::onMuxTextMessageReceived);
    connect(m_host, &QWebSocket::textMessageReceived,
            this, &DshApiClient::onHostTextMessageReceived);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(m_mux, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit transportError(QStringLiteral("mux"), m_mux->errorString());
        qWarning().noquote() << "[DshApi] mux error:" << m_mux->errorString();
    });
    connect(m_host, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit transportError(QStringLiteral("host"), m_host->errorString());
        qWarning().noquote() << "[DshApi] host error:" << m_host->errorString();
    });
#else
    connect(m_mux, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        emit transportError(QStringLiteral("mux"), m_mux->errorString());
        qWarning().noquote() << "[DshApi] mux error:" << m_mux->errorString();
    });
    connect(m_host, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        emit transportError(QStringLiteral("host"), m_host->errorString());
        qWarning().noquote() << "[DshApi] host error:" << m_host->errorString();
    });
#endif

    connect(m_mux, &QWebSocket::connected, this, [this] {
        m_muxConnected = true;
        qInfo().noquote() << "[DshApi] mux stream connected";
        if (m_muxConnected && m_hostConnected)
            emit connected();
    });
    connect(m_host, &QWebSocket::connected, this, [this] {
        m_hostConnected = true;
        qInfo().noquote() << "[DshApi] host stream connected";
        if (m_muxConnected && m_hostConnected)
            emit connected();
    });

    auto emitDisconnected = [this] {
        const bool wasConnected = m_muxConnected && m_hostConnected;
        m_muxConnected = false;
        m_hostConnected = false;
        if (wasConnected)
            emit disconnected();
        qInfo().noquote() << "[DshApi] DSH disconnected";
    };
    connect(m_mux, &QWebSocket::disconnected, this, emitDisconnected);
    connect(m_host, &QWebSocket::disconnected, this, emitDisconnected);
}

/**
 * 析构函数。
 * 先关闭 WebSocket 流，再释放两个 QWebSocket 对象。
 */
DshApiClient::~DshApiClient()
{
    m_destroyed = true;

    // 先断开 WebSocket 信号，避免析构过程中触发 lambda
    if (m_mux)
        disconnect(m_mux, nullptr, this, nullptr);
    if (m_host)
        disconnect(m_host, nullptr, this, nullptr);

    closeStreams();
    delete m_mux;
    delete m_host;
}

/**
 * 设置 DSH 服务基础 URL。
 * 后续所有 HTTP 和 WebSocket 请求都会基于这个地址拼接。
 */
void DshApiClient::setBaseUrl(const QUrl &url)
{
    m_baseUrl = url;
        qInfo().noquote() << "[DshApi] baseUrl set:" << m_baseUrl.toString();
}

/** 返回当前设置的基础 URL。 */
QUrl DshApiClient::baseUrl() const
{
    return m_baseUrl;
}

/**
 * 打开 mux 和 host 两条 WebSocket 事件流。
 * 如果尚未设置 baseUrl，则直接返回。
 */
void DshApiClient::openStreams()
{
    if (m_baseUrl.isEmpty()) {
        qWarning().noquote() << "[DshApi] openStreams ignored: baseUrl is empty";
        return;
    }

    // 避免重复打开/残留旧连接
    closeStreams();

    m_mux->open(makeUrl(QStringLiteral("/api/events.mux")));
    m_host->open(makeUrl(QStringLiteral("/api/events.host")));
    qInfo().noquote() << "[DshApi] opening WebSocket streams:" << m_baseUrl.toString();
}

/**
 * 关闭两条 WebSocket 流，并重置连接状态标记。
 */
void DshApiClient::closeStreams()
{
    qInfo().noquote() << "[DshApi] closing WebSocket streams";
    if (m_mux)
        m_mux->close();
    if (m_host)
        m_host->close();
    m_muxConnected = false;
    m_hostConnected = false;
}

/**
 * 返回当前是否已连接 DSH。
 * 需要 mux 和 host 两条流都处于已连接状态。
 */
bool DshApiClient::isConnected() const
{
    return m_muxConnected && m_hostConnected;
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
    const QString &method,
    const QJsonObject &payload,
    std::function<void(const QJsonObject &value)> onSuccess,
    std::function<void(const RpcError &error)> onError)
{
    const QString rpcId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QJsonObject body;
    body.insert(QStringLiteral("type"), QStringLiteral("client-request"));
    body.insert(QStringLiteral("rpcId"), rpcId);
    body.insert(QStringLiteral("method"), method);
    body.insert(QStringLiteral("payload"), payload);

    qInfo().noquote() << "[DshApi] RPC ->" << method << " rpcId=" << rpcId;

    post(QStringLiteral("/api/") + method, body, std::move(onSuccess), std::move(onError));
}

/**
 * 应答审批/提问请求。
 *
 * 请求体：
 * {
 *   "type": "client-response",
 *   "rpcId": "<服务端帧里的 rpcId>",
 *   "result": { "ok": true, "value": { ... } }
 * }
 *
 * POST 到 /api/respond。
 */
void DshApiClient::respond(
    const QString &rpcId,
    const QJsonObject &value,
    std::function<void(const QJsonObject &receipt)> onSuccess,
    std::function<void(const RpcError &error)> onError)
{
    QJsonObject result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("value"), value);

    QJsonObject body;
    body.insert(QStringLiteral("type"), QStringLiteral("client-response"));
    body.insert(QStringLiteral("rpcId"), rpcId);
    body.insert(QStringLiteral("result"), result);

    qInfo().noquote() << "[DshApi] respond rpcId=" << rpcId;

    post(QStringLiteral("/api/respond"), body, std::move(onSuccess), std::move(onError));
}

/**
 * 把 HTTP 基础 URL 转换为对应的 WebSocket URL。
 *
 * 例如：
 *   http://127.0.0.1:3080  ->  ws://127.0.0.1:3080/api/events.mux
 *   https://example.com     ->  wss://example.com/api/events.mux
 */
QUrl DshApiClient::makeUrl(const QString &path) const
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
    const QString &path,
    const QJsonObject &body,
    std::function<void(const QJsonObject &value)> onSuccess,
    std::function<void(const RpcError &error)> onError)
{
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

    QNetworkReply *reply = m_nam->post(
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
 * 5. 如果是 /api/respond，按 accepted 字段判断；
 * 6. 其他接口按 result.ok 判断业务成功/失败。
 */
void DshApiClient::onReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
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

    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();

    qInfo().noquote() << "[DshApi] HTTP response path=" << pending.path << " rpcId=" << rpcId;

    // /api/respond returns the carrier receipt directly, not a server-response
    // envelope. Success looks like { "accepted": true }; failure looks like
    // { "accepted": false, "reason": "not-pending" | "bad-response" }.
    if (pending.path == QStringLiteral("/api/respond")) {
        if (root.value(QStringLiteral("accepted")).toBool()) {
            if (pending.onSuccess)
                pending.onSuccess(root);
        } else if (pending.onError) {
            pending.onError(RpcError{
                QStringLiteral("respond-rejected"),
                root.value(QStringLiteral("reason")).toString(),
                QJsonObject(),
            });
        }
        reply->deleteLater();
        return;
    }

    const QJsonObject result = root.value(QStringLiteral("result")).toObject();
    if (result.value(QStringLiteral("ok")).toBool()) {
        if (pending.onSuccess)
            pending.onSuccess(result.value(QStringLiteral("value")).toObject());
    } else {
        const QJsonObject errorObj = result.value(QStringLiteral("error")).toObject();
        if (pending.onError) {
            pending.onError(RpcError{
                errorObj.value(QStringLiteral("code")).toString(),
                errorObj.value(QStringLiteral("message")).toString(),
                errorObj.value(QStringLiteral("details")).toObject(),
            });
        }
    }

    reply->deleteLater();
}

/**
 * mux WebSocket 收到文本消息。
 * 把 JSON 解析成 QJsonObject 后通过 muxFrameReceived 信号发出。
 */
void DshApiClient::onMuxTextMessageReceived(const QString &message)
{
    const QJsonObject frame = QJsonDocument::fromJson(message.toUtf8()).object();
    if (frame.isEmpty()) {
        qWarning().noquote() << "[DshApi] empty frame received";
        return;
    }
    if (!frame.isEmpty())
        emit muxFrameReceived(frame);
}

/**
 * host WebSocket 收到文本消息。
 * 把 JSON 解析成 QJsonObject 后通过 hostFrameReceived 信号发出。
 */
void DshApiClient::onHostTextMessageReceived(const QString &message)
{
    const QJsonObject frame = QJsonDocument::fromJson(message.toUtf8()).object();
    if (frame.isEmpty()) {
        qWarning().noquote() << "[DshApi] empty frame received";
        return;
    }
    if (!frame.isEmpty())
        emit hostFrameReceived(frame);
}
