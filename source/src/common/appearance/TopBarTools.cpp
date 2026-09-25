#include "common/appearance/TopBarTools.h"

#include <QDebug>
#include <QHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

namespace
{
	const char* const kEndpointPath = "/api/tools-filter";
	const char* const kDefaultDirectoryName = "Default";
	const char* const kLegacyDirectoryName = "tools";

	QString bodyPreview(const QByteArray& raw)
	{
		const QString text = QString::fromUtf8(raw).trimmed();
		return text.size() > 160 ? text.left(160) + QStringLiteral("…") : text;
	}

	QString rowToolName(const QJsonValue& row)
	{
		if (row.isString())
			return row.toString();
		if (row.isObject())
			return row.toObject().value(QStringLiteral("ToolName")).toString();
		return QString();
	}

	// 假标记集合必须与插件同款，否则界面会在勾选框上撒谎
	bool flagIsFalse(const QJsonValue& value)
	{
		if (value.isBool())
			return !value.toBool();
		if (value.isDouble())
			return value.toDouble() == 0.0;
		if (!value.isString())
			return false;

		const QString text = value.toString().trimmed().toUpper();
		return text == QStringLiteral("FALSE") || text == QStringLiteral("0")
			|| text == QStringLiteral("NO") || text == QStringLiteral("OFF");
	}

	// 字符串 = 可见；对象看 IsVisible，只有显式假标记才算隐藏
	bool rowVisible(const QJsonValue& row)
	{
		if (row.isString())
			return true;
		if (!row.isObject())
			return true;
		return !flagIsFalse(row.toObject().value(QStringLiteral("IsVisible")));
	}

	// 空名与历史遗留的 "tools" 都归一到 Default，否则老会话里会显示一个叫 tools 的目录
	QString normalizedDirectoryName(const QString& raw)
	{
		const QString name = raw.trimmed();
		if (name.isEmpty())
			return QString::fromLatin1(kDefaultDirectoryName);
		if (name.compare(QString::fromLatin1(kLegacyDirectoryName), Qt::CaseInsensitive) == 0)
			return QString::fromLatin1(kDefaultDirectoryName);
		return name;
	}

	QString directoryKey(const QString& name)
	{
		return normalizedDirectoryName(name).toLower();
	}

	// 插件返回的 directory 缺失（旧插件、纯单测）时用它做功能分组
	QString builtinFunctionDirectory(const QString& toolName)
	{
		if (toolName.isEmpty())
			return QString();

		static const QHash<QString, QString> directories{
			{ QStringLiteral("read"), QStringLiteral("File") },
			{ QStringLiteral("write"), QStringLiteral("File") },
			{ QStringLiteral("edit"), QStringLiteral("File") },
			{ QStringLiteral("read_image"), QStringLiteral("File") },
			{ QStringLiteral("str_replace_editor"), QStringLiteral("File") },
			{ QStringLiteral("glob"), QStringLiteral("Search") },
			{ QStringLiteral("grep"), QStringLiteral("Search") },
			{ QStringLiteral("bash"), QStringLiteral("Shell") },
			{ QStringLiteral("pwsh"), QStringLiteral("Shell") },
			{ QStringLiteral("web_search"), QStringLiteral("Web") },
			{ QStringLiteral("web_fetch"), QStringLiteral("Web") },
			{ QStringLiteral("create_goal"), QStringLiteral("Task") },
			{ QStringLiteral("get_goal"), QStringLiteral("Task") },
			{ QStringLiteral("update_goal"), QStringLiteral("Task") },
			{ QStringLiteral("job_list"), QStringLiteral("Task") },
			{ QStringLiteral("job_output"), QStringLiteral("Task") },
			{ QStringLiteral("job_kill"), QStringLiteral("Task") },
			{ QStringLiteral("todo_write"), QStringLiteral("Task") },
			{ QStringLiteral("schedule_create"), QStringLiteral("Task") },
			{ QStringLiteral("schedule_list"), QStringLiteral("Task") },
			{ QStringLiteral("schedule_delete"), QStringLiteral("Task") },
			{ QStringLiteral("subagent"), QStringLiteral("Agent") },
			{ QStringLiteral("subagent_fork"), QStringLiteral("Agent") },
			{ QStringLiteral("list_subagent_models"), QStringLiteral("Agent") },
			{ QStringLiteral("list_agents"), QStringLiteral("Agent") },
			{ QStringLiteral("send_message"), QStringLiteral("Agent") },
			{ QStringLiteral("interrupt_agent"), QStringLiteral("Agent") },
			{ QStringLiteral("exit_plan_mode"), QStringLiteral("Plan") },
			{ QStringLiteral("workflow"), QStringLiteral("Workflow") },
			{ QStringLiteral("ralph"), QStringLiteral("Workflow") },
			{ QStringLiteral("skill"), QStringLiteral("Skill") },
			{ QStringLiteral("ask_user_question"), QStringLiteral("Interaction") },
			{ QStringLiteral("present"), QStringLiteral("Output") },
			{ QStringLiteral("run_code"), QStringLiteral("Code") },
			{ QStringLiteral("cordis_inspect_list"), QStringLiteral("Cordis") },
			{ QStringLiteral("cordis_inspect_query"), QStringLiteral("Cordis") },
			{ QStringLiteral("cordis_inspect_self"), QStringLiteral("Cordis") },
			{ QStringLiteral("cordis_define"), QStringLiteral("Cordis") },
			{ QStringLiteral("cordis_run"), QStringLiteral("Cordis") },
			{ QStringLiteral("cordis_stop"), QStringLiteral("Cordis") },
			{ QStringLiteral("cordis_undefine"), QStringLiteral("Cordis") },
			{ QStringLiteral("gettools"), QStringLiteral("ToolsFilterPlugin") },
		};

		const auto exact = directories.constFind(toolName);
		if (exact != directories.constEnd())
			return exact.value();
		if (toolName.startsWith(QStringLiteral("mcp_")))
			return QStringLiteral("MCP");
		return QString();
	}
} // namespace

