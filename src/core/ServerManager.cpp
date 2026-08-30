#include "ServerManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpSocket>

namespace
{
	QUrl storedServerUrl()
	{
		// 优先级：环境变量 > QSettings
		const QString envUrl = qEnvironmentVariable("DSH_SERVER_URL").trimmed();
		if (!envUrl.isEmpty()) {
			const QUrl url(envUrl);
			if (url.isValid() && !url.host().isEmpty())
				return url;
		}

		QSettings settings;
		const QString savedUrl = settings.value(QStringLiteral("server/url")).toString().trimmed();
		if (!savedUrl.isEmpty()) {
			const QUrl url(savedUrl);
			if (url.isValid() && !url.host().isEmpty())
				return url;
		}

		return QUrl();
	}
} // namespace

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

	QUrl baseUrl = initialBaseUrl;
	if (baseUrl.isEmpty())
		baseUrl = storedServerUrl();

	if (!baseUrl.isEmpty()) {
		// 复用已有 DSH server，不创建新 server
		m_restarting = false;
		if (initialServerProcess) {
			m_serverProcess = initialServerProcess;
			m_serverProcess->setParent(this);
		}
		m_baseUrl = baseUrl;
		emit baseUrlReady(m_baseUrl);
		return;
	}

	startBundledServer();
}

void ServerManager::restart()
{
	m_restarting = true;

	if (m_serverProcess && m_serverProcess->state() != QProcess::NotRunning) {
		m_serverProcess->kill();
		m_serverProcess->waitForFinished(2000);
		delete m_serverProcess;
		m_serverProcess = nullptr;
	}

	const QUrl baseUrl = storedServerUrl();
	if (!baseUrl.isEmpty()) {
		m_restarting = false;
		m_baseUrl = baseUrl;
		emit baseUrlReady(m_baseUrl);
		return;
	}

	startBundledServer();
}

QProcess* ServerManager::takeProcess()
{
	QProcess* process = m_serverProcess;
	m_serverProcess = nullptr;
	if (process)
		process->setParent(nullptr);
	return process;
}

void ServerManager::adoptProcess(QProcess* process)
{
	if (m_serverProcess && m_serverProcess != process) {
		m_serverProcess->kill();
		m_serverProcess->waitForFinished(2000);
		delete m_serverProcess;
	}
	m_serverProcess = process;
	if (process)
		process->setParent(this);
}

QString ServerManager::dshHome() const
{
	return m_dshHome;
}

QUrl ServerManager::baseUrl() const
{
	return m_baseUrl;
}

