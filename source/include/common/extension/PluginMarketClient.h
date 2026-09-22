#pragma once

// 插件市场（/dsh-market/*）的 HTTP 客户端（无控件）：只负责请求与解析，失败时自动附带服务端诊断日志。
// GET registry/installed 与 POST install/uninstall/update/restart；分别以 registryLoaded / installedLoaded / operationCompleted / operationFailed 通知。

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;

class PluginMarketClient : public QObject
{
	Q_OBJECT

public:
	explicit PluginMarketClient(QObject* parent = nullptr);

	void setBaseUrl(const QUrl& url);

	// 各接口路径（也用于让调用方识别失败来源）
	static QString registryPath();
	static QString installedPath();
	static QString installPath();
	static QString uninstallPath();
	static QString updatePath();
	static QString restartPath();

	void fetchRegistry();
	void fetchInstalled();
	void installPlugin(const QString& url);
	void uninstallPlugin(const QString& name);
	void updatePlugin(const QString& name);
	void restartServer();

signals:
	// registry 拉取成功；source 可能是 "snapshot" / "cache" / 其它（实时数据）
	void registryLoaded(const QJsonArray& plugins, const QString& source);
	void registryFailed(const QString& error, int status);

	// 已安装列表拉取成功（name -> version）
	void installedLoaded(const QJsonObject& installed);

	// POST 操作成功/失败；path 用于区分具体操作
	void operationCompleted(const QString& path);
	void operationFailed(const QString& path, const QString& error, int status);

private:
	void post(const QString& path, const QJsonObject& body);
	// 诊断：拉取服务端 dshmarket 内存日志（5xx / 传输错误时自动触发）
	void fetchDiagnosticLogs(const QString& reason);
	// 由 baseUrl 只借 scheme/host/port 拼出接口地址（见实现里的说明）
	QUrl endpointUrl(const QString& path) const;

	QUrl m_baseUrl;
	QNetworkAccessManager* m_nam = nullptr;
};
