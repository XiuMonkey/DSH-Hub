#include "common/extension/ExtensionRegistry.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace
{
	const char* const kRegistryFileName = "/extensions.json";
	const char* const kPatchFileName = "/cordis.patch.yml";
	const char* const kInsertHeader = "- insert:";
}

ExtensionRegistry::ExtensionRegistry(const QString& serverProfilePath)
	: m_serverProfilePath(serverProfilePath)
{
}

QString ExtensionRegistry::serverProfilePath() const
{
	return m_serverProfilePath;
}

QString ExtensionRegistry::nodeModulesPath() const
{
	return m_serverProfilePath + QStringLiteral("/node_modules");
}

QString ExtensionRegistry::registryPath() const
{
	return m_serverProfilePath + QLatin1String(kRegistryFileName);
}

QString ExtensionRegistry::extensionsDir() const
{
	return m_serverProfilePath + QStringLiteral("/extensions");
}

QString ExtensionRegistry::extensionJsonPath(const QString& name) const
{
	return extensionsDir() + QStringLiteral("/") + name + QStringLiteral("/regulation.json5");
}

QString ExtensionRegistry::extensionDllPath(const QString& name) const
{
	return extensionsDir() + QStringLiteral("/") + name + QStringLiteral("/main.dll");
}

QStringList ExtensionRegistry::installedExtensions() const
{
	QStringList names;

	const QString path = registryPath();
	if (!QFile::exists(path))
		return names;

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return names;

	const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
	file.close();

	if (!doc.isArray())
		return names;

	for (const auto& value : doc.array()) {
		const QString name = value.toString();
		if (!name.isEmpty())
			names.append(name);
	}

	return names;
}

bool ExtensionRegistry::saveInstalledExtensions(const QStringList& names) const
{
	QJsonArray array;
	for (const QString& name : names)
		array.append(name);

	QFile file(registryPath());
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
		return false;

	file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
	file.close();
	return true;
}

bool ExtensionRegistry::registerInstalled(const QString& name) const
{
	if (name.isEmpty())
		return false;

	QStringList names = installedExtensions();
	if (names.contains(name))
		return false;

	names.append(name);
	saveInstalledExtensions(names);
	return true;
}

bool ExtensionRegistry::unregisterInstalled(const QString& name) const
{
	QStringList names = installedExtensions();
	const int before = names.size();
	names.removeAll(name);
	if (names.size() == before)
		return false;

	saveInstalledExtensions(names);
	return true;
}

bool ExtensionRegistry::removeExtensionDirectory(const QString& name, QString* error) const
{
	QDir dir(nodeModulesPath() + QStringLiteral("/") + name);
	if (dir.exists() && !dir.removeRecursively()) {
		if (error)
			*error = qtTrId("ext_delete_dir_failed_fmt").arg(dir.absolutePath());
		return false;
	}

	QDir extDir(extensionsDir() + QStringLiteral("/") + name);
	if (extDir.exists() && !extDir.removeRecursively()) {
		if (error)
			*error = qtTrId("ext_delete_resource_dir_failed_fmt").arg(extDir.absolutePath());
		return false;
	}

	return true;
}

QString ExtensionRegistry::patchIdLine(const QString& name)
{
	return QStringLiteral("    - id: %1").arg(name);
}

QString ExtensionRegistry::patchNameLine(const QString& name)
{
	return QStringLiteral("      name: '%1'").arg(name);
}

ExtensionRegistry::PatchEntryResult ExtensionRegistry::ensurePatchEntry(const QString& profilePath,
	const QString& id, const QString& name, const QString& comment, QString* error)
{
	if (id.isEmpty() || name.isEmpty()) {
		if (error)
			*error = QStringLiteral("a patch entry needs both an id and a name");
		return PatchEntryResult::Failed;
	}

	const QString patchPath = profilePath + QLatin1String(kPatchFileName);
	QFile file(patchPath);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		if (error)
			*error = QStringLiteral("cannot open cordis.patch.yml: %1").arg(patchPath);
		return PatchEntryResult::Failed;
	}

	QString text = QString::fromUtf8(file.readAll());
	file.close();

	if (text.contains(patchNameLine(name)))
		return PatchEntryResult::AlreadyPresent;

	// 结构必须与 removePatchEntry 解析的两行完全一致，否则那个函数删不掉它
	text += QLatin1Char('\n');
	if (!comment.isEmpty())
		text += QStringLiteral("# %1\n").arg(comment);
	text += QStringLiteral("%1\n%2\n%3\n").arg(QLatin1String(kInsertHeader), patchIdLine(id), patchNameLine(name));

	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		if (error)
			*error = QStringLiteral("cannot write cordis.patch.yml: %1").arg(patchPath);
		return PatchEntryResult::Failed;
	}
	file.write(text.toUtf8());
	file.close();
	return PatchEntryResult::Added;
}

