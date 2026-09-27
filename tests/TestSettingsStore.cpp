#include "TestSettingsStore.h"

#include "common/settings/ClientSettings.h"
#include "common/settings/SettingsStore.h"

#include <QFile>
#include <QTest>

// server/url 现存在 ClientSetting/ServerSetting.json（经 ClientSettings 读写）；
// 目录由 DSHHUB_CLIENT_SETTING_DIR 指到临时目录（见 TestClientSettings 同款做法）。

namespace
{
	QString serverFilePath()
	{
		return ClientSettings::filePath(SettingsStore::fileName());
	}
}

void TestSettingsStore::initTestCase()
{
	QVERIFY(m_dir.isValid());

	// 把设置根指到临时目录：必须在任何 ClientSettings 调用之前生效
	qputenv("DSHHUB_CLIENT_SETTING_DIR", m_dir.path().toLocal8Bit());
}

void TestSettingsStore::cleanupTestCase()
{
	qunsetenv("DSHHUB_CLIENT_SETTING_DIR");
}

void TestSettingsStore::init()
{
	// 每个用例从"文件不存在"的干净状态出发
	QFile::remove(serverFilePath());
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