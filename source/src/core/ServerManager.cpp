#include "core/ServerManager.h"
#include "core/ConnectionManager.h"
#include "common/extension/ExtensionRegistry.h"
#include "common/settings/SettingsStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpSocket>
#include <QUrlQuery>

namespace
{
	QUrl storedServerUrl()
	{
		const QString envUrl = qEnvironmentVariable("DSH_SERVER_URL").trimmed();
		if (!envUrl.isEmpty()) {
			const QUrl url(envUrl);
			if (url.isValid() && !url.host().isEmpty())
				return url;
		}

		const QString savedUrl = SettingsStore::serverUrl();
		if (!savedUrl.isEmpty()) {
			const QUrl url(savedUrl);
			if (url.isValid() && !url.host().isEmpty())
				return url;
		}

		return QUrl();
	}

	// 内置插件台账 .dsh-hub-builtin.json：revision 是插件源 SHA1 前缀，内容变了下次启动重装；
	// 台账里有记录但被手工删掉的插件不重装
	QString builtinMarkerPath(const QString& profileDir)
	{
		return profileDir + QStringLiteral("/.dsh-hub-builtin.json");
	}

	QString builtinSourceRevision(const QString& sourceDir)
	{
		QCryptographicHash hash(QCryptographicHash::Sha1);
		const QDir dir(sourceDir);
		QStringList names = dir.entryList(QDir::Files, QDir::Name);
		names.sort();

		for (const QString& name : names) {
			QFile file(sourceDir + QLatin1Char('/') + name);
			if (!file.open(QIODevice::ReadOnly))
				return QString();

			hash.addData(name.toUtf8());
			hash.addData(file.readAll());
			file.close();
		}

		return QString::fromLatin1(hash.result().toHex().left(12));
	}

	QString recordedBuiltinRevision(const QString& profileDir, const QString& pluginName)
	{
		QFile file(builtinMarkerPath(profileDir));
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			return QString();

		const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
		file.close();
		if (!doc.isObject())
			return QString();

		return doc.object().value(QStringLiteral("plugins")).toObject()
			.value(pluginName).toObject().value(QStringLiteral("revision")).toString();
	}

	void recordBuiltinRevision(const QString& profileDir, const QString& pluginName, const QString& revision)
	{
		QJsonObject plugins;
		QFile existing(builtinMarkerPath(profileDir));
		if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
			const QJsonDocument doc = QJsonDocument::fromJson(existing.readAll());
			existing.close();
			if (doc.isObject())
				plugins = doc.object().value(QStringLiteral("plugins")).toObject();
		}

		QJsonObject entry = plugins.value(pluginName).toObject();
		entry.insert(QStringLiteral("revision"), revision);
		entry.insert(QStringLiteral("installedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
		plugins.insert(pluginName, entry);

		QJsonObject root;
		root.insert(QStringLiteral("comment"),
			QStringLiteral("DSH Hub builtin plugins: revision of the embedded source that is currently installed."));
		root.insert(QStringLiteral("plugins"), plugins);

		QFile out(builtinMarkerPath(profileDir));
		if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
			out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
			out.close();
		}
	}

