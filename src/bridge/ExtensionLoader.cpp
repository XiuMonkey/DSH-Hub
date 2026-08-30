// ------------------------------------------------------------------
// ExtensionLoader.cpp
// ------------------------------------------------------------------
// Parses .ext extension packages (zip) and installs them into the
// DSH server profile.
//
// Standard .ext layout:
//   AttachedPlugin/
//       package.json
//       index.js
//   main.dll
//   regulation.json5
// ------------------------------------------------------------------

#include "ExtensionLoader.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtCore/private/qzipreader_p.h>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace
{
	bool copyRecursively(const QString& srcPath, const QString& dstPath, QString* error)
	{
		const QDir srcDir(srcPath);
		if (!srcDir.exists()) {
			if (error)
				*error = QStringLiteral("source directory does not exist: %1").arg(srcPath);
			return false;
		}

		QDir dstDir(dstPath);
		if (!dstDir.exists() && !dstDir.mkpath(QStringLiteral("."))) {
			if (error)
				*error = QStringLiteral("cannot create directory: %1").arg(dstPath);
			return false;
		}

		const QFileInfoList entries = srcDir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
		for (const QFileInfo& info : entries) {
			const QString target = dstPath + QStringLiteral("/") + info.fileName();
			if (info.isDir()) {
				if (!copyRecursively(info.absoluteFilePath(), target, error))
					return false;
			}
			else {
				if (QFile::exists(target) && !QFile::remove(target)) {
					if (error)
						*error = QStringLiteral("cannot remove old file: %1").arg(target);
					return false;
				}
				if (!QFile::copy(info.absoluteFilePath(), target)) {
					if (error)
						*error = QStringLiteral("cannot copy file: %1 -> %2").arg(info.absoluteFilePath(), target);
					return false;
				}
			}
		}

		return true;
	}

	bool removeRecursively(const QString& path)
	{
		QDir dir(path);
		return dir.removeRecursively();
	}

	QString readPluginName(const QString& pluginDir)
	{
		const QString packagePath = pluginDir + QStringLiteral("/package.json");
		QFile packageFile(packagePath);
		if (packageFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
			const QJsonDocument doc = QJsonDocument::fromJson(packageFile.readAll());
			if (doc.isObject()) {
				const QString name = doc.object().value(QStringLiteral("name")).toString();
				if (!name.isEmpty())
					return name;
			}
		}

		return QFileInfo(pluginDir).fileName();
	}

	QSet<QString> extractToolNames(const QString& indexJsPath)
	{
		QSet<QString> names;

		QFile file(indexJsPath);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			return names;

		const QString code = QString::fromUtf8(file.readAll());
		file.close();

		// Tool registrations look like: name: "pipe_ping"
		const QRegularExpression re(QStringLiteral("name:\\s*\"([^\"]+)\""));
		QRegularExpressionMatchIterator it = re.globalMatch(code);
		while (it.hasNext()) {
			const QRegularExpressionMatch match = it.next();
			const QString name = match.captured(1);
			if (!name.isEmpty())
				names.insert(name);
		}

		return names;
	}

	void stripUtf8BomFromJsonFiles(const QString& dirPath)
	{
		QDirIterator it(dirPath, { QStringLiteral("*.json") }, QDir::Files, QDirIterator::Subdirectories);
		while (it.hasNext()) {
			const QString filePath = it.next();

			QFile file(filePath);
			if (!file.open(QIODevice::ReadOnly))
				continue;

			QByteArray data = file.readAll();
			file.close();

			if (data.startsWith("\xEF\xBB\xBF")) {
				QByteArray cleaned = data.mid(3);
				if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
					file.write(cleaned);
					file.close();
				}
			}
		}
	}
} // namespace

bool ExtensionLoader::loadAndInstall(const QString& extFilePath,
	const QString& serverProfilePath,
	LoadedExtension* out,
	QString* error)
{
	const QFileInfo extInfo(extFilePath);
	qInfo().noquote() << QStringLiteral("[ExtensionLoader] loadAndInstall start: %1").arg(extFilePath);
	QElapsedTimer installTimer;
	installTimer.start();
	if (!extInfo.exists() || extInfo.suffix().compare(QStringLiteral("ext"), Qt::CaseInsensitive) != 0) {
		m_errorString = QStringLiteral("invalid extension file: %1").arg(extFilePath);
		if (error)
			*error = m_errorString;
		return false;
	}

	const QString rootDir = QDir::temp().filePath(
		QStringLiteral("DSHHubExtensions/") + extInfo.completeBaseName());

	if (QDir(rootDir).exists() && !removeRecursively(rootDir)) {
		m_errorString = QStringLiteral("cannot clear old extension directory: %1").arg(rootDir);
		if (error)
			*error = m_errorString;
		return false;
	}

	if (!extractArchive(extFilePath, rootDir, error))
		return false;

	LoadedExtension loaded;
	if (!findFiles(rootDir, &loaded, error))
		return false;

	if (!serverProfilePath.isEmpty() && !loaded.pluginPath.isEmpty()) {
		if (!installPlugin(loaded.pluginPath, serverProfilePath, &loaded, error))
			return false;
	}

	loaded.rootDir = rootDir;
	if (out)
		*out = loaded;

	m_errorString.clear();
	qInfo().noquote() << QStringLiteral("[ExtensionLoader] loadAndInstall finished, total=%1 ms").arg(installTimer.elapsed());
	return true;
}