ToolsFilter::ToolsFilter(QObject* parent)
	: QObject(parent)
	, m_nam(new QNetworkAccessManager(this))
{
}

void ToolsFilter::setBaseUrl(const QUrl& url)
{
	m_baseUrl = url;
}

QString ToolsFilter::defaultDirectoryName()
{
	return QLatin1String(kDefaultDirectoryName);
}

// 只借 baseUrl 的 scheme/host/port：它带启动令牌，直接拼会让请求落到 GET / 拿回 HTML
QUrl ToolsFilter::endpointUrl() const
{
	QUrl url = m_baseUrl;
	url.setPath(QLatin1String(kEndpointPath));
	url.setQuery(QString());
	url.setFragment(QString());
	return url;
}

void ToolsFilter::fetch(const QString& sessionId, std::function<void(const ToolFilterCatalog&)> done)
{
	ToolFilterCatalog catalog;
	catalog.sessionId = sessionId;

	if (sessionId.isEmpty()) {
		catalog.error = qtTrId("toolfilter_no_session");
		if (done)
			done(catalog);
		return;
	}
	if (m_baseUrl.isEmpty()) {
		catalog.error = qtTrId("toolfilter_server_not_ready");
		if (done)
			done(catalog);
		return;
	}

	QUrl url = endpointUrl();
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("session"), sessionId);
	url.setQuery(query);

	QNetworkRequest request(url);
	QNetworkReply* reply = m_nam->get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, sessionId, done]() {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray raw = reply->readAll();
		const QString transportError = reply->errorString();
		const bool failed = reply->error() != QNetworkReply::NoError || status >= 400;
		reply->deleteLater();

		ToolFilterCatalog catalog;
		catalog.sessionId = sessionId;

		if (failed) {
			// 插件缺席时会落到 /api 前缀路由，拿回的不是我们的 JSON
			catalog.error = status > 0
				? qtTrId("toolfilter_plugin_no_response_http_fmt").arg(status)
				: qtTrId("toolfilter_plugin_no_response_fmt").arg(transportError);
			qWarning().noquote() << "[ToolsFilter] GET failed status=" << status
				<< "error=" << transportError << "body=" << bodyPreview(raw);
			if (done)
				done(catalog);
			return;
		}

		const QJsonObject root = QJsonDocument::fromJson(raw).object();
		if (!root.value(QStringLiteral("tools")).isArray()) {
			catalog.error = qtTrId("toolfilter_plugin_unavailable");
			qWarning().noquote() << "[ToolsFilter] unexpected payload, body=" << bodyPreview(raw);
			if (done)
				done(catalog);
			return;
		}

		catalog.ok = true;
		catalog.degraded = root.value(QStringLiteral("degraded")).toBool();
		catalog.configFound = root.value(QStringLiteral("configFound")).toBool();
		catalog.dropGuidance = root.value(QStringLiteral("dropGuidance")).toBool(true);
		catalog.visibleCount = root.value(QStringLiteral("visibleCount")).toInt();
		// UI 保存时整份替换，必须原样带回
		for (const QJsonValue& value : root.value(QStringLiteral("hideContexts")).toArray()) {
			const QString name = value.toString();
			if (!name.isEmpty())
				catalog.hideContexts.append(name);
		}
		catalog.directories = buildDirectories(root.value(QStringLiteral("tools")).toArray(),
			root.value(QStringLiteral("storedConfig")).toObject());

		if (done)
			done(catalog);
		});
}