	// qrc 插件目录逐文件写到目标目录；.json 落盘前去掉 UTF-8 BOM（Node 的 JSON.parse 会被噎住）
	bool materializePluginFiles(const QString& sourceDir, const QString& destDir, QString* error)
	{
		const QDir srcDir(sourceDir);
		if (!srcDir.exists()) {
			if (error)
				*error = QStringLiteral("source directory does not exist: %1").arg(sourceDir);
			return false;
		}

		QDir dest(destDir);
		if (dest.exists() && !dest.removeRecursively()) {
			if (error)
				*error = QStringLiteral("cannot clear old plugin directory: %1").arg(destDir);
			return false;
		}
		if (!QDir().mkpath(destDir)) {
			if (error)
				*error = QStringLiteral("cannot create directory: %1").arg(destDir);
			return false;
		}

		const QStringList names = srcDir.entryList(QDir::Files, QDir::Name);
		for (const QString& name : names) {
			QFile source(sourceDir + QLatin1Char('/') + name);
			if (!source.open(QIODevice::ReadOnly)) {
				if (error)
					*error = QStringLiteral("cannot read resource: %1").arg(source.fileName());
				return false;
			}
			QByteArray data = source.readAll();
			source.close();

			if (name.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)
				&& data.startsWith("\xEF\xBB\xBF"))
				data.remove(0, 3);

			QFile target(destDir + QLatin1Char('/') + name);
			if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				if (error)
					*error = QStringLiteral("cannot write: %1").arg(target.fileName());
				return false;
			}
			target.write(data);
			target.close();
		}

		return true;
	}

	// patch 写入统一走 ensurePatchEntry：行格式与判重的唯一所有者

	// 出厂 settings.yaml；注释写成 ASCII，无 BOM 的 UTF-8 中文会被 YAML 解析器当成乱码。
	// models 写成空数组是**刻意的**：适配器在 models 缺席时会公布自带 DEFAULT_MODELS（4 条）。
	const char* const kFactorySettingsYaml =
		"# DSH Hub factory settings.\n"
		"#\n"
		"# The empty model list below is deliberate: it suppresses the adapter's\n"
		"# built-in default catalog, so every model in the list is one you added.\n"
		"# Delete the two lines below to get the adapter's shipped catalog back.\n"
		"llm-deepseek:\n"
		"  models: []\n";

	// 文件作用域静态而非成员：ServerManager 每窗口一份、切主题会新建，用成员的话切主题后的
	// start() 会把内置进程重新拉起（见 DshApiClient::Takenover）
	bool g_backendTakenover = false;
}

ServerManager::ServerManager(QObject* parent)
	: QObject(parent)
{
}

ServerManager::~ServerManager()
{
	if (m_serverProcess && m_serverProcess->state() != QProcess::NotRunning) {
		m_serverProcess->kill();
		m_serverProcess->waitForFinished(2000);
	}
}

void ServerManager::start(const QUrl& initialBaseUrl, QProcess* initialServerProcess)
{
	const QString appDir = QCoreApplication::applicationDirPath();
	m_dshHome = appDir + QStringLiteral("/resources/server/harness");

	// 接管态：不启动内置 DSH 服务端（已被扩展取代）
	// 必须在 m_dshHome 赋值之后：Settings / PluginsManager 要用 dshHome，提前 return 会踩空
	if (g_backendTakenover) {
		// 接管后仍被移交本窗口的进程一并停掉，否则会留下没人管的内置服务端
		if (initialServerProcess) {
			m_serverProcess = initialServerProcess;
			m_serverProcess->setParent(this);
			stopForTakeover();
		}
		qInfo().noquote() << QStringLiteral("[ServerManager] 后端已被客户端扩展接管：不启动内置 DSH 服务端");
		return;
	}

	QUrl baseUrl = initialBaseUrl;
	if (baseUrl.isEmpty())
		baseUrl = storedServerUrl();

	if (!baseUrl.isEmpty()) {
		if (initialServerProcess) {
			m_serverProcess = initialServerProcess;
			m_serverProcess->setParent(this);
		}
		publishBaseUrl(baseUrl);
		return;
	}

	startBundledServer();
}

void ServerManager::restart()
{
	// 接管态：重启内置服务端没有意义，这里明确变成 no-op
	if (g_backendTakenover) {
		qInfo().noquote() << QStringLiteral("[ServerManager] 后端已被客户端扩展接管：restart() 不重启内置 DSH 服务端");
		return;
	}

	m_restarting = true;

	if (m_serverProcess && m_serverProcess->state() != QProcess::NotRunning) {
		m_serverProcess->kill();
		m_serverProcess->waitForFinished(2000);
		delete m_serverProcess;
		m_serverProcess = nullptr;
	}

	const QUrl baseUrl = storedServerUrl();
	if (!baseUrl.isEmpty()) {
		publishBaseUrl(baseUrl);
		return;
	}

	startBundledServer();
}

void ServerManager::publishBaseUrl(const QUrl& url)
{
	m_baseUrl = url;
	m_restarting = false;
	emit baseUrlReady(m_baseUrl);
}

// 接管时停掉内置 DSH 进程（不用 takeProcess()，那是移交下一个窗口）
// 幂等：未 spawn / 启动中 / 已退出 / 已停过四种切断点都必须当成功并清干净指针
void ServerManager::stopForTakeover()
{
	setTakenover(true);

	if (!m_serverProcess) {
		qInfo().noquote() << QStringLiteral("[ServerManager] 接管：当前没有内置 DSH 进程可停（还没启动 / 已移交）");
		return;
	}

	if (m_serverProcess->state() != QProcess::NotRunning) {
		qInfo().noquote() << QStringLiteral("[ServerManager] 接管：停掉内置 DSH 进程 pid=")
			<< m_serverProcess->processId();
		m_serverProcess->kill();
		m_serverProcess->waitForFinished(2000);
	}
	else {
		qInfo().noquote() << QStringLiteral("[ServerManager] 接管：内置 DSH 进程已退出，只做清理");
	}

	delete m_serverProcess;
	m_serverProcess = nullptr;
}

