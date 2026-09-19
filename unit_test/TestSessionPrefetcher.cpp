#include "TestSessionPrefetcher.h"

#include "DshApiClient.h"
#include "SessionPrefetcher.h"

#include <QTest>

void TestSessionPrefetcher::initTestCase()
{
}

void TestSessionPrefetcher::cleanupTestCase()
{
}

void TestSessionPrefetcher::defaultState()
{
	SessionPrefetcher prefetcher;
	// 默认应该没有 API 客户端
	// isPending 对于任何 sessionId 都应该返回 false
	QVERIFY(!prefetcher.isPending(QStringLiteral("test-session")));
}

void TestSessionPrefetcher::setApi()
{
	SessionPrefetcher prefetcher;
	DshApiClient api;

	prefetcher.setApi(&api);
	// 设置 API 后，基本状态应该不变
	QVERIFY(!prefetcher.isPending(QStringLiteral("test-session")));
}

void TestSessionPrefetcher::isPendingDefault()
{
	SessionPrefetcher prefetcher;
	// 默认没有任何在途请求
	QVERIFY(!prefetcher.isPending(QStringLiteral("session-1")));
	QVERIFY(!prefetcher.isPending(QStringLiteral("session-2")));
}

void TestSessionPrefetcher::forgetNonexistent()
{
	SessionPrefetcher prefetcher;
	// 忘记一个不存在的会话应该安全执行
	prefetcher.forget(QStringLiteral("nonexistent-session"));
	QVERIFY(!prefetcher.isPending(QStringLiteral("nonexistent-session")));
}

void TestSessionPrefetcher::isPendingAfterForget()
{
	SessionPrefetcher prefetcher;
	// forget 后应该不在 pending 中
	prefetcher.forget(QStringLiteral("test-session"));
	QVERIFY(!prefetcher.isPending(QStringLiteral("test-session")));
}