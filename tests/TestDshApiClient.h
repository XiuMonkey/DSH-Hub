#pragma once

// ------------------------------------------------------------------
// TestDshApiClient.h
// ------------------------------------------------------------------
// DshApiClient（DSH API 客户端）的单元测试。
// ------------------------------------------------------------------

#include <QObject>

class TestDshApiClient : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	void setBaseUrl();
	void setBaseUrlWithToken();
	void makeUrlHttp();
	void makeUrlHttps();
	void nextStreamId();
	void nextStreamIdUnique();
	void isConnectedDefault();
	void followSessionEmpty();
	void unfollowSessionDefault();
};