void ServerManager::setTakenover(bool takenover)
{
	if (g_backendTakenover == takenover)
		return;

	g_backendTakenover = takenover;
	qInfo().noquote() << QStringLiteral("[ServerManager] 后端接管标记 ->")
		<< (takenover ? QStringLiteral("已接管（不再启动/重启内置 DSH 服务端）")
			: QStringLiteral("已复位（内置 DSH 服务端可再次启动/重启）"));
}

bool ServerManager::isTakenover()
{
	return g_backendTakenover;
}

QProcess* ServerManager::takeProcess()
{
	QProcess* process = m_serverProcess;
	m_serverProcess = nullptr;
	if (process)
		process->setParent(nullptr);
	return process;
}

QString ServerManager::dshHome() const
{
	return m_dshHome;
}

bool ServerManager::isRestarting() const
{
	return m_restarting;
}

bool ServerManager::ensureFactorySettings(const QString& dshHome)
{
	if (dshHome.isEmpty())
		return false;

	const QString path = dshHome + QStringLiteral("/settings.yaml");

	// 已有就一个字都不动：可能是用户自己改过的
	if (QFile::exists(path))
		return true;

	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		qWarning().noquote() << QStringLiteral("[ServerManager] 出厂 settings.yaml 写不进去: %1").arg(path);
		return false;
	}
	file.write(kFactorySettingsYaml);
	if (!file.commit()) {
		qWarning().noquote() << QStringLiteral("[ServerManager] 出厂 settings.yaml 提交失败: %1").arg(path);
		return false;
	}

	qInfo().noquote() << QStringLiteral("[ServerManager] 已写入出厂 settings.yaml（不随附任何模型）: %1").arg(path);
	return true;
}

