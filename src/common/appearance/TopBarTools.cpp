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
	// 与 resources/ToolsFilterPlugin/index.js 注册的路由一致
	const char* const kEndpointPath = "/api/tools-filter";
	// 程序自动建立的目录名。**历史版本叫 "tools"**，见 normalizedDirectoryName()
	const char* const kDefaultDirectoryName = "Default";
	const char* const kLegacyDirectoryName = "tools";

	QString bodyPreview(const QByteArray& raw)
	{
		const QString text = QString::fromUtf8(raw).trimmed();
		return text.size() > 160 ? text.left(160) + QStringLiteral("…") : text;
	}

	// 从一行 ToolsList 里取出工具名（可能是字符串，也可能是 {ToolName, …}）
	QString rowToolName(const QJsonValue& row)
	{
		if (row.isString())
			return row.toString();
		if (row.isObject())
			return row.toObject().value(QStringLiteral("ToolName")).toString();
		return QString();
	}

	/**
	 * 插件那边的假标记集合（FALSE / 0 / no / off，不分大小写；布尔与数字按
	 * String(value) 的路子算）。界面必须与它同款：插件的判定才是真正生效的那个，
	 * 界面读得不一样就会在勾选框上撒谎。
	 */
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

	// 这一行表达的可见性：字符串 = 可见；对象看 IsVisible（只有显式的假标记才算隐藏）
	bool rowVisible(const QJsonValue& row)
	{
		if (row.isString())
			return true;
		if (!row.isObject())
			return true;
		return !flagIsFalse(row.toObject().value(QStringLiteral("IsVisible")));
	}

	/**
	 * 目录名归一：
	 *   · 空名 → Default（配置里没写 DirectoryName 的目录在界面上得有个标题）；
	 *   · 历史自动建立的 "tools" → Default（那时还没有目录界面，程序建的目录叫
	 *     tools；不归一的话老会话的窗口里会显示一个叫 tools 的目录，而用户看到的
	 *     "从 tools 改成 Default" 只对新会话生效）。
	 * 名字只是给人看的标签 —— 插件那边的目录名不参与任何判定。
	 */
	QString normalizedDirectoryName(const QString& raw)
	{
		const QString name = raw.trimmed();
		if (name.isEmpty())
			return QString::fromLatin1(kDefaultDirectoryName);
		if (name.compare(QString::fromLatin1(kLegacyDirectoryName), Qt::CaseInsensitive) == 0)
			return QString::fromLatin1(kDefaultDirectoryName);
		return name;
	}

	// 目录的比较键：只有大小写不同不该变成两个目录
	QString directoryKey(const QString& name)
	{
		return normalizedDirectoryName(name).toLower();
	}

	/**
	 * 原版自带工具的功能目录。插件返回的 `directory` 字段缺失时（旧插件版本、
	 * 纯函数单测），用它保证初始文档最少也能按功能分组；未知工具再回退 Default。
	 */
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

/**
 * 由 baseUrl 拼出接口地址：**只借它的 scheme/host/port**，path 重设、query 与
 * fragment 清掉。理由同 PluginMarketClient::endpointUrl：ServerManager 交出来的
 * baseUrl 是"带启动令牌"的那条（…/?token=…），直接拼 path 会把接口地址当成 token
 * 的参数值、请求落到 `GET /`，于是拿回一份 HTML 而不是 JSON。
 */
QUrl ToolsFilter::endpointUrl() const
{
	QUrl url = m_baseUrl;
	url.setPath(QLatin1String(kEndpointPath));
	url.setQuery(QString());
	url.setFragment(QString());
	return url;
}

// ------------------------------------------------------------------
// GET
// ------------------------------------------------------------------

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
	// 回调挂在 this 上：本对象销毁时自动断开，不会有回调打到已释放的对象
	connect(reply, &QNetworkReply::finished, this, [this, reply, sessionId, done]() {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray raw = reply->readAll();
		const QString transportError = reply->errorString();
		const bool failed = reply->error() != QNetworkReply::NoError || status >= 400;
		reply->deleteLater();

		ToolFilterCatalog catalog;
		catalog.sessionId = sessionId;

		if (failed) {
			// 插件缺席时请求会落到 /api 前缀路由（连接插件），拿回的不是我们的 JSON
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
		// 这份名单由配置持有；UI 保存时整份替换，必须原样带回，否则会被冲掉
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

		// 还没有配置文件：用工具名建一份初始版（描述与参数不写进去）。
		// buildDirectories() 会按插件返回的 `directory` 元数据分组；插件工具各归
		// 其插件，原版工具按功能分目录。
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

// ------------------------------------------------------------------
// POST
// ------------------------------------------------------------------

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
	// 插件要求 application/json（否则 415）—— 这也是它挡跨站简单请求的方式
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

// ------------------------------------------------------------------
// 纯函数
// ------------------------------------------------------------------

QVector<ToolFilterDirectory> ToolsFilter::buildDirectories(const QJsonArray& tools, const QJsonObject& storedConfig)
{
	QVector<ToolFilterDirectory> directories;
	QHash<QString, int> indexByKey;            // 目录键 → directories 下标
	QHash<QString, QString> directoryOfTool;   // 工具名 → 认领它的目录键
	QHash<QString, bool> storedVisible;        // 工具名 → 是否可见

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

	// 1) 目录骨架：按存储顺序，**空目录也留着** —— 配置文件里声明过的目录都要在
	//    界面上露面（用户自己写进去的分组不能因为"暂时没有工具"就消失）。
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
			// 只有显式的假标记才算"整组隐藏"（与插件 compileFilterList 同款判定）
			directories[index].expanded = !flagIsFalse(directory.value(QStringLiteral("IsExpanded")));
		}

		const QJsonArray toolsList = directory.value(QStringLiteral("ToolsList")).toArray();
		for (const QJsonValue& row : toolsList) {
			const QString toolName = rowToolName(row);
			if (toolName.isEmpty())
				continue;
			// 归属：同一个工具出现在多个目录里时，**第一个**认领它的目录说话
			// （界面上一个工具只能有一行）；可见性相反，按插件语义"后写覆盖先写"。
			if (!directoryOfTool.contains(toolName))
				directoryOfTool.insert(toolName, key);
			storedVisible.insert(toolName, rowVisible(row));
		}
	}

	// 2) 没有存储配置时，插件随 tools 一起返回的 `directory` 就是初始目录：
	//    插件工具各归其插件，原版工具按功能分组。没有该字段时才回退 Default。
	const QString defaultKey = directoryKey(defaultDirectoryName());
	int defaultIndex = -1;
	if (!autoGroup) {
		defaultIndex = ensureDirectory(defaultDirectoryName());
	}

	// 3) 工具归位：顺序跟着插件返回的目录（名字 → 描述 → 参数），
	//    这样同一个工具在界面上总是同一行。
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
	// 可见的只写工具名；隐藏的写 { ToolName, IsVisible:"False" }
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
		// IsExpanded 是**筛选语义**（"False" = 插件整组隐藏），界面上的展开/收起
		// 不写在这里 —— 所以这里原样带回读到的值，默认新建的目录写 "True"。
		payload.insert(QStringLiteral("IsExpanded"),
			directory.expanded ? QStringLiteral("True") : QStringLiteral("False"));
		payload.insert(QStringLiteral("DirectoryName"),
			directory.name.isEmpty() ? defaultDirectoryName() : directory.name);
		// 描述只在配置文件里存在（界面不能改它），有就原样带回
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
	// 只在名单非空时才写这一项：保持初始版配置文件最小（只有工具名）
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