void ToolsFilter::ensureSession(const QString& sessionId,
	std::function<void(bool ok, const ToolFilterCatalog& catalog, bool created, const QString& error)> done)
{
	fetch(sessionId, [this, done, sessionId](const ToolFilterCatalog& catalog) {
		if (!catalog.ok) {
			if (done)
				done(false, catalog, false, catalog.error);
			return;
		}
		if (catalog.configFound) {
			if (done)
				done(true, catalog, false, QString());
			return;
		}

		// 用工具名建一份初始版（不写描述与参数），分组交给 buildDirectories()
		QVector<ToolFilterDirectory> allVisible = catalog.directories;
		for (ToolFilterDirectory& directory : allVisible) {
			directory.expanded = true;
			for (ToolFilterEntry& entry : directory.tools)
				entry.visible = true;
		}

		save(sessionId, allVisible, catalog.dropGuidance, catalog.hideContexts,
			[done, catalog](bool ok, const QString& error) {
				if (done)
					done(ok, catalog, ok, error);
			});
		});
}

void ToolsFilter::save(const QString& sessionId, const QVector<ToolFilterDirectory>& directories,
	bool dropGuidance, const QStringList& hideContexts,
	std::function<void(bool ok, const QString& error)> done)
{
	if (sessionId.isEmpty()) {
		if (done)
			done(false, qtTrId("toolfilter_no_session"));
		return;
	}
	if (m_baseUrl.isEmpty()) {
		if (done)
			done(false, qtTrId("toolfilter_server_not_ready"));
		return;
	}

	const QJsonObject config = buildDocument(directories, dropGuidance, hideContexts);

	QJsonObject payload;
	payload.insert(QStringLiteral("sessionId"), sessionId);
	payload.insert(QStringLiteral("config"), config);

	QNetworkRequest request(endpointUrl());
	// 插件只收 application/json，否则 415
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	QNetworkReply* reply = m_nam->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
	connect(reply, &QNetworkReply::finished, this, [reply, sessionId, done]() {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray raw = reply->readAll();
		const QString transportError = reply->errorString();
		const bool failed = reply->error() != QNetworkReply::NoError || status >= 400;
		reply->deleteLater();

		if (!failed && QJsonDocument::fromJson(raw).object().value(QStringLiteral("ok")).toBool()) {
			if (done)
				done(true, QString());
			return;
		}

		const QJsonObject root = QJsonDocument::fromJson(raw).object();
		QString message = root.value(QStringLiteral("error")).toString();
		if (message.isEmpty()) {
			if (status == 409)
				message = qtTrId("toolfilter_session_not_persisted");
			else if (status > 0)
				message = qtTrId("toolfilter_save_failed_http_fmt").arg(status);
			else
				message = qtTrId("toolfilter_save_failed_reason_fmt").arg(transportError);
		}
		qWarning().noquote() << "[ToolsFilter] POST failed status=" << status
			<< "session=" << sessionId << "body=" << bodyPreview(raw);
		if (done)
			done(false, message);
		});
}

