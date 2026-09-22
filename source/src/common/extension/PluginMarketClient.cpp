#include "common/extension/PluginMarketClient.h"

#include <QDebug>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace
{
	const char* const kRegistryPath = "/dsh-market/registry";
	const char* const kInstalledPath = "/dsh-market/installed";
	const char* const kInstallPath = "/dsh-market/install";
	const char* const kUninstallPath = "/dsh-market/uninstall";
	const char* const kUpdatePath = "/dsh-market/update";
	const char* const kRestartPath = "/dsh-market/restart";
	const char* const kLogsPath = "/dsh-market/logs";

	// 失败的响应体可能不是 JSON（例如网关 502），截断后再展示
	QString bodyPreview(const QByteArray& raw)
	{
		return QString::fromUtf8(raw).trimmed().left(300);
	}
}

PluginMarketClient::PluginMarketClient(QObject* parent)
	: QObject(parent)
	, m_nam(new QNetworkAccessManager(this))
{
}

void PluginMarketClient::setBaseUrl(const QUrl& url)
{
	m_baseUrl = url;
}

/**
 * 由 baseUrl 拼出接口地址：**只借它的 scheme/host/port**，path 重设、query 与 fragment 清掉。
 *
 * 为什么必须这样：ServerManager::baseUrlReady 交出来的 baseUrl 是"带启动令牌"的那条
 * （http://127.0.0.1:<port>/?token=…）。老写法
 *     QUrl(base.toString(QUrl::RemovePath) + path)
 * 会得到 http://127.0.0.1:<port>?token=…/dsh-market/registry —— path 为空、接口地址被
 * 当成 token 的参数值，请求实际落到 `GET /`：
 *   · 没有 cookie 时 → index 围栏 401；
 *   · 有 cookie 时   → index 路由 303 到 index.html，客户端把 HTML 当 JSON 解析，
 *                      于是市场显示"0 个插件"（静默故障，最难查的那种）。
 */
QUrl PluginMarketClient::endpointUrl(const QString& path) const
{
	QUrl url = m_baseUrl;
	url.setPath(path);
	url.setQuery(QString());
	url.setFragment(QString());
	return url;
}

/** 纯 origin（scheme://host:port），给 POST 的 Origin 头用。 */
static QUrl originOf(const QUrl& base)
{
	QUrl url = base;
	url.setPath(QString());
	url.setQuery(QString());
	url.setFragment(QString());
	return url;
}

QString PluginMarketClient::registryPath()
{
	return QLatin1String(kRegistryPath);
}

QString PluginMarketClient::installedPath()
{
	return QLatin1String(kInstalledPath);
}

QString PluginMarketClient::installPath()
{
	return QLatin1String(kInstallPath);
}

QString PluginMarketClient::uninstallPath()
{
	return QLatin1String(kUninstallPath);
}

QString PluginMarketClient::updatePath()
{
	return QLatin1String(kUpdatePath);
}

QString PluginMarketClient::restartPath()
{
	return QLatin1String(kRestartPath);
}

// ------------------------------------------------------------------
// GET
// ------------------------------------------------------------------

void PluginMarketClient::fetchRegistry()
{
	const QString path = registryPath();
	QNetworkRequest request(endpointUrl(path));
	qInfo().noquote() << "[Market] GET" << path;

	QNetworkReply* reply = m_nam->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, path]() {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray raw = reply->readAll();
		const QString transportError = reply->errorString();
		const bool failed = reply->error() != QNetworkReply::NoError || status >= 400;
		reply->deleteLater();

		if (failed) {
			qWarning().noquote() << "[PluginMarketClient] registry request failed, status=" << status
				<< "error=" << transportError
				<< "body=" << bodyPreview(raw);
			emit registryFailed(transportError, status);
			if (status >= 500)
				fetchDiagnosticLogs(path);
			return;
		}

		const QJsonObject root = QJsonDocument::fromJson(raw).object();
		const QString source = root.value(QStringLiteral("source")).toString();
		const QJsonArray plugins = root.value(QStringLiteral("registry")).toObject()
			.value(QStringLiteral("plugins")).toArray();

		// 契约没满足就喊出来：这类"静默 0 个插件"（例如被前端回退喂了一份 HTML）
		// 过去只能靠猜，这里把状态码/字节数/响应开头都打出来
		if (plugins.isEmpty())
			qWarning().noquote() << "[PluginMarketClient] registry payload unexpected"
			<< "status=" << status << "bytes=" << raw.size() << "body=" << bodyPreview(raw);

		qInfo().noquote() << "[PluginMarketClient] registry loaded"
			<< "source=" << source
			<< "plugins=" << plugins.size();

		emit registryLoaded(plugins, source);
		});
}

