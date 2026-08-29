#include "Logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QTextStream>
#include <QtGlobal>

namespace
{
	QFile* g_logFile = nullptr;
	QTextStream* g_logStream = nullptr;
	QMutex g_logMutex;
}

static void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
	QMutexLocker locker(&g_logMutex);
	if (!g_logStream)
		return;

	QString level;
	switch (type) {
	case QtDebugMsg: level = QStringLiteral("DEBUG"); break;
	case QtInfoMsg: level = QStringLiteral("INFO"); break;
	case QtWarningMsg: level = QStringLiteral("WARN"); break;
	case QtCriticalMsg: level = QStringLiteral("ERROR"); break;
	case QtFatalMsg: level = QStringLiteral("FATAL"); break;
	}

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

	*g_logStream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"))
		<< QStringLiteral(" [") << level << QStringLiteral("] ");

	if (!category.isEmpty())
		*g_logStream << QStringLiteral("[") << category << QStringLiteral("] ");

	if (!source.isEmpty())
		*g_logStream << source << QStringLiteral(": ");

	*g_logStream << message << QStringLiteral("\n");
	g_logStream->flush();
}

void Logger::init()
{
	const QString logPath = QCoreApplication::applicationDirPath() + QStringLiteral("/log.txt");

	g_logFile = new QFile(logPath);
	if (!g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
		delete g_logFile;
		g_logFile = nullptr;
		return;
	}

	g_logStream = new QTextStream(g_logFile);
	qInstallMessageHandler(messageHandler);

	qInfo().noquote() << QStringLiteral("==== DSH Hub started ====")
		<< QStringLiteral("log=") << logPath;
}