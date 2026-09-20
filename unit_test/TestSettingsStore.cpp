#include "TestSettingsStore.h"

#include "common/settings/SettingsStore.h"

#include <QCoreApplication>
#include <QTest>

// 注意：界面语言已迁到运行目录的 ClientSetting/AppearanceSetting.json
//（见 ClientSettings.h / TestClientSettings）；
// 默认 Agent 预设已归服务端设置文档（见 AgentPresetService），本类不再涉及。

void TestSettingsStore::initTestCase()
{
	// QSettings 需要 organizationName + applicationName 才能定位存储路径
	QCoreApplication::setOrganizationName(QStringLiteral("DSHHubTest"));
	QCoreApplication::setApplicationName(QStringLiteral("UnitTest"));

	// 清理之前的测试数据
	SettingsStore::setServerUrl(QString());
}

void TestSettingsStore::cleanupTestCase()
{
	// 清理测试数据
	SettingsStore::setServerUrl(QString());
}

void TestSettingsStore::serverUrlDefault()
{
	// 默认应该为空
	SettingsStore::setServerUrl(QString());
	QString url = SettingsStore::serverUrl();
	QVERIFY(url.isEmpty());
}

void TestSettingsStore::setServerUrl()
{
	const QString testUrl = QStringLiteral("http://127.0.0.1:3080");
	SettingsStore::setServerUrl(testUrl);
	QCOMPARE(SettingsStore::serverUrl(), testUrl);
}

void TestSettingsStore::setServerUrlEmpty()
{
	SettingsStore::setServerUrl(QStringLiteral("http://test.com"));
	SettingsStore::setServerUrl(QString());
	QVERIFY(SettingsStore::serverUrl().isEmpty());
}

void TestSettingsStore::serverUrlTrimmed()
{
	SettingsStore::setServerUrl(QStringLiteral("  http://test.com  "));
	QCOMPARE(SettingsStore::serverUrl(), QStringLiteral("http://test.com"));
}