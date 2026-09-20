#include "ExtensionSystem/ExtensionLoader.h"
#include "ExtensionSystem/ClientExtension.h"
#include "common/extension/ExtensionRegistry.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QtCore/private/qzipreader_p.h>
#include <QRegularExpression>
#include <QSet>

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

		return QString();
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
	// 极简 JSON5 → QJsonObject：剥掉 // 与 /* */ 注释、去掉尾随逗号，再交给
	// QJsonDocument（它只认严格 JSON）。逐字节扫描，只在字符串外面动手。
	QJsonObject parseJson5Object(const QByteArray& raw, QString* error)
	{
		QByteArray cleaned;
		cleaned.reserve(raw.size());

		bool inString = false;
		bool escaped = false;
		bool inLineComment = false;
		bool inBlockComment = false;

		for (int i = 0; i < raw.size(); ++i) {
			const char c = raw.at(i);
			const char next = (i + 1 < raw.size()) ? raw.at(i + 1) : '\0';

			if (inLineComment) {
				if (c == '\n') {
					inLineComment = false;
					cleaned.append(c);
				}
				continue;
			}
			if (inBlockComment) {
				if (c == '*' && next == '/') {
					inBlockComment = false;
					++i;
				}
				continue;
			}
			if (inString) {
				cleaned.append(c);
				if (escaped)
					escaped = false;
				else if (c == '\\')
					escaped = true;
				else if (c == '"')
					inString = false;
				continue;
			}
			if (c == '"') {
				inString = true;
				cleaned.append(c);
				continue;
			}
			if (c == '/' && next == '/') {
				inLineComment = true;
				++i;
				continue;
			}
			if (c == '/' && next == '*') {
				inBlockComment = true;
				++i;
				continue;
			}
			cleaned.append(c);
		}

		// 尾随逗号：JSON5 允许 ,] 与 ,}，QJsonDocument 不允许。手工扫一遍
		// （Qt 6 的 QByteArray 没有 replace(QRegularExpression, ...) 了），
		// 并跳过字符串内部，免得吃掉 "a,]b" 这种字面量里的逗号。
		QByteArray trimmed;
		trimmed.reserve(cleaned.size());
		bool inStr = false;
		bool esc = false;
		for (int i = 0; i < cleaned.size(); ++i) {
			const char c = cleaned.at(i);
			if (inStr) {
				trimmed.append(c);
				if (esc)
					esc = false;
				else if (c == '\\')
					esc = true;
				else if (c == '"')
					inStr = false;
				continue;
			}
			if (c == '"') {
				inStr = true;
				trimmed.append(c);
				continue;
			}
			if (c == ',') {
				int j = i + 1;
				while (j < cleaned.size()) {
					const char w = cleaned.at(j);
					if (w == ' ' || w == '\t' || w == '\n' || w == '\r') {
						++j;
						continue;
					}
					break;
				}
				if (j < cleaned.size() && (cleaned.at(j) == '}' || cleaned.at(j) == ']'))
					continue; // 尾随逗号：丢掉
			}
			trimmed.append(c);
		}

		QJsonParseError parseError{};
		const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &parseError);
		if (parseError.error != QJsonParseError::NoError) {
			if (error)
				*error = QStringLiteral("JSON5 parse error: %1 at offset %2")
				.arg(parseError.errorString())
				.arg(parseError.offset);
			return {};
		}
		return doc.object();
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

	// regulation 里的 Type 决定走哪条路线（见头文件开头两条路线的说明）。
	if (!readDescriptor(loaded.jsonPath, &loaded, error))
		return false;

	if (ClientExtension::isClientExtensionType(loaded.type)) {
		// 客户端扩展：不碰 serverProfilePath（不写 extensions.json、不改
		// cordis.patch.yml、不重启服务端），只把 dll + regulation 落到
		// <exe>/clientExtensions/<Name>/。这里**不装载 DLL** —— 本函数跑在
		// ExtensionInstallTask 的后台线程上，装载必须由 GUI 线程做（调用方拿到
		// isClientExtension 后自己调 ClientExtension::loadOne）。
		const QString name = loaded.declaredName.isEmpty()
			? extInfo.completeBaseName()
			: loaded.declaredName;
		const QString destDir = ClientExtension::extensionDirectory()
			+ QStringLiteral("/") + name;

		if (!QDir().mkpath(destDir)) {
			m_errorString = QStringLiteral("cannot create client extension directory: %1").arg(destDir);
			if (error)
				*error = m_errorString;
			return false;
		}

		const QString destDll = destDir + QStringLiteral("/")
			+ QFileInfo(loaded.dllPath).fileName();
		const QString destJson = destDir + QStringLiteral("/regulation.json5");

		QFile::remove(destDll);
		QFile::remove(destJson);

		if (!QFile::copy(loaded.dllPath, destDll)) {
			m_errorString = QStringLiteral("cannot copy client extension dll: %1").arg(loaded.dllPath);
			if (error)
				*error = m_errorString;
			return false;
		}
		if (!QFile::copy(loaded.jsonPath, destJson)) {
			m_errorString = QStringLiteral("cannot copy client extension descriptor: %1").arg(loaded.jsonPath);
			if (error)
				*error = m_errorString;
			return false;
		}

		loaded.pluginName = name;
		loaded.dllPath = destDll;
		loaded.jsonPath = destJson;
		loaded.isClientExtension = true;
		loaded.rootDir = rootDir;
		if (out)
			*out = loaded;

		m_errorString.clear();
		qInfo().noquote() << QStringLiteral(
			"[ExtensionLoader] client extension installed: name=%1 type=%2 dll=%3")
			.arg(name, loaded.type, destDll);
		return true;
	}

	// ---- 老路线：工具扩展（以下原样）----
	if (!serverProfilePath.isEmpty() && !loaded.pluginPath.isEmpty()) {
		if (!installPlugin(loaded.pluginPath, serverProfilePath, &loaded, error))
			return false;
	}
	else if (!serverProfilePath.isEmpty() && !loaded.pluginName.isEmpty()) {
		// 纯 native DLL 扩展没有 AttachedPlugin，但也要把运行文件持久化到 extensions/<name>，
		// 否则重启后 regulation.json5 / main.dll 会随临时目录丢失。
		const QString extRoot = serverProfilePath + QStringLiteral("/extensions/") + loaded.pluginName;
		QDir().mkpath(extRoot);

		const QString srcDir = QFileInfo(loaded.dllPath).absolutePath();
		QDir sourceRoot(srcDir);
		const QFileInfoList entries = sourceRoot.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
		for (const QFileInfo& entry : entries) {
			const QString dest = extRoot + QStringLiteral("/") + entry.fileName();
			if (entry.isDir()) {
				QString copyError;
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

		loaded.jsonPath = extRoot + QStringLiteral("/regulation.json5");
		loaded.dllPath = extRoot + QStringLiteral("/main.dll");
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
		// 兼容某些打包工具生成的反斜杠路径（例如 Compress-Archive）。
		QString entryPath = info.filePath;
		entryPath.replace(QLatin1Char('\\'), QLatin1Char('/'));

		const QString destPath = QDir(destDir).filePath(entryPath);
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

		QByteArray data = reader.fileData(info.filePath);
		if (data.isEmpty() && entryPath != info.filePath)
			data = reader.fileData(entryPath);

		if (data.isEmpty() && info.size > 0) {
			outFile.close();
			m_errorString = QStringLiteral("extracted file is empty: %1").arg(info.filePath);
			if (error)
				*error = m_errorString;
			return false;
		}

		outFile.write(data);
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
		if (out->pluginName.isEmpty())
			out->pluginName = QFileInfo(rootDir).fileName();
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

	// 纯 native DLL 扩展没有 AttachedPlugin 时，用 .ext 文件名作为扩展名，
	// 否则扩展不会出现在已安装列表里，运行时文件也不会持久化到 extensions/ 下。
	if (out->pluginName.isEmpty())
		out->pluginName = QFileInfo(rootDir).fileName();

	return true;
}

bool ExtensionLoader::readDescriptor(const QString& jsonPath,
	LoadedExtension* out,
	QString* error)
{
	QFile file(jsonPath);
	if (!file.open(QIODevice::ReadOnly)) {
		m_errorString = QStringLiteral("cannot open descriptor: %1").arg(jsonPath);
		if (error)
			*error = m_errorString;
		return false;
	}
	const QByteArray raw = file.readAll();
	file.close();

	QString parseError;
	const QJsonObject root = parseJson5Object(raw, &parseError);
	if (!parseError.isEmpty()) {
		m_errorString = QStringLiteral("%1 (%2)").arg(parseError, jsonPath);
		if (error)
			*error = m_errorString;
		return false;
	}

	out->declaredName = root.value(QStringLiteral("Name")).toString();
	out->type = root.value(QStringLiteral("Type")).toString();

	qInfo().noquote() << QStringLiteral("[ExtensionLoader] descriptor Name=%1 Type=%2")
		.arg(out->declaredName.isEmpty() ? QStringLiteral("(none)") : out->declaredName,
			out->type.isEmpty() ? QStringLiteral("(none)") : out->type);
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

	// 确保 cordis.patch.yml 里有这个插件的行。行的格式与判重规则归
	// ExtensionRegistry 所有（它同时负责解析这个文件做移除与残留清理），
	// 这样"一行长什么样"只有一个地方定义。
	if (ExtensionRegistry::ensurePatchEntry(serverProfilePath, out->pluginName, out->pluginName,
		QString(), &m_errorString) == ExtensionRegistry::PatchEntryResult::Failed) {
		if (error)
			*error = m_errorString;
		return false;
	}

	return true;
}