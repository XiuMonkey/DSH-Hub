#pragma once

// ------------------------------------------------------------------
// TestSessionPrefetcher.h
// ------------------------------------------------------------------
// SessionPrefetcher（会话预取器）的单元测试。
// ------------------------------------------------------------------

#include <QObject>

class TestSessionPrefetcher : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	void defaultState();
	void setApi();
	void isPendingDefault();
	void forgetNonexistent();
	void isPendingAfterForget();
};
