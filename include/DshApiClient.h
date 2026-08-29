#pragma once

// ------------------------------------------------------------------
// DshApiClient.h
// ------------------------------------------------------------------
// DSH（DeepSeek Harness）本地 HTTP/WebSocket API 的轻量级 Qt 客户端。
//
// 主要功能：
//   - 通过 HTTP POST /api/<method> 调用 DSH 的一元 RPC；
//   - 通过 WebSocket 连接 /api/events.mux 和 /api/events.host；
//   - 接收服务端推送的会话事件、审批请求、Host 事件；
//   - 支持通过 /api/respond 应答审批/提问。
// ------------------------------------------------------------------

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
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
 * connect(&api, &DshApiClient::hostFrameReceived, ...);
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
		QJsonObject details; // 附加错误详情
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
	 * 打开两条服务端推送 WebSocket 流：
	 *   /api/events.mux
	 *   /api/events.host
	 * 两条流都连接成功后会发出 connected() 信号。
	 */
	void openStreams();

	/**
	 * 关闭 WebSocket 流。
	 * 同时会清空 mux/host 的连接状态标记。
	 */
	void closeStreams();

	/**
	 * 返回当前是否已连接 DSH。
	 * 只有当 mux 和 host 两条 WebSocket 流都连接成功时才返回 true。
	 */
	bool isConnected() const;

	/**
	 * 发送一个一元 RPC 请求。
	 *
	 * 例如：
	 *   callMethod("session.list", {});
	 *   callMethod("session.prompt", payload);
	 *
	 * 客户端会自动生成 rpcId。
	 *
	 * @param method    DSH 方法名，例如 "host.describe"、"workspace.list"、"session.prompt"
	 * @param payload   方法参数对象
	 * @param onSuccess 成功后回调，参数是 result.value
	 * @param onError   失败后回调，参数是 RpcError
	 */
	void callMethod(
		const QString& method,
		const QJsonObject& payload = {},
		std::function<void(const QJsonObject& value)> onSuccess = {},
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
	/** 两条 WebSocket 流都连接成功后发出。 */
	void connected();

	/** 任意一条 WebSocket 流断开后发出。 */
	void disconnected();

	/**
	 * 收到 mux 流的一整帧。
	 * frame["payload"] 是 MuxFrame；
	 * frame["rpcId"] 用于应答审批/提问。
	 */
	void muxFrameReceived(const QJsonObject& frame);

	/** 收到 host 流的一整帧。 */
	void hostFrameReceived(const QJsonObject& frame);

	/**
	 * 传输层错误信号。
	 * @param context 出错来源，例如 "mux"、"host" 或 HTTP 请求
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
		QString path;       // 请求的 API 路径，例如 /api/session.list
		std::function<void(const QJsonObject& value)> onSuccess;
		std::function<void(const RpcError& error)> onError;
	};

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
		std::function<void(const QJsonObject& value)> onSuccess,
		std::function<void(const RpcError& error)> onError);

private slots:
	/** QNetworkReply::finished 的统一处理槽。 */
	void onReplyFinished();
	/** mux WebSocket 收到文本消息。 */
	void onMuxTextMessageReceived(const QString& message);
	/** host WebSocket 收到文本消息。 */
	void onHostTextMessageReceived(const QString& message);

private:
	QNetworkAccessManager* m_nam = nullptr; // 用于发送 HTTP 请求
	QWebSocket* m_mux = nullptr;            // mux 事件流
	QWebSocket* m_host = nullptr;           // host 事件流
	QUrl m_baseUrl;                         // DSH 服务基础地址
	QHash<QString, PendingCall> m_pending;  // rpcId -> 待处理请求
	bool m_muxConnected = false;            // mux 是否已连接
	bool m_hostConnected = false;           // host 是否已连接
	bool m_destroyed = false;               // 正在析构，忽略后续回调
};