void ServerManager::startBundledServer()
{
	const QString appDir = QCoreApplication::applicationDirPath();

	QString nodePath = appDir + QStringLiteral("/resources/server/node.exe");
	QString entryPath = appDir + QStringLiteral("/resources/harness-node-entry.mjs");
	QString dshEntry = appDir + QStringLiteral("/resources/server/node_modules/@deepseek-ai/dsh/lib/bin.js");
	QString cwd = appDir + QStringLiteral("/resources/server/launch-root");
	QString dshHome = appDir + QStringLiteral("/resources/server/harness");

	qInfo().noquote() << QStringLiteral("[ServerManager] startBundledServer, dshHome=") << dshHome;
	m_dshHome = dshHome;

	if (!QFile::exists(nodePath) || !QFile::exists(entryPath) || !QFile::exists(dshEntry)) {
		QUrl fallback = storedServerUrl();
		if (fallback.isEmpty())
			fallback = QUrl(QStringLiteral("http://127.0.0.1:3080"));
		publishBaseUrl(fallback);
		return;
	}

	QDir().mkpath(cwd);
	QDir().mkpath(dshHome);

	// 出厂配置必须在服务端起之前就位：DSH 起完就读 settings.yaml，晚了要等下次重启
	ensureFactorySettings(dshHome);

	// profile 引用的插件包不存在时自动从 bundles 移除，避免 DSH 无法启动
	{
		const QString profileDir = dshHome + QStringLiteral("/profiles/web");
		const QString manifestPath = profileDir + QStringLiteral("/package.json");

		if (!QFile::exists(manifestPath)) {
			QDir().mkpath(profileDir);

			QJsonObject root;
			root.insert(QStringLiteral("name"), QStringLiteral("dsh-profile-web"));
			root.insert(QStringLiteral("private"), true);
			root.insert(QStringLiteral("dependencies"), QJsonObject());

			QJsonArray bundles;
			bundles.append(QStringLiteral("@deepseek-ai/dsh-base"));

			const QString marketPkg = profileDir + QStringLiteral("/node_modules/dshmarket/package.json");
			if (QFile::exists(marketPkg))
				bundles.append(QStringLiteral("dshmarket"));

			QJsonObject profile;
			profile.insert(QStringLiteral("bundles"), bundles);

			QJsonObject dsh;
			dsh.insert(QStringLiteral("profile"), profile);

			root.insert(QStringLiteral("dsh"), dsh);

			QFile out(manifestPath);
			if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
				out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
				out.close();
			}

			const QString urlPrinterDir = profileDir + QStringLiteral("/node_modules/dsh-url-printer");
			QDir().mkpath(urlPrinterDir);

			QFile urlPkg(urlPrinterDir + QStringLiteral("/package.json"));
			if (urlPkg.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
				urlPkg.write("{\n  \"name\": \"dsh-url-printer\",\n  \"version\": \"1.0.0\",\n  \"private\": true,\n  \"type\": \"module\",\n  \"main\": \"index.js\"\n}\n");
				urlPkg.close();
			}

			QFile urlIndex(urlPrinterDir + QStringLiteral("/index.js"));
			if (urlIndex.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
				// /api 需要启动令牌换来的认证 cookie，故打印机把认证 URL 打到 stdout
				urlIndex.write("import z from \"@deepseek-ai/schemastery\";\n");
				urlIndex.write("const name = \"dsh-url-printer\";\n");
				urlIndex.write("const inject = [\"webServer\", \"connection\"];\n");
				urlIndex.write("const Config = z.object({});\n");
				urlIndex.write("function apply(ctx) {\n");
				urlIndex.write("  const printUrl = () => {\n");
				urlIndex.write("    const port = ctx.webServer?.port;\n");
				urlIndex.write("    if (port === undefined) return;\n");
				urlIndex.write("    const base = `http://127.0.0.1:${port}`;\n");
				urlIndex.write("    let url = base;\n");
				urlIndex.write("    try {\n");
				urlIndex.write("      url = ctx.connection.authenticatedUrl(base);\n");
				urlIndex.write("    } catch (error) {\n");
				urlIndex.write("      console.log(`dsh web: ${base} (authenticated url unavailable: ${String(error)})`);\n");
				urlIndex.write("      return;\n");
				urlIndex.write("    }\n");
				urlIndex.write("    console.log(`dsh web: ${url}`);\n");
				urlIndex.write("  };\n");
				urlIndex.write("  const settled = ctx.get(\"loader\")?.await();\n");
				urlIndex.write("  if (settled === undefined) printUrl();\n");
				urlIndex.write("  else settled.then(() => { if (ctx.get(\"webServer\") !== undefined) printUrl(); }, () => {});\n");
				urlIndex.write("}\n");
				urlIndex.write("export { Config, apply, inject, name };\n");
				urlIndex.close();
			}

			QFile patch(profileDir + QStringLiteral("/cordis.patch.yml"));
			if (patch.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
				patch.write("# Minimal API-only profile for the DSH Hub Qt client.\n");
				patch.write("- id: hmr\n  disabled: true\n\n");
				patch.write("- insert:\n");
				// storage 等由 dsh-base 附带，插入会以 "duplicate loader entry id" 中止启动
				patch.write("    - id: workspace\n      name: '@deepseek-ai/dsh-workspace'\n\n");
				patch.write("    - id: plugin-inventory\n      name: '@deepseek-ai/dsh-host-plugin-inventory'\n\n");
				// dsh-host-apiproxy 已消失；/api 分派来自 dsh-base 的 api-gateway 行
				patch.write("    - id: api-session-controller\n      name: '@deepseek-ai/dsh-api-session-controller'\n\n");
				patch.write("    - id: api-workspace-controller\n      name: '@deepseek-ai/dsh-api-workspace-controller'\n\n");
				patch.write("    - id: api-settings-controller\n      name: '@deepseek-ai/dsh-api-settings-controller'\n\n");
				patch.write("    - id: api-workspace-files\n      name: '@deepseek-ai/dsh-api-workspace-files'\n\n");
				patch.write("    - id: cordis-host-runner\n      name: '@deepseek-ai/dsh-cordis-host-runner'\n\n");
				// shipped preset 里 tool-subagent 配了 modelSelectionSettings，要求 host 侧挂载这一行；
				// 缺它整份 preset 挂载失败，会话 resume 会以 api-session/error 结束
				patch.write("    - id: subagent-model-selection-settings\n      name: '@deepseek-ai/dsh-tool-subagent/model-selection-settings'\n\n");
				patch.write("    - id: web-startup\n      name: '@deepseek-ai/dsh-web-app/startup'\n\n");
				patch.write("    - id: webserver\n      name: '@deepseek-ai/dsh-host-webserver'\n      inject: [webStartup]\n      config:\n        host: !!js ctx.webStartup.host ?? '127.0.0.1'\n        port: !!js ctx.webStartup.port ?? 3080\n\n");
				// /api 的认证 cookie 只由本行拥有的 index 路由铸出；没有它 GET /?token=... 会 404
				patch.write("    - id: web-runtime\n      name: '@deepseek-ai/dsh-web-app'\n      inject: [webStartup]\n      config:\n        openBrowser: false\n        printUrl: true\n        surfaceContext: false\n        trustedHosts: []\n\n");
				patch.write("    - id: directory-picker-auto\n      name: '@deepseek-ai/dsh-host-directory-picker-auto'\n      inject: [webServer, loader]\n\n");
				patch.write("    - id: url-printer\n      name: 'dsh-url-printer'\n      inject: [webServer]\n\n");
				patch.write("    - id: connection\n      name: '@deepseek-ai/dsh-client-connection'\n      inject: []\n      config:\n        trustedHosts: []\n\n");
				// api-session-controller 注入本包的 fileUploads 服务；缺它整棵树激活审计失败
				patch.write("    - id: file-upload\n      name: '@deepseek-ai/dsh-client-file-upload'\n\n");
				patch.write("    - id: api-remotes\n      name: '@deepseek-ai/dsh-api-remotes'\n\n");
				patch.write("    - id: agent-presets\n      name: '@deepseek-ai/dsh-agent-presets'\n      config:\n        default: standard\n");
				patch.close();
			}

			QFile cordis(profileDir + QStringLiteral("/cordis.yml"));
			if (cordis.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
				cordis.write("[]\n");
				cordis.close();
			}

			QFile workspace(profileDir + QStringLiteral("/pnpm-workspace.yaml"));
			if (workspace.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
				workspace.write("packages:\n  - .\n\nnodeLinker: hoisted\nautoInstallPeers: false\n");
				workspace.close();
			}
		}
		else {
			QFile manifestFile(manifestPath);
			if (manifestFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
				const QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
				manifestFile.close();
				if (doc.isObject()) {
					QJsonObject root = doc.object();
					QJsonObject dsh = root.value(QStringLiteral("dsh")).toObject();
					QJsonObject profile = dsh.value(QStringLiteral("profile")).toObject();
					QJsonArray bundles = profile.value(QStringLiteral("bundles")).toArray();
					QJsonArray validBundles;
					QJsonObject dependencies = root.value(QStringLiteral("dependencies")).toObject();
					bool changed = false;

					for (const auto& value : bundles) {
						const QString name = value.toString();

						if (name == QStringLiteral("@deepseek-ai/dsh-web-app")) {
							changed = true;
							dependencies.remove(name);
							continue;
						}

						const QString profilePkg = profileDir + QStringLiteral("/node_modules/") + name
							+ QStringLiteral("/package.json");
						const QString serverPkg = appDir + QStringLiteral("/resources/server/node_modules/") + name
							+ QStringLiteral("/package.json");
						if (QFile::exists(profilePkg) || QFile::exists(serverPkg)) {
							validBundles.append(value);
						}
						else {
							changed = true;
							dependencies.remove(name);
						}
					}

					if (changed) {
						profile.insert(QStringLiteral("bundles"), validBundles);
						dsh.insert(QStringLiteral("profile"), profile);
						root.insert(QStringLiteral("dsh"), dsh);
						root.insert(QStringLiteral("dependencies"), dependencies);

						QFile out(manifestPath);
						if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
							out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
							out.close();
						}
					}
				}
			}
		}

		// 统计小灰字读的是 `sessionStats` 会话投影，注册它的行只挂在官方 web bundle 里；
		// 本 profile 是精简 API-only，必须幂等补上（profile 生成后不再重写）
		{
			const QString statsName = QStringLiteral("@deepseek-ai/dsh-session-stats");
			const bool statsAvailable =
				QFile::exists(profileDir + QStringLiteral("/node_modules/") + statsName
					+ QStringLiteral("/package.json"))
				|| QFile::exists(appDir + QStringLiteral("/resources/server/node_modules/") + statsName
					+ QStringLiteral("/package.json"));

			if (!statsAvailable) {
				qWarning().noquote() << QStringLiteral(
					"[ServerManager] 找不到 %1：输入区统计小灰字只会有 token 那一段").arg(statsName);
			}
			else {
				QString error;
				const ExtensionRegistry::PatchEntryResult row = ExtensionRegistry::ensurePatchEntry(profileDir,
					QStringLiteral("session-stats"), statsName,
					QStringLiteral("composer stats line: sessionStats projection "
						"(turns/steps + LLM/tool/ttft/decode times)"),
					&error);

				if (row == ExtensionRegistry::PatchEntryResult::Failed)
					qWarning().noquote() << QStringLiteral(
						"[ServerManager] profile patch 补不上 %1: %2").arg(statsName, error);
				else if (row == ExtensionRegistry::PatchEntryResult::Added)
					qInfo().noquote() << QStringLiteral("[ServerManager] profile patch: 已补上 %1").arg(statsName);
			}
		}
	}

	// 内置插件在服务端启动前装好（profile 生成或内置源码变更时装一次）；复用外部服务端时，
	// 新装的行要等它重启才被读到
	ensureBuiltinPlugins(dshHome + QStringLiteral("/profiles/web"));

	launchBundledServer(nodePath, entryPath, dshEntry, cwd, dshHome, 0);
}

