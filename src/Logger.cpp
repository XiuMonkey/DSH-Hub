#include "Logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QTextStream>
#include <QtGlobal>

namespace
{
QFile *g_logFile = nullptr;
QTextStream *g_logStream = nullptr;
QMutex g_logMutex;
}

static void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Q_UNUSED(context)

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

    *g_logStream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"))
                 << QStringLiteral(" [") << level << QStringLiteral("] ")
                 << message << QStringLiteral("\n");
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

    qInfo().noquote() << QStringLiteral("==== DSH Hub started ====");
}
