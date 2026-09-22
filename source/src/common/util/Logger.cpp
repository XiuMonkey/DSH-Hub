#include "common/util/Logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QTextStream>
#include <QtGlobal>

namespace
{
	enum LogType
	{
		LogDebug = 0,
		LogInfo,
		LogWarning,
		LogError,
		LogFatal,
		LogTypeCount
	};

	const char* const kLevelNames[LogTypeCount] = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };
	const char* const kLevelFileNames[LogTypeCount] = { "debug.log", "info.log", "warn.log", "error.log", "fatal.log" };

	LogType toLogType(QtMsgType type)
	{
		switch (type) {
		case QtDebugMsg: return LogDebug;
		case QtInfoMsg: return LogInfo;
		case QtWarningMsg: return LogWarning;
		case QtCriticalMsg: return LogError;
		case QtFatalMsg: return LogFatal;
		}
		return LogInfo;
	}

	// 消息开头 [Tag]（或 Qt category）到模块文件夹名的映射。
	struct ModuleRule
	{
		const char* tag;
		const char* folder;
	};

	const ModuleRule kModuleRules[] = {
		// 会话
		{ "History", "session" },
		{ "MessageHost", "session" },
		{ "MessageQuery", "session" },
		{ "CacheManager", "session" },
		{ "SessionPrefetcher", "session" },
		// 模型选择
		{ "ModelSelector", "model" },
		{ "ModelList", "model" },
		{ "ModelSelection", "model" },
		// 插件市场
		{ "Market", "plugin" },
		{ "PluginMarketClient", "plugin" },
		{ "dsh-market-log", "plugin" },
		{ "market-installer", "plugin" },
		// 扩展管理
		{ "ExtensionLoader", "extension" },
		{ "ExtensionManager", "extension" },
		{ "ExtensionRegistry", "extension" },
		{ "DllCaller", "extension" },
		{ "DSH DllCaller", "extension" },
		// 客户端扩展（装进本进程的 QPlugin DLL）：宿主侧装载器与插件自身各一个标签
		{ "ClientExtension", "extension" },
		{ "QPlugin", "extension" },
		// 服务端 / 网络
		{ "ServerManager", "server" },
		{ "DshApi", "server" },
		{ "DSH Server", "server" },
		{ "DSH Pipe", "server" },
		{ "DshNamedPipeBridge", "server" },
		// UI / 界面
		{ "Translation", "ui" },
		{ "Theme", "ui" },
		{ "LayoutTrace", "ui" },
		{ "Render", "ui" },
		{ "ToolsFilter", "ui" },
		// 耗时打点
		{ "Timing", "timing" },
		// 应用整体
		{ "DSH Hub", "app" },
	};

	const int kRuleCount = static_cast<int>(sizeof(kModuleRules) / sizeof(kModuleRules[0]));
	const int kGeneralModuleIndex = kRuleCount; // 最后一个槽位给未识别的模块
	const int kModuleCount = kRuleCount + 1;

	struct ModuleTarget
	{
		const char* folder = nullptr;
		QFile* files[LogTypeCount] = {};
		QTextStream* streams[LogTypeCount] = {};
	};

	ModuleTarget g_modules[kModuleCount];
	QString g_logsDir;
	QMutex g_logMutex;

	void openModuleTarget(ModuleTarget* target, const char* folder)
	{
		target->folder = folder;
		const QString dirPath = g_logsDir + QStringLiteral("/") + QString::fromUtf8(folder);
		if (!QDir().mkpath(dirPath))
			return;

		for (int i = 0; i < LogTypeCount; ++i) {
			const QString filePath = dirPath + QStringLiteral("/") + QString::fromUtf8(kLevelFileNames[i]);

			QFile* file = new QFile(filePath);
			if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
				delete file;
				continue;
			}

			target->files[i] = file;
			target->streams[i] = new QTextStream(file);
		}
	}

	ModuleTarget* targetForFolder(const char* folder)
	{
		if (folder) {
			for (int i = 0; i < kModuleCount; ++i) {
				if (g_modules[i].folder && qstrcmp(g_modules[i].folder, folder) == 0)
					return &g_modules[i];
			}
		}
		return &g_modules[kGeneralModuleIndex];
	}

	// 从消息开头的 [Tag]（找不到则尝试 Qt category）解析模块文件夹。
	const char* moduleFolderFor(const QMessageLogContext& context, const QString& message)
	{
		if (message.startsWith(QLatin1Char('['))) {
			const int close = message.indexOf(QLatin1Char(']'));
			if (close > 1) {
				const QString tag = message.mid(1, close - 1);
				for (const ModuleRule& rule : kModuleRules) {
					if (tag.compare(QLatin1String(rule.tag), Qt::CaseSensitive) == 0)
						return rule.folder;
				}
			}
		}

		if (context.category) {
			const QString category = QString::fromUtf8(context.category);
			for (const ModuleRule& rule : kModuleRules) {
				if (category.compare(QLatin1String(rule.tag), Qt::CaseSensitive) == 0)
					return rule.folder;
			}
		}

		return nullptr;
	}

	void writeEntry(ModuleTarget* target, LogType level, const QMessageLogContext& context, const QString& message)
	{
		QTextStream* stream = target->streams[level];
		if (!stream)
			return;

		QString source;
		if (context.file) {
			source = QFileInfo(QString::fromUtf8(context.file)).fileName()
				+ QStringLiteral(":") + QString::number(context.line);
			if (context.function)
				source += QStringLiteral(" ") + QString::fromUtf8(context.function);
		}

		QString category;
		if (context.category)
			category = QString::fromUtf8(context.category);

		*stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"))
			<< QStringLiteral(" [") << QString::fromUtf8(target->folder) << QStringLiteral("] [")
			<< QLatin1String(kLevelNames[level]) << QStringLiteral("] ");

		if (!category.isEmpty())
			*stream << QStringLiteral("[") << category << QStringLiteral("] ");

		if (!source.isEmpty())
			*stream << source << QStringLiteral(": ");

		*stream << message << QStringLiteral("\n");
		stream->flush();
	}
}

static void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
	QMutexLocker locker(&g_logMutex);

	const LogType level = toLogType(type);
	const char* folder = moduleFolderFor(context, message);
	ModuleTarget* target = targetForFolder(folder);

	writeEntry(target, level, context, message);
}

void Logger::init()
{
	g_logsDir = QCoreApplication::applicationDirPath() + QStringLiteral("/logs");
	if (!QDir().mkpath(g_logsDir))
		return;

	for (int i = 0; i < kRuleCount; ++i)
		openModuleTarget(&g_modules[i], kModuleRules[i].folder);

	openModuleTarget(&g_modules[kGeneralModuleIndex], "general");

	qInstallMessageHandler(messageHandler);

	qInfo().noquote() << QStringLiteral("==== DSH Hub started ====")
		<< QStringLiteral("logs dir=") << g_logsDir;
}

QElapsedTimer& TimingLogger::timer()
{
	static QElapsedTimer t = []() {
		QElapsedTimer tm;
		tm.start();
		return tm;
		}();
	return t;
}

void TimingLogger::mark(const QString& phase)
{
	const qint64 totalMs = timer().nsecsElapsed() / 1000000;
	const qint64 deltaMs = totalMs - s_lastMs;
	qInfo().noquote() << QStringLiteral("[Timing] %1  +%2ms  (since start %3ms)")
		.arg(phase)
		.arg(deltaMs)
		.arg(totalMs);
	s_lastMs = totalMs;
}