bool ExtensionRegistry::removePatchEntry(const QString& profilePath, const QString& name)
{
	const QString patchPath = profilePath + QLatin1String(kPatchFileName);
	QFile file(patchPath);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	QString text = QString::fromUtf8(file.readAll());
	file.close();

	const QStringList lines = text.split(QLatin1Char('\n'));
	QStringList kept;
	bool removed = false;

	for (int i = 0; i < lines.size(); ++i) {
		const QString line = lines.at(i);
		const QString trimmed = line.trimmed();

		// 删掉 ExtensionLoader 写进来的标准两行条目（- id: <name> / name: '<name>'）
		if (trimmed == patchIdLine(name).trimmed() && i + 1 < lines.size()
			&& lines.at(i + 1).trimmed() == patchNameLine(name).trimmed()) {
			++i; // skip the next line too
			removed = true;
			continue;
		}

		// 也删掉孤立出现的 name 行（万一它没跟 id 成对）
		if (trimmed == patchNameLine(name).trimmed()) {
			removed = true;
			continue;
		}

		kept.append(line);
	}

	if (!removed)
		return true;

	// 收掉删空了的 "- insert:" 头。注释行不算"内容"：这个 patch 文件里会混进注释（ServerManager
	// 追加 session-stats 那行时就带了一句），只跳空行会把"后面还有内容"误判成真，空头就删不掉了
	QStringList cleaned;
	for (int i = 0; i < kept.size(); ++i) {
		if (kept.at(i).trimmed() == QLatin1String(kInsertHeader)) {
			int j = i + 1;
			while (j < kept.size()) {
				const QString next = kept.at(j).trimmed();
				if (!next.isEmpty() && !next.startsWith(QLatin1Char('#')))
					break;
				++j;
			}
			if (j >= kept.size() || kept.at(j).trimmed().startsWith(QLatin1String(kInsertHeader)))
				continue;
		}
		cleaned.append(kept.at(i));
	}
	const QString newText = cleaned.join(QLatin1Char('\n'));

	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
		return false;

	file.write(newText.toUtf8());
	file.close();
	return true;
}

ExtensionRegistry::CleanupResult ExtensionRegistry::cleanupResiduals() const
{
	CleanupResult result;

	const QString patchPath = m_serverProfilePath + QLatin1String(kPatchFileName);
	QFile patchFile(patchPath);
	if (!patchFile.open(QIODevice::ReadOnly | QIODevice::Text))
		return result;

	result.patchReadable = true;

	const QStringList lines = QString::fromUtf8(patchFile.readAll()).split(QLatin1Char('\n'));
	patchFile.close();

	const QString profileNodeModulesPath = nodeModulesPath();
	const QString serverNodeModulesPath = QDir::cleanPath(
		m_serverProfilePath + QStringLiteral("/../../../node_modules"));

	for (int i = 0; i + 1 < lines.size(); ++i) {
		const QString trimmed = lines.at(i).trimmed();
		if (!trimmed.startsWith(QStringLiteral("- id: ")))
			continue;

		const QString name = trimmed.mid(QStringLiteral("- id: ").length()).trimmed();
		if (name.isEmpty())
			continue;

		if (lines.at(i + 1).trimmed() != patchNameLine(name).trimmed())
			continue;

		// 内置插件存在于服务端根 node_modules；只有两边都不存在才视为残留
		const bool existsInProfile = QFileInfo::exists(profileNodeModulesPath + QStringLiteral("/") + name);
		const bool existsInServer = QFileInfo::exists(serverNodeModulesPath + QStringLiteral("/") + name);
		qInfo().noquote() << "[ExtensionRegistry] cleanup check"
			<< name
			<< "profile=" << existsInProfile
			<< "server=" << existsInServer;
		if (existsInProfile || existsInServer)
			continue;

		if (removePatchEntry(m_serverProfilePath, name))
			qInfo().noquote() << "[ExtensionRegistry] cleanup candidate:" << name;
		result.removed.append(name);
	}

	if (!result.removed.isEmpty()) {
		QStringList names = installedExtensions();
		for (const QString& name : result.removed)
			names.removeAll(name);
		saveInstalledExtensions(names);
	}

	return result;
}
