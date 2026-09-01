#include "TestHistoryManager.h"

#include "CacheHistoryManager.h"

#include <QTest>

void TestHistoryManager::resetDefaults()
{
	HistoryManager manager;
	manager.setLimit(50);
	manager.setHasMore(true);
	manager.setEventCount(10);
	manager.setLoadMoreRequested(true);

	manager.reset();

	// reset() 只重置分页/加载计数。
	QCOMPARE(manager.limit(), 20);
	QVERIFY(!manager.hasMore());
	QCOMPARE(manager.eventCount(), 0);
	QVERIFY(!manager.loadMoreRequested());
}

void TestHistoryManager::limitLifecycle()
{
	HistoryManager manager;
	QCOMPARE(manager.limit(), 20);
	manager.setLimit(100);
	QCOMPARE(manager.limit(), 100);
	manager.setLimit(0);
	QCOMPARE(manager.limit(), 0);
}

void TestHistoryManager::increaseLimit()
{
	HistoryManager manager;
	QCOMPARE(manager.limit(), 20);
	manager.increaseLimit(30);
	QCOMPARE(manager.limit(), 50);
	manager.increaseLimit(-10);
	QCOMPARE(manager.limit(), 40);
}

void TestHistoryManager::hasMoreLifecycle()
{
	HistoryManager manager;
	QVERIFY(!manager.hasMore());
	manager.setHasMore(true);
	QVERIFY(manager.hasMore());
	manager.setHasMore(false);
	QVERIFY(!manager.hasMore());
}

void TestHistoryManager::eventCountLifecycle()
{
	HistoryManager manager;
	QCOMPARE(manager.eventCount(), 0);
	manager.setEventCount(12);
	QCOMPARE(manager.eventCount(), 12);
	manager.setEventCount(-1);
	QCOMPARE(manager.eventCount(), -1);
}

void TestHistoryManager::loadMoreRequestedLifecycle()
{
	HistoryManager manager;
	QVERIFY(!manager.loadMoreRequested());
	manager.setLoadMoreRequested(true);
	QVERIFY(manager.loadMoreRequested());
	manager.setLoadMoreRequested(false);
	QVERIFY(!manager.loadMoreRequested());
}