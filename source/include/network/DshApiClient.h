#pragma once

// DSH（DeepSeek Harness）本地 API 的 Qt 客户端（对齐 dsh 0.1.5）：一元 RPC + 认证围栏 + 单条 mux WebSocket 上的逻辑流。
// 陷阱：/api 在浏览器认证栅栏之后（cookie 只能由 GET /?token=<启动令牌> 换到，否则一律 401）；
// 逻辑流 id 必须唯一，重复会让服务端 close(1008) 关掉整条 mux。

#include <QByteArray>
#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QObject>
#include <QPointer>
#include <QRunnable>
#include <QUrl>
#include <functional>

// 接管的两半接口：本类实现 VirtualApiHost（插件调宿主），扩展实现 VirtualApiSink。
// ⚠️ 插件侧只许经 VirtualApiHost 虚函数访问本类，别引用成员（宿主 exe 符号零导出，直接引用即 LNK2019）。
#include "VirtualClass/VirtualApiTakeover.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QWebSocket;

// DSH API 客户端；用法：setBaseUrl() → openStreams() → callMethod() / 监听 muxFrameReceived。
// 接管态下不走 HTTP，出站请求改交扩展。
class DshApiClient : public QObject, public VirtualApiHost
{
	Q_OBJECT
	// ⚠️ 不能漏：qobject_cast<VirtualApiHost*> 只认这里列出的 IID，漏了就永远 cast 出 nullptr 且不报错
	Q_INTERFACES(VirtualApiHost)

public:
	// HTTP 成功但 result.ok == false 时使用
	struct RpcError
	{
		QString code;
		QString message;
	};

	explicit DshApiClient(QObject* parent = nullptr);
	~DshApiClient() override;

	// 设置基础 URL 并启动认证握手；接管态下忽略：那条地址指向即将被停掉的内置服务端
	void setBaseUrl(const QUrl& url);
	// 当前基础 URL；接管态下恒为空（那个 DSH 已经不在了）
	QUrl baseUrl() const;
	// 启动令牌；重建主窗口（切主题）时必须一起带过去，否则新窗口换不到 cookie。接管态下恒为空
	QString launchToken() const { return m_takenover ? QString() : m_launchToken; }

	// 打开 mux 上的逻辑流；接管态下忽略（流由扩展负责）
	void openStreams();
	// 关闭流通道（WebSocket + 全部逻辑流）；接管态下只清本地账本、不回调扩展（析构期回调不安全）
	void closeStreams();
	// 跟随一个会话（mux 上开 session/follow，换会话自动换流）；接管态下改通知扩展
	void followSession(const QString& sessionId);
	// 关闭 session/follow 流；接管态下通知扩展
	void unfollowSession();
	// mux 已打开且 $events 收到 ready 帧；接管态下恒为 true（传输已归扩展）
	bool isConnected() const;
	// 后端是否处于接管态（原始上报/提问应答的分流、DSHHub 的旁路开关都看它）
	bool isTakenover() const { return m_takenover; }

	// VirtualApiHost 接口（插件调用宿主，插件 → 宿主）：方法实现全内联在本头文件
	// 幂等：拿不到扩展接收端时拒绝接管并记警告
	void Takenover(bool on) override
	{
		// 放行的话接管态下所有出站都会石沉大海
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

		// 宿主侧动作本就幂等，重复拨同一状态也照发一次
		emit takeoverChanged(on);

		qInfo().noquote() << "[DshApi] 后端接管" << (on ? "开启" : "关闭")
			<< "changed=" << changed;
	}

	// 扩展回填成功结果（resultJson = 成功回调该拿到的 value）
	void CompleteCall(const char* rpcId, const char* resultJson) override
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
			// 正常情形之一：回填的是 fire-and-forget 的 rpcId，只是警告
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
		// 包了数组的回退（取值时要拆掉那一层）
		bool unwrapScalar = false;
		if (parseError.error != QJsonParseError::NoError) {
			// 顶层只接受对象/数组，而成功值可以是裸标量：包一层数组再取回唯一元素
			parseError = QJsonParseError{};
			const QJsonDocument wrapped = QJsonDocument::fromJson(QByteArray("[") + json + QByteArray("]"), &parseError);
			if (parseError.error != QJsonParseError::NoError) {
				qWarning().noquote() << "[DshApi] CompleteCall: resultJson 不是合法 JSON rpcId=" << id
					<< "error=" << parseError.errorString();
			if (pending.onError)
				pending.onError(RpcError{ QStringLiteral("takenover-bad-result"), parseError.errorString() });
			return;
			}
			doc = wrapped;
			unwrapScalar = true;
		}