void ServerManager::ensureBuiltinPlugins(const QString& profileDir)
{
	if (profileDir.isEmpty())
		return;

	// 内置插件清单：qrc 目录名 == node_modules 目录名 == patch 行的 id/name
	static const QStringList kBuiltinPlugins{ QStringLiteral("ToolsFilterPlugin") };

	const QString nodeModulesPath = profileDir + QStringLiteral("/node_modules");
	for (const QString& pluginName : kBuiltinPlugins) {
		const QString sourceDir = QStringLiteral(":/DSHHub/") + pluginName;
		if (!QDir(sourceDir).exists()) {
			qWarning().noquote() << QStringLiteral(
				"[ServerManager] 内置插件资源缺失（qrc 未包含？）: %1").arg(sourceDir);
			continue;
		}

		const QString revision = builtinSourceRevision(sourceDir);
		if (revision.isEmpty()) {
			qWarning().noquote() << QStringLiteral("[ServerManager] 内置插件内容不可读: %1").arg(sourceDir);
			continue;
		}

		const QString recorded = recordedBuiltinRevision(profileDir, pluginName);
		if (recorded == revision) {
			qDebug().noquote() << QStringLiteral("[ServerManager] 内置插件已装且未变更: %1 (rev %2)")
				.arg(pluginName, revision);
			continue;
		}

		QDir().mkpath(nodeModulesPath);

		QString error;
		if (!installBuiltinPlugin(profileDir, pluginName, &error)) {
			// 装不上不阻断启动：插件缺席只是那项功能不可用
			qWarning().noquote() << QStringLiteral("[ServerManager] 内置插件安装失败 %1: %2").arg(pluginName, error);
			continue;
		}

		recordBuiltinRevision(profileDir, pluginName, revision);
		qInfo().noquote() << QStringLiteral("[ServerManager] 内置插件%1: %2 (rev %3)")
			.arg(recorded.isEmpty() ? QStringLiteral("已安装") : QStringLiteral("已更新")).arg(pluginName, revision);
	}
}

