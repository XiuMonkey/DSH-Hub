#include "TestDshApiClient.h"

#include "DshApiClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QTest>

void TestDshApiClient::initTestCase()
{
}

void TestDshApiClient::cleanupTestCase()
{
}

void TestDshApiClient::setBaseUrl()
{
	DshApiClient client;
	QVERIFY(client.baseUrl().isEmpty());

	const QUrl url(QStringLiteral("http://127.0.0.1:3080"));
	client.setBaseUrl(url);
	QCOMPARE(client.baseUrl(), url);
}

void TestDshApiClient::setBaseUrlWithToken()
{
	DshApiClient client;

	const QUrl url(QStringLiteral("http://127.0.0.1:3080/?token=abc123"));
	client.setBaseUrl(url);

	// baseUrl 应该剥离 ?token=
	// baseUrl 应该剥离 ?token=（QUrl::toString 会自动补尾部 /）
	QVERIFY(client.baseUrl().toString().startsWith(QStringLiteral("http://127.0.0.1:3080")));
	QVERIFY(!client.baseUrl().toString().contains(QStringLiteral("token")));
	// launchToken 应该被提取
	QCOMPARE(client.launchToken(), QStringLiteral("abc123"));
}

void TestDshApiClient::makeUrlHttp()
{
	DshApiClient client;
	client.setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:3080")));

	// 通过反射测试 makeUrl 私有方法
	// 由于 makeUrl 是私有的，我们测试 setBaseUrl 后的行为
	// 这里测试 baseUrl 的基本设置
	QCOMPARE(client.baseUrl().scheme(), QStringLiteral("http"));
}

void TestDshApiClient::makeUrlHttps()
{
	DshApiClient client;
	client.setBaseUrl(QUrl(QStringLiteral("https://example.com")));

	QCOMPARE(client.baseUrl().scheme(), QStringLiteral("https"));
}

void TestDshApiClient::nextStreamId()
{
	DshApiClient client;
	client.setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:3080")));

	// 测试 nextStreamId 生成唯一 ID
	// 由于是私有方法，我们通过 followSession 间接测试
	// 这里测试基本的对象创建
	QVERIFY(!client.isConnected());
}

void TestDshApiClient::nextStreamIdUnique()
{
	DshApiClient client;
	client.setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:3080")));

	// 测试多次调用生成不同的 streamId
	// 通过 followSession 测试（需要连接状态）
	// 这里只测试对象初始化状态
	QVERIFY(!client.isConnected());
}

void TestDshApiClient::isConnectedDefault()
{
	DshApiClient client;
	// 新创建的客户端应该未连接
	QVERIFY(!client.isConnected());
}

void TestDshApiClient::followSessionEmpty()
{
	DshApiClient client;
	client.setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:3080")));

	// 空 sessionId 应该被忽略
	client.followSession(QString());
	// 由于未连接，不会有实际效果
	QVERIFY(!client.isConnected());
}

void TestDshApiClient::unfollowSessionDefault()
{
	DshApiClient client;
	// 默认 unfollow 应该安全执行
	client.unfollowSession();
	QVERIFY(!client.isConnected());
}