		// 不区分三种回调语义，照存进去的那个喂
		const QJsonValue value = unwrapScalar ? doc.array().at(0)
			: doc.isArray() ? QJsonValue(doc.array())
			: doc.isObject() ? QJsonValue(doc.object())
			: QJsonValue();

		qInfo().noquote() << "[DshApi] 接管态回填 path=" << pending.path << "rpcId=" << id;
		if (pending.onSuccess)
			pending.onSuccess(value);
	}

	// 扩展回填失败（code / message 原样进 RpcError）
	void FailCall(const char* rpcId, const char* code, const char* message) override
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

	// 接管态查询：Takenover(bool) 无回执，插件靠它确认请求有没有被接受（即 isTakenover 的接口版）
	bool IsTakenover() const override
	{
		return isTakenover();
	}

	// 一元 RPC：payload 是本端点的 args 内容（本类包一层 {"args": …}）；endpoint 用斜杠（"session/list"），
	// 返回裸数组的端点改用 callMethodValue
	void callMethod(const QString& method, const QJsonObject& payload = {}, std::function<void(const QJsonObject& value)> onSuccess = {}, std::function<void(const RpcError& error)> onError = {});

	// 同 callMethod，但成功回调拿裸 JSON 值
	void callMethodValue(const QString& method, const QJsonObject& payload, std::function<void(const QJsonValue& value)> onSuccess, std::function<void(const RpcError& error)> onError = {});

	// 应答 mux 上的审批/提问帧：rpcId 用帧里的，value 形如 {sessionId, approvalId, outcome}
	void respond(const QString& rpcId, const QJsonObject& value, std::function<void(const QJsonObject& receipt)> onSuccess = {}, std::function<void(const RpcError& error)> onError = {});

signals:
	// ⚠️ 宿主内部信号：DSHHub 靠它停掉内置 DSH 进程、关掉三条 DSH 专属旁路；插件 ↔ 宿主不走信号
	void takeoverChanged(bool takenover);
	void connected();
	// 兼容旧消费者：把 0.1.5 的新帧翻译成旧形状再发出
	void muxFrameReceived(const QJsonObject& frame);
	void workspaceSnapshotReady(const QJsonArray& items, const QJsonArray& archivedSessionIds);
	// workspace/follow 的四类增量帧：order 与 archived 是整体替换
	void workspaceUpserted(const QJsonObject& workspace);
	void workspaceRemoved(const QString& workspaceId);
	void workspaceReordered(const QStringList& workspaceIds);
	void workspaceArchiveChanged(const QJsonArray& archivedSessionIds);
	void sessionSnapshotReady(const QString& sessionId, int cursor, const QJsonArray& records, bool hasMore);
	// 快照帧带的投影由服务端全量折叠现算，没被 Agent 附着的会话也准
	void sessionProjectionsReady(const QString& sessionId, int asOfSeq, const QJsonObject& values);
	void sessionProjectionsBaselineReady(const QJsonObject& projectionsBySession);
	// 现算现推，不读缓存
	void sessionProjectionChanged(const QString& sessionId, const QString& key, const QJsonValue& value, int seq);
	void transportError(const QString& context, const QString& message);

