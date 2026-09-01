// ------------------------------------------------------------------
// UnitTestMain.cpp
// ------------------------------------------------------------------
// 单元测试程序入口：依次运行所有测试类。
// ------------------------------------------------------------------

#include <QCoreApplication>
#include <QTest>

#include "TestDshEventParser.h"
#include "TestCodeHighlighter.h"
#include "TestHistoryManager.h"
#include "TestMarkdownPreprocess.h"
#include "TestThunk.h"

int main(int argc, char* argv[])
{
	QCoreApplication app(argc, argv);

	int status = 0;

	{
		TestDshEventParser test;
		status |= QTest::qExec(&test, argc, argv);
	}

	{
		TestCodeHighlighter test;
		status |= QTest::qExec(&test, argc, argv);
	}

	{
		TestHistoryManager test;
		status |= QTest::qExec(&test, argc, argv);
	}

	{
		TestMarkdownPreprocess test;
		status |= QTest::qExec(&test, argc, argv);
	}

	{
		TestThunk test;
		status |= QTest::qExec(&test, argc, argv);
	}

	return status;
}