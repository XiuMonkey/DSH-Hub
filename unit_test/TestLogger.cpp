#include "TestLogger.h"

#include "Logger.h"

#include <QTest>

void TestLogger::initTestCase()
{
}

void TestLogger::cleanupTestCase()
{
}

void TestLogger::timingLoggerMark()
{
	// 测试 TimingLogger 的基本功能
	// 第一次调用应该不崩溃
	TimingLogger::mark(QStringLiteral("test-phase-1"));
	QVERIFY(true); // 如果没有崩溃就通过
}

void TestLogger::timingLoggerMultipleMarks()
{
	// 测试多次调用
	TimingLogger::mark(QStringLiteral("test-phase-1"));
	TimingLogger::mark(QStringLiteral("test-phase-2"));
	TimingLogger::mark(QStringLiteral("test-phase-3"));
	QVERIFY(true); // 如果没有崩溃就通过
}