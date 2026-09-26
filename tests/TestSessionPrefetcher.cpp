#include "TestSessionPrefetcher.h"

#include "network/DshApiClient.h"
#include "network/SessionPrefetcher.h"

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
}

void TestSessionPrefetcher::setApi()
{
	SessionPrefetcher prefetcher;
	DshApiClient api;

	prefetcher.setApi(&api);
}