QString ExtensionLoader::errorString() const
{
	return m_errorString;
}

bool ExtensionLoader::extractArchive(const QString& extFilePath,
	const QString& destDir,
	QString* error)
{
	qInfo().noquote() << QStringLiteral("[ExtensionLoader] extract start: %1 -> %2").arg(extFilePath, destDir);
	QElapsedTimer extractTimer;
	extractTimer.start();
	QDir().mkpath(destDir);

	QZipReader reader(extFilePath);
	if (!reader.isReadable()) {
		m_errorString = QStringLiteral("cannot open extension archive: %1").arg(extFilePath);
		if (error)
			*error = m_errorString;
		return false;
	}

	const QList<QZipReader::FileInfo> infos = reader.fileInfoList();
	for (const QZipReader::FileInfo& info : infos) {
		const QString destPath = QDir(destDir).filePath(info.filePath);
		if (info.isDir) {
			QDir().mkpath(destPath);
			continue;
		}

		QDir().mkpath(QFileInfo(destPath).absolutePath());
		QFile outFile(destPath);
		if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			m_errorString = QStringLiteral("cannot write extracted file: %1").arg(destPath);
			if (error)
				*error = m_errorString;
			return false;
		}
		outFile.write(reader.fileData(info.filePath));
		outFile.close();
	}

	qInfo().noquote() << QStringLiteral("[ExtensionLoader] extract finished, elapsed=%1 ms").arg(extractTimer.elapsed());
	return true;
}
bool ExtensionLoader::findFiles(const QString& rootDir,
	LoadedExtension* out,
	QString* error)
{
	QDir root(rootDir);

	// Standard file: regulation.json5
	QString jsonPath;
	if (QFile::exists(root.filePath(QStringLiteral("regulation.json5")))) {
		jsonPath = root.filePath(QStringLiteral("regulation.json5"));
	}
	else {
		const QStringList json5Files = root.entryList({ QStringLiteral("*.json5") }, QDir::Files);
		if (!json5Files.isEmpty())
			jsonPath = root.filePath(json5Files.first());
	}

	if (jsonPath.isEmpty()) {
		m_errorString = QStringLiteral("extension package has no regulation.json5 descriptor");
		if (error)
			*error = m_errorString;
		return false;
	}

	// Standard file: main.dll
	QString dllPath;
	if (QFile::exists(root.filePath(QStringLiteral("main.dll")))) {
		dllPath = root.filePath(QStringLiteral("main.dll"));
	}
	else {
		const QStringList dllFiles = root.entryList({ QStringLiteral("*.dll") }, QDir::Files);
		if (!dllFiles.isEmpty())
			dllPath = root.filePath(dllFiles.first());
	}

	if (dllPath.isEmpty()) {
		m_errorString = QStringLiteral("extension package has no main.dll");
		if (error)
			*error = m_errorString;
		return false;
	}

	out->jsonPath = jsonPath;
	out->dllPath = dllPath;
	qInfo().noquote() << QStringLiteral("[ExtensionLoader] found json=%1 dll=%2").arg(jsonPath, dllPath);

	// Standard plugin directory: AttachedPlugin
	const QString attachedDir = root.filePath(QStringLiteral("AttachedPlugin"));
	if (QDir(attachedDir).exists()
		&& QDir(attachedDir).exists(QStringLiteral("package.json"))
		&& QDir(attachedDir).exists(QStringLiteral("index.js"))) {
		out->pluginPath = attachedDir;
		out->pluginName = readPluginName(attachedDir);
		return true;
	}

	// Fallback: any subdirectory with package.json + index.js
	QDirIterator it(rootDir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		const QString dirPath = it.next();
		const QDir dir(dirPath);
		if (dir.exists(QStringLiteral("package.json")) && dir.exists(QStringLiteral("index.js"))) {
			out->pluginPath = dirPath;
			out->pluginName = readPluginName(dirPath);
			break;
		}
	}

	return true;
}