private:
	struct PendingCall
	{
		QString path;       // 接管路径记 "takeover:<方法名>"
		std::function<void(const QJsonValue& value)> onSuccess;
		std::function<void(const RpcError& error)> onError;
		// 以下两项只有接管路径会置位
		bool takeover = false;
		qint64 takeoverDeadlineMs = 0;                // 0 = 不检查
	};

	// 认证未就绪时挂起：服务端刚重启那一两秒直发必然 401
	struct QueuedCall
	{
		QString path;
		QJsonObject body;
		std::function<void(const QJsonValue&)> onSuccess;
		std::function<void(const RpcError&)> onError;
	};

	// 线程池里解析响应体（大 JSON 不在主线程 fromJson），完成后经 queued invokeMethod 回投主线程
	class JsonParseRunnable : public QRunnable
	{
	public:
		JsonParseRunnable(DshApiClient* client, QString rpcId, QByteArray body);
		void run() override;

	private:
		QPointer<DshApiClient> m_client;
		QString m_rpcId;
		QByteArray m_body;
	};

	// ⚠️ 接管路径的回填不走这里：它查的是 m_parsing，而接管条目只登记在 m_pending —— 复用它只会命中
	// "stale parsed response" 分支，静默丢掉回调
	void handleParsedResponse(const QString& rpcId, const QJsonDocument& doc);

	// 接管态出站请求的回填时限：扩展不回填时不能让调用方永久挂起（未接管路径有 failAuthQueue 兜底）
	static constexpr qint64 kTakeoverCallTimeoutMs = 120000;

	// 取登记为接收端的扩展根对象（kApiSink）；每次现取，插件可能已被卸载或被顶掉
	VirtualApiSink* apiSink() const;
	// 把请求交给扩展；返回 false = 无接收端，调用方应当场把这条请求失败掉
	bool sendToSink(const QString& rpcId, const QString& method, const QJsonObject& args);
	// 接管路径的一元 RPC 入口：回调留在 m_pending（std::function 不是 metatype，不能随接口/信号传出去）
	void dispatchTakeoverCall(const QString& rpcId, const QString& method, const QJsonObject& args, std::function<void(const QJsonValue&)> onSuccess, std::function<void(const RpcError&)> onError);
	// 按 rpcId 取出挂着的一条请求并用错误收尾；不在表里返回 false
	bool failPending(const QString& rpcId, const QString& code, const QString& message);
	// 交还后端时把所有挂着的接管请求失败掉
	void failTakeoverPending(const QString& code, const QString& message);
	void sweepTakeoverTimeouts();

	// 进入接管态的一次性收尾（判废在途认证 / 断开 mux / 把等认证的请求改投扩展）
	void enterTakenoverState();
	void leaveTakenoverState();

	QUrl makeUrl(const QString& path) const;
	void post(const QString& path, const QJsonObject& body, std::function<void(const QJsonValue& value)> onSuccess, std::function<void(const RpcError& error)> onError);
	// 用启动令牌换认证 cookie：GET /?token=<令牌> → 303 + Set-Cookie，之后所有 /api 请求与
	// WebSocket 握手都必须带上，否则一律 401；成功后自动 openStreams()
	void startAuthHandshake();

private slots:
	void onReplyFinished();

private:
	void onStreamTextMessage(const QString& message);

	// 帧形如 {type:"open",streamId,endpoint,payload:{args}}
	void sendStreamOpen(const QString& endpoint, const QString& streamId, const QJsonObject& args = {});
	void sendStreamCancel(const QString& streamId);

	// 四条逻辑流的 item 分发：$events、workspace/follow、session/follow、session/control
	void handleEventsItem(const QJsonValue& value);
	void handleWorkspaceItem(const QJsonValue& value);
	void handleSessionItem(const QJsonValue& value);
	void handleControlItem(const QJsonValue& value);

	void emitSessionEventFrame(const QString& sessionId, const QJsonObject& event);
	void scheduleReconnect();
	// ⚠️ 每次 open 都必须唯一；服务端遇重复 id 会 close(1008) 关掉整条 mux
	QString nextStreamId(const QString& prefix);

	void flushAuthQueue();
	void failAuthQueue(const QString& code, const QString& message);

private:
	QNetworkAccessManager* m_nam = nullptr;
	QWebSocket* m_stream = nullptr;         // /api/remote.mux（0.1.5 唯一的一条流通道）
	QUrl m_baseUrl;
	QHash<QString, PendingCall> m_pending;
	QHash<QString, PendingCall> m_parsing;  // 响应体已在后台解析、待回投
	QString m_launchToken;
	QString m_authCookie;
	bool m_authenticated = false;
	bool m_authInFlight = false;
	// 认证代次：丢掉服务端重启后晚一步回来的过期回包
	quint64 m_authAttempt = 0;
	QList<QueuedCall> m_authQueue;

	QString m_clientId;
	QString m_eventsStreamId;
	QString m_workspaceStreamId;
	QString m_controlStreamId;
	QString m_sessionStreamId;
	QString m_followedSessionId;
	bool m_eventsReady = false;
	bool m_streamConnected = false;
	int m_streamSeq = 0;                    // 逻辑流 id 自增号
	bool m_streamClosing = false;
	QTimer* m_reconnectTimer = nullptr;
	int m_reconnectDelayMs = 1000;

	// true = 全部出站交给客户端扩展，不走 HTTP、不开 mux、不启动认证握手
	bool m_takenover = false;
	// 回填超时扫描（只有接管态才跑）
	QTimer* m_takeoverSweep = nullptr;

	bool m_destroyed = false;
};