QVector<ToolFilterDirectory> ToolsFilter::buildDirectories(const QJsonArray& tools, const QJsonObject& storedConfig)
{
	QVector<ToolFilterDirectory> directories;
	QHash<QString, int> indexByKey;
	QHash<QString, QString> directoryOfTool;
	QHash<QString, bool> storedVisible;

	auto ensureDirectory = [&](const QString& rawName) -> int {
		const QString name = normalizedDirectoryName(rawName);
		const QString key = name.toLower();
		const auto existing = indexByKey.constFind(key);
		if (existing != indexByKey.constEnd())
			return existing.value();

		ToolFilterDirectory entry;
		entry.name = name;
		indexByKey.insert(key, directories.size());
		directories.append(entry);
		return directories.size() - 1;
		};

	// 空目录也留着 —— 配置里声明过的目录都要在界面上露面
	const QJsonArray filterList = storedConfig.value(QStringLiteral("FilterList")).toArray();
	const bool autoGroup = filterList.isEmpty();
	for (const QJsonValue& directoryValue : filterList) {
		const QJsonObject directory = directoryValue.toObject()
			.value(QStringLiteral("Directory")).toObject();
		if (directory.isEmpty())
			continue;

		const QString name = normalizedDirectoryName(
			directory.value(QStringLiteral("DirectoryName")).toString());
		const QString key = name.toLower();
		const bool isNewDirectory = !indexByKey.contains(key);
		const int index = ensureDirectory(name);
		if (isNewDirectory) {
			directories[index].description = directory.value(QStringLiteral("Description")).toString();
			directories[index].expanded = !flagIsFalse(directory.value(QStringLiteral("IsExpanded")));
		}

		const QJsonArray toolsList = directory.value(QStringLiteral("ToolsList")).toArray();
		for (const QJsonValue& row : toolsList) {
			const QString toolName = rowToolName(row);
			if (toolName.isEmpty())
				continue;
			// 同一工具出现在多个目录时第一个认领的说话；可见性相反，后写覆盖先写
			if (!directoryOfTool.contains(toolName))
				directoryOfTool.insert(toolName, key);
			storedVisible.insert(toolName, rowVisible(row));
		}
	}

	// 无存储配置时，插件随 tools 返回的 directory 即初始目录，缺该字段才回退 Default
	const QString defaultKey = directoryKey(defaultDirectoryName());
	int defaultIndex = -1;
	if (!autoGroup) {
		defaultIndex = ensureDirectory(defaultDirectoryName());
	}

	// 工具顺序跟着插件返回的目录
	for (const QJsonValue& value : tools) {
		const QJsonObject tool = value.toObject();
		ToolFilterEntry entry;
		entry.name = tool.value(QStringLiteral("name")).toString();
		if (entry.name.isEmpty())
			continue;
		entry.description = tool.value(QStringLiteral("description")).toString();
		const QJsonValue parameters = tool.value(QStringLiteral("parameters"));
		if (parameters.isObject())
			entry.parameters = QString::fromUtf8(QJsonDocument(parameters.toObject()).toJson(QJsonDocument::Compact));

		const auto stored = storedVisible.constFind(entry.name);
		entry.visible = stored == storedVisible.constEnd() ? true : stored.value();

		int index = defaultIndex;
		if (autoGroup) {
			QString directory = tool.value(QStringLiteral("directory")).toString().trimmed();
			if (directory.isEmpty())
				directory = builtinFunctionDirectory(entry.name);
			if (!directory.isEmpty()) {
				index = ensureDirectory(directory);
			}
			else {
				if (defaultIndex < 0)
					defaultIndex = ensureDirectory(defaultDirectoryName());
				index = defaultIndex;
			}
		}
		else {
			index = indexByKey.value(directoryOfTool.value(entry.name, defaultKey), defaultIndex);
		}

		if (index >= 0 && index < directories.size())
			directories[index].tools.append(entry);
	}

	if (autoGroup && directories.isEmpty())
		ensureDirectory(defaultDirectoryName());

	return directories;
}

QJsonArray ToolsFilter::buildToolsList(const QVector<ToolFilterEntry>& tools)
{
	// 隐藏的写 { ToolName, IsVisible:"False" }
	QJsonArray toolsList;
	for (const ToolFilterEntry& tool : tools) {
		if (tool.name.isEmpty())
			continue;
		if (tool.visible) {
			toolsList.append(tool.name);
			continue;
		}
		QJsonObject row;
		row.insert(QStringLiteral("ToolName"), tool.name);
		row.insert(QStringLiteral("IsVisible"), QStringLiteral("False"));
		toolsList.append(row);
	}
	return toolsList;
}

int ToolsFilter::toolCount(const QVector<ToolFilterDirectory>& directories)
{
	int count = 0;
	for (const ToolFilterDirectory& directory : directories)
		count += directory.tools.size();
	return count;
}

int ToolsFilter::hiddenCount(const QVector<ToolFilterDirectory>& directories)
{
	int hidden = 0;
	for (const ToolFilterDirectory& directory : directories) {
		for (const ToolFilterEntry& tool : directory.tools) {
			if (!tool.visible)
				++hidden;
		}
	}
	return hidden;
}

QJsonObject ToolsFilter::buildDocument(const QVector<ToolFilterDirectory>& directories, bool dropGuidance,
	const QStringList& hideContexts)
{
	QJsonArray filterList;
	for (const ToolFilterDirectory& directory : directories) {
		QJsonObject payload;
		// IsExpanded 是筛选语义（"False" = 插件整组隐藏），与界面展开 / 收起无关，原样带回
		payload.insert(QStringLiteral("IsExpanded"),
			directory.expanded ? QStringLiteral("True") : QStringLiteral("False"));
		payload.insert(QStringLiteral("DirectoryName"),
			directory.name.isEmpty() ? defaultDirectoryName() : directory.name);
		if (!directory.description.isEmpty())
			payload.insert(QStringLiteral("Description"), directory.description);
		payload.insert(QStringLiteral("ToolsList"), buildToolsList(directory.tools));

		QJsonObject entry;
		entry.insert(QStringLiteral("Directory"), payload);
		filterList.append(entry);
	}

	QJsonObject document;
	document.insert(QStringLiteral("FilterList"), filterList);
	document.insert(QStringLiteral("DropGuidance"), dropGuidance ? QStringLiteral("True") : QStringLiteral("False"));
	// 名单非空才写这一项
	if (!hideContexts.isEmpty()) {
		QJsonArray names;
		for (const QString& name : hideContexts) {
			if (!name.isEmpty())
				names.append(name);
		}
		if (!names.isEmpty())
			document.insert(QStringLiteral("HideContexts"), names);
	}
	return document;
}