bool ExtensionLoader::installPlugin(const QString& pluginPath,
	const QString& serverProfilePath,
	LoadedExtension* out,
	QString* error)
{
	qInfo().noquote() << QStringLiteral("[ExtensionLoader] installPlugin start: %1 -> %2").arg(pluginPath, serverProfilePath);
	const QDir profileDir(serverProfilePath);
	const QString nodeModulesPath = profileDir.filePath(QStringLiteral("node_modules"));
	if (!QDir(nodeModulesPath).exists()) {
		m_errorString = QStringLiteral("server profile node_modules not found: %1").arg(nodeModulesPath);
		if (error)
			*error = m_errorString;
		return false;
	}

	if (out->pluginName.isEmpty()) {
		m_errorString = QStringLiteral("extension plugin has no name");
		if (error)
			*error = m_errorString;
		return false;
	}

	const QString destPluginPath = nodeModulesPath + QStringLiteral("/") + out->pluginName;

	const QString extRoot = profileDir.filePath(QStringLiteral("extensions"))
		+ QStringLiteral("/") + out->pluginName;
	QDir().mkpath(extRoot);
	// Persist runtime files (main.dll / regulation.json5 / bin / etc.) to extRoot.
	// The plugin directory (AttachedPlugin) is copied to node_modules separately below.
	const QString extRootDir = QFileInfo(out->dllPath).absolutePath();
	const QString excludedPluginDir = QFileInfo(out->pluginPath).fileName();
	QDir sourceRoot(extRootDir);
	const QFileInfoList entries = sourceRoot.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
	for (const QFileInfo& entry : entries) {
		if (entry.isDir() && entry.fileName() == excludedPluginDir)
			continue;

		QString copyError;
		const QString dest = extRoot + QStringLiteral("/") + entry.fileName();
		if (entry.isDir()) {
			if (!copyRecursively(entry.absoluteFilePath(), dest, &copyError)) {
				m_errorString = QStringLiteral("cannot copy extension runtime directory: %1").arg(copyError);
				if (error)
					*error = m_errorString;
				return false;
			}
		}
		else {
			QFile::remove(dest);
			if (!QFile::copy(entry.absoluteFilePath(), dest)) {
				m_errorString = QStringLiteral("cannot copy extension runtime file: %1").arg(entry.absoluteFilePath());
				if (error)
					*error = m_errorString;
				return false;
			}
		}
	}

	// If another already-installed plugin exposes the same tool names, skip
	// installing this plugin to avoid duplicate tool registration crashes.
	const QString newIndexPath = pluginPath + QStringLiteral("/index.js");
	const QSet<QString> newToolNames = extractToolNames(newIndexPath);
	if (!newToolNames.isEmpty()) {
		QSet<QString> existingToolNames;
		QDirIterator it(nodeModulesPath, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
		while (it.hasNext()) {
			const QString dirPath = it.next();
			const QString dirName = QFileInfo(dirPath).fileName();
			if (dirName == QStringLiteral(".pnpm") || dirName == out->pluginName)
				continue;

			const QString indexFile = dirPath + QStringLiteral("/index.js");
			if (QFile::exists(indexFile))
				existingToolNames.unite(extractToolNames(indexFile));
		}

		for (const QString& toolName : newToolNames) {
			if (existingToolNames.contains(toolName)) {
				// Skip the duplicate plugin; the DLL part of the extension is still usable.
				return true;
			}
		}
	}
	if (QDir(destPluginPath).exists() && !removeRecursively(destPluginPath)) {
		m_errorString = QStringLiteral("cannot clear old plugin directory: %1").arg(destPluginPath);
		if (error)
			*error = m_errorString;
		return false;
	}
	qInfo().noquote() << QStringLiteral("[ExtensionLoader] copying plugin to: %1").arg(destPluginPath);

	if (!copyRecursively(pluginPath, destPluginPath, error)) {
		m_errorString = QStringLiteral("cannot install plugin: %1").arg(error ? *error : QString());
		if (error)
			*error = m_errorString;
		return false;
	}

	// Some extension packages ship UTF-8 BOM in package.json, which breaks
	// Node's JSON.parse and typert-loader. Strip BOM from copied JSON files.
	stripUtf8BomFromJsonFiles(destPluginPath);

	// Normalize the plugin's internal export name to match package.json name.
	{
		const QString indexFilePath = destPluginPath + QStringLiteral("/index.js");
		QFile indexFile(indexFilePath);
		if (indexFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
			QString code = QString::fromUtf8(indexFile.readAll());
			indexFile.close();

			const QRegularExpression namePattern(
				QStringLiteral("export\\s+const\\s+name\\s*=\\s*['\"][^'\"]*['\"];"));
			if (namePattern.isValid()) {
				code.replace(namePattern,
					QStringLiteral("export const name = \"%1\";").arg(out->pluginName));

				if (indexFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
					indexFile.write(code.toUtf8());
					indexFile.close();
				}
			}
		}
	}

	// Make sure cordis.patch.yml has an entry for the plugin.
	const QString patchPath = profileDir.filePath(QStringLiteral("cordis.patch.yml"));
	QFile patchFile(patchPath);
	if (!patchFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
		m_errorString = QStringLiteral("cannot open cordis.patch.yml: %1").arg(patchPath);
		if (error)
			*error = m_errorString;
		return false;
	}

	QString patchText = QString::fromUtf8(patchFile.readAll());
	patchFile.close();

	const QString marker = QStringLiteral("name: '%1'").arg(out->pluginName);
	if (!patchText.contains(marker)) {
		patchText += QStringLiteral("\n- insert:\n    - id: %1\n      name: '%1'\n").arg(out->pluginName);

		if (!patchFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
			m_errorString = QStringLiteral("cannot write cordis.patch.yml: %1").arg(patchPath);
			if (error)
				*error = m_errorString;
			return false;
		}
		patchFile.write(patchText.toUtf8());
		patchFile.close();
	}

	return true;
}