void PluginMarketClient::fetchInstalled()
{
	const QString path = installedPath();
	QNetworkRequest request(endpointUrl(path));
	qInfo().noquote() << "[Market] GET" << path;

	QNetworkReply* reply = m_nam->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, path]() {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray raw = reply->readAll();
		const QString transportError = reply->errorString();
		const bool failed = reply->error() != QNetworkReply::NoError || status >= 400;
		reply->deleteLater();

		if (failed) {
			qWarning().noquote() << "[PluginMarketClient] installed request failed, status=" << status
				<< "error=" << transportError
				<< "body=" << bodyPreview(raw);
			return;
		}

		const QJsonObject root = QJsonDocument::fromJson(raw).object();
		emit installedLoaded(root.value(QStringLiteral("installed")).toObject());
		});
}

// ------------------------------------------------------------------
// POST
// ------------------------------------------------------------------

void PluginMarketClient::installPlugin(const QString& url)
{
	if (url.isEmpty())
		return;

	QJsonObject body;
	body.insert(QStringLiteral("url"), url);
	post(installPath(), body);
}

void PluginMarketClient::uninstallPlugin(const QString& name)
{
	QJsonObject body;
	body.insert(QStringLiteral("name"), name);
	post(uninstallPath(), body);
}

void PluginMarketClient::updatePlugin(const QString& name)
{
	QJsonObject body;
	body.insert(QStringLiteral("name"), name);
	post(updatePath(), body);
}

void PluginMarketClient::restartServer()
{
	post(restartPath(), QJsonObject());
}

void PluginMarketClient::post(const QString& path, const QJsonObject& body)
{
	QNetworkRequest request(endpointUrl(path));
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	// 市场服务端要求同源（Origin 的 host 必须等于 Host）；用纯 origin，别把令牌带进去
	request.setRawHeader("Origin", originOf(m_baseUrl).toString().toUtf8());

	qInfo().noquote() << QStringLiteral("[Market] POST %1 body=%2")
		.arg(path, QString::fromUtf8(QJsonDocument(body).toJson(QJsonDocument::Compact)));

	QNetworkReply* reply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(reply, &QNetworkReply::finished, this, [this, reply, path]() {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray raw = reply->readAll();
		reply->deleteLater();

		const bool transportError = reply->error() != QNetworkReply::NoError;
		const QJsonObject root = QJsonDocument::fromJson(raw).object();
		QString serverError = root.value(QStringLiteral("error")).toString();

		if (transportError || status >= 400) {
			// 服务端可能返回非 JSON（如网关 502），此时展示原始状态码与正文
			if (serverError.isEmpty())
				serverError = QStringLiteral("HTTP %1 %2").arg(status).arg(bodyPreview(raw));
			qWarning().noquote() << "[Market] POST" << path
				<< "failed, status=" << status
				<< "error=" << serverError;
			emit operationFailed(path, serverError, status);

			// 5xx / 传输错误多半只在服务端可见（pnpm、网络、路由抛异常），
			// 顺手拉取 dshmarket 内存日志，定位 502 的具体原因
			if (status >= 500 || transportError)
				fetchDiagnosticLogs(path);
			return;
		}

		qInfo().noquote() << "[Market] POST" << path << "ok, status=" << status;
		emit operationCompleted(path);
		});
}

void PluginMarketClient::fetchDiagnosticLogs(const QString& reason)
{
	QNetworkRequest request(endpointUrl(QLatin1String(kLogsPath)));

	QNetworkReply* reply = m_nam->get(request);
	connect(reply, &QNetworkReply::finished, this, [reason, reply]() {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray raw = reply->readAll();
		reply->deleteLater();

		qInfo().noquote() << "[Market] fetchDiagnosticLogs reason=" << reason << "status=" << status;
		const QString text = QString::fromUtf8(raw).trimmed();
		if (text.isEmpty()) {
			qInfo().noquote() << "[dsh-market-log] (empty)";
			return;
		}
		for (const QString& line : text.split(QLatin1Char('\n')))
			qInfo().noquote() << "[dsh-market-log]" << line;
		});
}