bool ServerManager::installBuiltinPlugin(const QString& profileDir, const QString& pluginName, QString* error)
{
	// 就地安装：qrc 插件目录进 node_modules 并补 patch 行。不走 ExtensionLoader —— 那是用户装
	// .ext 的通路（解归档 + 必须带 main.dll）
	const QString sourceDir = QStringLiteral(":/DSHHub/") + pluginName;
	const QString destDir = profileDir + QStringLiteral("/node_modules/") + pluginName;
	if (!materializePluginFiles(sourceDir, destDir, error))
		return false;

	return ExtensionRegistry::ensurePatchEntry(profileDir, pluginName, pluginName, QString(), error)
		!= ExtensionRegistry::PatchEntryResult::Failed;
}

void ServerManager::launchBundledServer(const QString& nodePath, const QString& entryPath, const QString& dshEntry,
	const QString& cwd, const QString& dshHome, int port)
{
	const int serverPort = port > 0 ? port : 3080;
	{
		QTcpSocket probe;
		probe.connectToHost(QStringLiteral("127.0.0.1"), serverPort);
		if (probe.waitForConnected(500)) {
			qDebug().noquote() << "[ServerManager] using port:" << serverPort;
			publishBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort)));
			return;
		}
	}

	m_serverProcess = new QProcess(this);
	m_serverProcess->setProcessChannelMode(QProcess::MergedChannels);

	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	env.insert(QStringLiteral("DSH_HOME"), dshHome);

	// 让 server 进程内也能解析 pnpm：GUI 启动的进程没有 shell PATH —— 用内嵌 node 跑 pnpm，
	// 建本地 shim 并加进 PATH（已存在则不覆盖）
	{
		const QString appDir = QCoreApplication::applicationDirPath();
		const QString nodePath = QDir::toNativeSeparators(
			appDir + QStringLiteral("/resources/server/node.exe"));
		const QString pnpmEntry = QDir::toNativeSeparators(
			appDir + QStringLiteral("/resources/server/node_modules/pnpm/bin/pnpm.cjs"));
		const QString binDir = QDir::toNativeSeparators(dshHome + QStringLiteral("/.desktop-bin"));
		QDir().mkpath(binDir);
		const QString pnpmCmdPath = binDir + QStringLiteral("/pnpm.cmd");
		if (!QFile::exists(pnpmCmdPath)) {
			QFile shim(pnpmCmdPath);
			if (shim.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
				shim.write(QStringLiteral("@echo off\r\n\"%1\" \"%2\" %*\r\n")
					.arg(nodePath, pnpmEntry)
					.toUtf8());
				shim.close();
			}
		}

		QString path = env.value(QStringLiteral("Path"));
		if (!path.isEmpty())
			path = binDir + QLatin1Char(';') + path;
		else
			path = binDir;
		env.insert(QStringLiteral("Path"), path);
		env.insert(QStringLiteral("PATH"), path);
	}

	m_serverProcess->setProcessEnvironment(env);
	m_serverProcess->setWorkingDirectory(cwd);

	dshRegister("ServerManager.001", m_serverProcess, &QProcess::readyReadStandardOutput,
		this, &ServerManager::handleServerOutput);
	dshRegister("ServerManager.002", m_serverProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		this, &ServerManager::handleServerFinished);

	m_serverProcess->start(nodePath, QStringList{ entryPath, dshEntry, QStringLiteral("web"),
		QStringLiteral("--port"), QStringLiteral("0") });
	qInfo().noquote() << QStringLiteral("[ServerManager] bundled server process started");
}

