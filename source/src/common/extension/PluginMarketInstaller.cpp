#include "common/extension/PluginMarketInstaller.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>

namespace
{
	// 自动安装的市场包版本（与 registry 快照保持一致）
	const char* const kMarketPackage = "dshmarket";
	const char* const kMarketVersion = "1.9.0";
	const char* const kMarketPackageJson =
		"/resources/server/harness/profiles/web/node_modules/dshmarket/package.json";
	const char* const kNodeRelativePath = "/resources/server/node.exe";
	const char* const kHarnessRelativePath = "/resources/server/harness";
	const char* const kPnpmEntryRelativePath =
		"/resources/server/node_modules/pnpm/bin/pnpm.cjs";
}

PluginMarketInstaller::PluginMarketInstaller(QObject* parent)
	: QObject(parent)
{
}

bool PluginMarketInstaller::isRunning() const
{
	return m_installer != nullptr;
}

bool PluginMarketInstaller::ensureInstalled(const QString& appDir)
{
	if (isRunning())
		return false;

	// 已经装过就直接返回，避免每次打开插件市场都跑一次 pnpm
	const QString marketPkg = appDir + QLatin1String(kMarketPackageJson);
	if (QFile::exists(marketPkg))
		return false;

	const QString nodePath = appDir + QLatin1String(kNodeRelativePath);
	const QString dshHome = appDir + QLatin1String(kHarnessRelativePath);

	// 确保 pnpm 可用：如果 PATH 里没有 pnpm，就创建一个本地 shim
	const QString pnpmEntry = QDir::toNativeSeparators(
		appDir + QLatin1String(kPnpmEntryRelativePath));
	const QString binDir = QDir::toNativeSeparators(dshHome + QStringLiteral("/.desktop-bin"));
	QDir().mkpath(binDir);
	const QString pnpmCmdPath = binDir + QStringLiteral("/pnpm.cmd");
	if (!QFile::exists(pnpmCmdPath)) {
		QFile shim(pnpmCmdPath);
		if (shim.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
			shim.write(QStringLiteral("@echo off\r\n\"%1\" \"%2\" %*\r\n")
				.arg(QDir::toNativeSeparators(nodePath), pnpmEntry)
				.toUtf8());
			shim.close();
		}
	}

	m_installer = new QProcess(this);
	m_installer->setProcessChannelMode(QProcess::MergedChannels);

	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	env.insert(QStringLiteral("DSH_HOME"), dshHome);

	// 把本地 pnpm shim 目录加入 PATH
	QString path = env.value(QStringLiteral("Path"));
	if (!path.isEmpty())
		path = binDir + QLatin1Char(';') + path;
	else
		path = binDir;
	env.insert(QStringLiteral("Path"), path);
	env.insert(QStringLiteral("PATH"), path);

	m_installer->setProcessEnvironment(env);

	// m_installer 每次安装新建、完成即销毁，不进登记表
	connect(m_installer, &QProcess::readyReadStandardOutput, this, [this]() {
		while (m_installer->canReadLine()) {
			const QString line = QString::fromUtf8(m_installer->readLine()).trimmed();
			if (line.isEmpty())
				continue;
			qInfo().noquote() << QStringLiteral("[market-installer]") << line;
			emit installOutput(line);
		}
		});

	const QString profileDir = QDir::toNativeSeparators(dshHome + QStringLiteral("/profiles/web"));

	connect(m_installer, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		this, [this, profileDir](int exitCode, QProcess::ExitStatus exitStatus) {
			Q_UNUSED(exitStatus)
				m_installer->deleteLater();
			m_installer = nullptr;

			qInfo().noquote() << QStringLiteral("[market-installer] finished, exitCode=") << exitCode;

			if (exitCode == 0) {
				patchProfileManifest(profileDir);
				emit installFinished(true);
			}
			else {
				emit installFinished(false);
			}
		});

	m_installer->setWorkingDirectory(profileDir);

	qInfo().noquote() << QStringLiteral("[market-installer] starting pnpm install for %1@%2")
		.arg(QLatin1String(kMarketPackage), QLatin1String(kMarketVersion));

	emit installStarted();

	// 直接使用 node 运行 pnpm，避免依赖系统 PATH 里的 pnpm
	m_installer->start(nodePath, QStringList{
		pnpmEntry,
		QStringLiteral("add"),
		QStringLiteral("--save-exact"),
		QStringLiteral("%1@%2").arg(QLatin1String(kMarketPackage), QLatin1String(kMarketVersion))
		});

	return true;
}

void PluginMarketInstaller::patchProfileManifest(const QString& profileDir) const
{
	// pnpm 只写 dependencies，需要手动把 dshmarket 加入 profile bundles
	const QString manifestPath = profileDir + QStringLiteral("/package.json");
	QFile manifestFile(manifestPath);
	if (!manifestFile.open(QIODevice::ReadOnly | QIODevice::Text))
		return;

	const QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll());
	manifestFile.close();
	if (!doc.isObject())
		return;

	QJsonObject root = doc.object();

	QJsonObject dependencies = root.value(QStringLiteral("dependencies")).toObject();
	dependencies.insert(QLatin1String(kMarketPackage), QLatin1String(kMarketVersion));
	root.insert(QStringLiteral("dependencies"), dependencies);

	QJsonObject dsh = root.value(QStringLiteral("dsh")).toObject();
	QJsonObject profile = dsh.value(QStringLiteral("profile")).toObject();
	QJsonArray bundles = profile.value(QStringLiteral("bundles")).toArray();

	bool found = false;
	for (const auto& value : bundles) {
		if (value.toString() == QLatin1String(kMarketPackage)) {
			found = true;
			break;
		}
	}
	if (!found)
		bundles.append(QLatin1String(kMarketPackage));

	profile.insert(QStringLiteral("bundles"), bundles);
	dsh.insert(QStringLiteral("profile"), profile);
	root.insert(QStringLiteral("dsh"), dsh);

	QFile out(manifestPath);
	if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
		out.close();
	}
}