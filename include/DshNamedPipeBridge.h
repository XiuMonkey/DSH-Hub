#pragma once

// ------------------------------------------------------------------
// DshNamedPipeBridge.h
// ------------------------------------------------------------------
// DSH Hub 客户端的命名管道桥接服务。
// 监听 \\.\pipe\dshhub-bridge，Node/DSh server 可连接本管道，
// 以 JSON + 换行 的协议发送工具调用请求，由客户端负责装载 DLL 并返回结果。
// ------------------------------------------------------------------

#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QHash>
#include <QByteArray>
#include <QJsonObject>

class DshNamedPipeBridge : public QObject
{
	Q_OBJECT

public:
	explicit DshNamedPipeBridge(QObject* parent = nullptr);

	// 启动监听。pipeName 在 Windows 上会对应 \\.\pipe\<pipeName>
	bool start(const QString& pipeName = QStringLiteral("dshhub-bridge"));
	void stop();

	bool isListening() const;
	QString errorString() const;
	QString pipeName() const;

	// 主动向某个连接的客户端发送 JSON 响应（协议：JSON + '\n'）
	void sendResponse(QLocalSocket* socket,
		int id,
		bool ok,
		const QJsonObject& result,
		const QString& error = QString());

signals:
	// 收到一个完整的请求行（JSON 已被解析）
	void requestReceived(int id,
		const QString& tool,
		const QJsonObject& args,
		QLocalSocket* socket);

	void clientConnected();
	void clientDisconnected();
	void logMessage(const QString& message);

private slots:
	void onNewConnection();
	void onReadyRead();
	void onDisconnected();

private:
	void handleLine(QLocalSocket* socket, const QByteArray& line);

	QLocalServer* m_server = nullptr;
	QHash<QLocalSocket*, QByteArray> m_buffers;
	QString m_pipeName;
	QString m_errorString;
};
