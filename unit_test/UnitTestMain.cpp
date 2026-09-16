// ------------------------------------------------------------------
// UnitTestMain.cpp
// ------------------------------------------------------------------
// 单元测试程序入口：依次运行所有测试类。
//
// 注意：本程序是 WIN32 子系统（没有控制台），而且所有测试类共用一次 argv，
// 所以命令行的 `-o file,txt` 会被后面的类覆盖，stdout 也抓不到。需要留证据时
// 设置环境变量 DSHHUB_TEST_REPORT_DIR，每个类会各写一份 report-<类名>.txt；
// 不设该变量时行为与以前完全一致（原样把 argv 传给每个 qExec）。
// ------------------------------------------------------------------

#include <QCoreApplication>
#include <QDir>
#include <QTest>

#include "TestDshEventParser.h"
#include "TestCodeHighlighter.h"
#include "TestHistoryManager.h"
#include "TestMarkdownPreprocess.h"
#include "TestModelSelection.h"
#include "TestPluginMarketModel.h"
#include "TestSessionCatalog.h"
#include "TestThunk.h"
#include "TestDshApiClient.h"
#include "TestSessionPrefetcher.h"
#include "TestSettingsStore.h"
#include "TestLogger.h"
#include "TestTranslationManager.h"
#include "TestServerManager.h"

namespace
{
	// 每个类的报告路径；未设置 DSHHUB_TEST_REPORT_DIR 时返回空 = 保持原行为。
	QString reportPathFor(const char* className)
	{
		const QByteArray dir = qgetenv("DSHHUB_TEST_REPORT_DIR");
		if (dir.isEmpty())
			return QString();
		return QDir(QString::fromLocal8Bit(dir))
			.filePath(QStringLiteral("report-%1.txt").arg(QLatin1String(className)));
	}
}

int main(int argc, char* argv[])
{
	QCoreApplication app(argc, argv);

	int status = 0;

	auto runClass = [&status, argc, argv](QObject* test, const char* className) {
		const QString report = reportPathFor(className);
		if (report.isEmpty()) {
			status |= QTest::qExec(test, argc, argv);
			return;
		}

		const QByteArray spec = report.toLocal8Bit() + ",txt";
		char* args[] = {
			argv[0],
			const_cast<char*>("-o"),
			const_cast<char*>(spec.constData()),
		};
		status |= QTest::qExec(test, 3, args);
		};

	{
		TestDshEventParser test;
		runClass(&test, "TestDshEventParser");
	}

	{
		TestCodeHighlighter test;
		runClass(&test, "TestCodeHighlighter");
	}

	{
		TestHistoryManager test;
		runClass(&test, "TestHistoryManager");
	}

	{
		TestMarkdownPreprocess test;
		runClass(&test, "TestMarkdownPreprocess");
	}

	{
		TestThunk test;
		runClass(&test, "TestThunk");
	}

	{
		TestSessionCatalog test;
		runClass(&test, "TestSessionCatalog");
	}

	{
		TestPluginMarketModel test;
		runClass(&test, "TestPluginMarketModel");
	}

	{
		TestModelSelection test;
		runClass(&test, "TestModelSelection");
	}

	{
		TestDshApiClient test;
		runClass(&test, "TestDshApiClient");
	}

	{
		TestSessionPrefetcher test;
		runClass(&test, "TestSessionPrefetcher");
	}

	{
		TestSettingsStore test;
		runClass(&test, "TestSettingsStore");
	}

	{
		TestLogger test;
		runClass(&test, "TestLogger");
	}

	{
		TestTranslationManager test;
		runClass(&test, "TestTranslationManager");
	}

	{
		TestServerManager test;
		runClass(&test, "TestServerManager");
	}

	return status;
}