bool ServerManager::isRestarting() const
{
	return m_restarting;
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
		m_baseUrl = storedServerUrl();
		if (m_baseUrl.isEmpty())
			m_baseUrl = QUrl(QStringLiteral("http://127.0.0.1:3080"));
		m_restarting = false;
		emit baseUrlReady(m_baseUrl);
		return;
	}

	QDir().mkpath(cwd);
	QDir().mkpath(dshHome);

	// 如果 profile 里引用的插件包不存在（例如插件市场被删除），自动从 bundles 中移除，
	// 避免 DSH 因为缺少可选插件而无法启动。
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
				urlIndex.write("import z from \"@deepseek-ai/schemastery\";\n");
				urlIndex.write("const name = \"dsh-url-printer\";\n");
				urlIndex.write("const inject = [\"webServer\"];\n");
				urlIndex.write("const Config = z.object({});\n");
				urlIndex.write("function apply(ctx) {\n");
				urlIndex.write("  const printUrl = () => {\n");
				urlIndex.write("    const port = ctx.webServer?.port;\n");
				urlIndex.write("    if (port !== undefined) console.log(`dsh web: http://127.0.0.1:${port}`);\n");
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
				patch.write("    - id: storage\n      name: '@deepseek-ai/dsh-storage'\n\n");
				patch.write("    - id: storage-json\n      name: '@deepseek-ai/dsh-storage-json'\n      config:\n        root: !!js dshHomePath('storages')\n\n");
				patch.write("    - id: storage-domain\n      name: '@deepseek-ai/dsh-storage-domain'\n      config:\n        backend: json\n\n");
				patch.write("    - id: workspace\n      name: '@deepseek-ai/dsh-workspace'\n\n");
				patch.write("    - id: session-projection-cache\n      name: '@deepseek-ai/dsh-session-projection-cache'\n      config:\n        writeEveryEvents: 200\n        writeIntervalMs: 5000\n\n");
				patch.write("    - id: plugin-inventory\n      name: '@deepseek-ai/dsh-host-plugin-inventory'\n\n");
				patch.write("    - id: api-gateway\n      name: '@deepseek-ai/dsh-host-apiproxy'\n\n");
				patch.write("    - id: cordis-host-runner\n      name: '@deepseek-ai/dsh-cordis-host-runner'\n\n");
				patch.write("    - id: web-startup\n      name: '@deepseek-ai/dsh-web-app/startup'\n\n");
				patch.write("    - id: webserver\n      name: '@deepseek-ai/dsh-host-webserver'\n      inject: [webStartup]\n      config:\n        host: !!js ctx.webStartup.host ?? '127.0.0.1'\n        port: !!js ctx.webStartup.port ?? 3080\n\n");
				patch.write("    - id: directory-picker-auto\n      name: '@deepseek-ai/dsh-host-directory-picker-auto'\n      inject: [webServer, loader]\n\n");
				patch.write("    - id: url-printer\n      name: 'dsh-url-printer'\n      inject: [webServer]\n\n");
				patch.write("    - id: connection\n      name: '@deepseek-ai/dsh-client-connection'\n      inject: []\n      config:\n        trustedHosts: []\n\n");
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

						const QString profilePkg = profileDir + QStringLiteral("/node_modules/") + name + QStringLiteral("/package.json");
						const QString serverPkg = appDir + QStringLiteral("/resources/server/node_modules/") + name + QStringLiteral("/package.json");
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
	}

	launchBundledServer(nodePath, entryPath, dshEntry, cwd, dshHome, 0);
}

void ServerManager::launchBundledServer(const QString& nodePath,
	const QString& entryPath,
	const QString& dshEntry,
	const QString& cwd,
	const QString& dshHome,
	int port)
{
	const int serverPort = port > 0 ? port : 3080;
	{
		QTcpSocket probe;
		probe.connectToHost(QStringLiteral("127.0.0.1"), serverPort);
		if (probe.waitForConnected(500)) {
			qDebug().noquote() << "[ServerManager] using port:" << serverPort;
			m_baseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort));
			m_restarting = false;
			emit baseUrlReady(m_baseUrl);
			return;
		}
	}

	m_serverProcess = new QProcess(this);
	m_serverProcess->setProcessChannelMode(QProcess::MergedChannels);

	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	env.insert(QStringLiteral("DSH_HOME"), dshHome);
	m_serverProcess->setProcessEnvironment(env);
	m_serverProcess->setWorkingDirectory(cwd);

	connect(m_serverProcess, &QProcess::readyReadStandardOutput,
		this, &ServerManager::handleServerOutput);
	connect(m_serverProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		this, &ServerManager::handleServerFinished);

	m_serverProcess->start(nodePath, QStringList{
		entryPath, dshEntry, QStringLiteral("web"),
		QStringLiteral("--port"), QStringLiteral("0")
		});
	qInfo().noquote() << QStringLiteral("[ServerManager] bundled server process started");
}

void ServerManager::handleServerOutput()
{
	if (!m_serverProcess)
		return;

	while (m_serverProcess->canReadLine()) {
		const QString line = QString::fromUtf8(m_serverProcess->readLine()).trimmed();
		emit outputLine(line);

		QRegularExpression re(QStringLiteral("dsh web: http://127\\.0\\.0\\.1:(\\d+)"));
		const QRegularExpressionMatch match = re.match(line);

		if (match.hasMatch()) {
			const int port = match.captured(1).toInt();
			qDebug().noquote() << QStringLiteral("[ServerManager] 服务端端口: %1").arg(port);
			m_baseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port));
			m_restarting = false;
			emit baseUrlReady(m_baseUrl);
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