void ServerManager::handleServerOutput()
{
	if (!m_serverProcess)
		return;

	while (m_serverProcess->canReadLine()) {
		const QString line = QString::fromUtf8(m_serverProcess->readLine()).trimmed();
		emit outputLine(line);

		// dsh web 打印的认证 URL 带启动令牌，客户端必须整段抓下来换认证 cookie（0.1.5 起 /api
		// 需要它，缺了 401）；裸地址说明服务端不是 0.1.5，明确报错
		QRegularExpression re(QStringLiteral("dsh web:\\s*(http://127\\.0\\.0\\.1:\\d+)(/\\?token=)?([^\\s]*)"));
		const QRegularExpressionMatch match = re.match(line);

		if (match.hasMatch()) {
			const QString printed = match.captured(1) + match.captured(2) + match.captured(3);
			const QUrl parsed(printed);
			const int port = parsed.port();
			const bool hasToken = QUrlQuery(parsed).hasQueryItem(QStringLiteral("token"));

			if (!hasToken) {
				qWarning().noquote() << QStringLiteral(
					"[ServerManager] 服务端未提供认证令牌（不是 dsh 0.1.5？）: %1").arg(printed);
				emit errorLine(qtTrId("server_auth_token_missing"));
				return;
			}

			qDebug().noquote() << QStringLiteral("[ServerManager] 服务端端口: %1（认证令牌: 有）").arg(port);
			publishBaseUrl(parsed);
			continue;
		}

		if (line.contains(QStringLiteral("error"), Qt::CaseInsensitive)
			|| line.contains(QStringLiteral("failed"), Qt::CaseInsensitive)
			|| line.contains(QStringLiteral("uncaught"), Qt::CaseInsensitive)
			|| line.contains(QStringLiteral("exception"), Qt::CaseInsensitive)) {
			emit errorLine(line);
		}
	}
}

void ServerManager::handleServerFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
	qInfo().noquote() << QStringLiteral("[ServerManager] server finished, code=") << exitCode;
	emit finished(exitCode, exitStatus);
}
