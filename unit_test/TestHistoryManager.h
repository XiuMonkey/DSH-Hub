#pragma once

// ------------------------------------------------------------------
// TestHistoryManager.h
// ------------------------------------------------------------------
// HistoryManager（历史加载状态管理）的单元测试。
// ------------------------------------------------------------------

#include <QObject>

class TestHistoryManager : public QObject
{
	Q_OBJECT

private slots:
	void resetDefaults();
	void setLoading();
	void limitLifecycle();
	void increaseLimit();
	void hasMoreLifecycle();
	void eventCountLifecycle();
	void loadMoreRequestedLifecycle();
};