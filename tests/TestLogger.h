#pragma once

// ------------------------------------------------------------------
// TestLogger.h
// ------------------------------------------------------------------
// Logger / TimingLogger（日志与耗时打点）的单元测试。
// ------------------------------------------------------------------

#include <QObject>

class TestLogger : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	void timingLoggerMark();
	void timingLoggerMultipleMarks();
};
