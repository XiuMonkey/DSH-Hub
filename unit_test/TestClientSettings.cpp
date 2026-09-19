#include "TestClientSettings.h"

#include "ClientSettings.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTest>

namespace
{
	QString appearanceFilePath()
	{
		return ClientSettings::filePath(AppearanceSetting::fileName());
	}

	// 直接往设置文件里写原始字节，用于构造"用户手改坏了"的场面
	bool writeRaw(const QByteArray& text)
	{
		QFile file(appearanceFilePath());
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
			return false;
		return file.write(text) == text.size();
	}

	// 读回磁盘上的原始 JSON 对象（绕过 AppearanceSetting 的默认值包装）
	QJsonObject rawObject()
	{
		QFile file(appearanceFilePath());
		if (!file.open(QIODevice::ReadOnly))
			return {};
		return QJsonDocument::fromJson(file.readAll()).object();
	}

	void removeSettingsFile()
	{
		QFile::remove(appearanceFilePath());
	}
}

void TestClientSettings::initTestCase()
{
	QVERIFY(m_dir.isValid());

	// 把设置根指到临时目录：必须在任何 ClientSettings 调用之前生效
	qputenv("DSHHUB_CLIENT_SETTING_DIR", m_dir.path().toLocal8Bit());

	QCOMPARE(QDir::cleanPath(ClientSettings::dir()), QDir::cleanPath(m_dir.path()));
}

void TestClientSettings::cleanupTestCase()
{
	qunsetenv("DSHHUB_CLIENT_SETTING_DIR");
}

void TestClientSettings::init()
{
	removeSettingsFile();
}

void TestClientSettings::settingsDirFollowsEnvOverride()
{
	QCOMPARE(QDir::cleanPath(ClientSettings::dir()), QDir::cleanPath(m_dir.path()));
}

void TestClientSettings::filePathBuiltUnderSettingsDir()
{
	const QString name = QStringLiteral("AppearanceSetting.json");
	QCOMPARE(ClientSettings::filePath(name),
		ClientSettings::dir() + QLatin1Char('/') + name);
}

void TestClientSettings::languageDefaultsToSystemWhenFileMissing()
{
	QVERIFY(!QFile::exists(appearanceFilePath()));
	// 空串 = 跟随系统（沿用原有的语义约定）
	QVERIFY(AppearanceSetting::languageCode().isEmpty());
}

void TestClientSettings::languageRoundTrip()
{
	AppearanceSetting::setLanguageCode(QStringLiteral("en"));
	QCOMPARE(AppearanceSetting::languageCode(), QStringLiteral("en"));

	AppearanceSetting::setLanguageCode(QStringLiteral("zh_CN"));
	QCOMPARE(AppearanceSetting::languageCode(), QStringLiteral("zh_CN"));
}

void TestClientSettings::languageSystemIsStoredAsReadableWord()
{
	AppearanceSetting::setLanguageCode(QStringLiteral("en"));
	AppearanceSetting::setLanguageCode(QString());

	// 代码里是空串，文件里要写成可读的 "system"（用户会手改这个文件）
	QVERIFY(AppearanceSetting::languageCode().isEmpty());
	QCOMPARE(rawObject().value(QStringLiteral("language")).toString(),
		QStringLiteral("system"));
}

void TestClientSettings::languageTrimmed()
{
	AppearanceSetting::setLanguageCode(QStringLiteral("  zh_CN  "));
	QCOMPARE(AppearanceSetting::languageCode(), QStringLiteral("zh_CN"));
}

void TestClientSettings::themeDefaultsToSystemWhenFileMissing()
{
	QVERIFY(!QFile::exists(appearanceFilePath()));
	QCOMPARE(AppearanceSetting::themeMode(), AppearanceSetting::ThemeMode::System);
}

void TestClientSettings::themeRoundTrip()
{
	AppearanceSetting::setThemeMode(AppearanceSetting::ThemeMode::Dark);
	QCOMPARE(AppearanceSetting::themeMode(), AppearanceSetting::ThemeMode::Dark);

	AppearanceSetting::setThemeMode(AppearanceSetting::ThemeMode::Light);
	QCOMPARE(AppearanceSetting::themeMode(), AppearanceSetting::ThemeMode::Light);

	AppearanceSetting::setThemeMode(AppearanceSetting::ThemeMode::System);
	QCOMPARE(AppearanceSetting::themeMode(), AppearanceSetting::ThemeMode::System);
}

void TestClientSettings::themeNameRoundTrip()
{
	using AppearanceSetting::ThemeMode;

	const ThemeMode modes[] = { ThemeMode::System, ThemeMode::Light, ThemeMode::Dark };
	for (ThemeMode mode : modes) {
		const QString name = AppearanceSetting::themeModeName(mode);
		QCOMPARE(AppearanceSetting::themeModeFromName(name), mode);
	}

	// 与文件里的字面量对齐（写错了会让用户手改的配置失效）
	QCOMPARE(AppearanceSetting::themeModeName(ThemeMode::System), QStringLiteral("system"));
	QCOMPARE(AppearanceSetting::themeModeName(ThemeMode::Light), QStringLiteral("light"));
	QCOMPARE(AppearanceSetting::themeModeName(ThemeMode::Dark), QStringLiteral("dark"));
}

void TestClientSettings::themeNameIsCaseInsensitive()
{
	QCOMPARE(AppearanceSetting::themeModeFromName(QStringLiteral("DARK")),
		AppearanceSetting::ThemeMode::Dark);
	QCOMPARE(AppearanceSetting::themeModeFromName(QStringLiteral("Light")),
		AppearanceSetting::ThemeMode::Light);
	QCOMPARE(AppearanceSetting::themeModeFromName(QStringLiteral("  DARK ")),
		AppearanceSetting::ThemeMode::Dark);
}

void TestClientSettings::themeUnknownValueFallsBackToSystem()
{
	QVERIFY(writeRaw("{\"theme\": \"neon\"}"));
	QCOMPARE(AppearanceSetting::themeMode(), AppearanceSetting::ThemeMode::System);

	QVERIFY(writeRaw("{\"theme\": \"\"}"));
	QCOMPARE(AppearanceSetting::themeMode(), AppearanceSetting::ThemeMode::System);
}

void TestClientSettings::malformedJsonFallsBackToDefaults()
{
	// 截断的半截 JSON（像写到一半断电）
	QVERIFY(writeRaw("{\"language\": \"en\", \"theme\": "));

	// 读出来是空对象，调用方据此走默认值（不会抛、不会崩）
	QVERIFY(ClientSettings::read(AppearanceSetting::fileName()).isEmpty());
	QVERIFY(AppearanceSetting::languageCode().isEmpty());
	QCOMPARE(AppearanceSetting::themeMode(), AppearanceSetting::ThemeMode::System);
}

void TestClientSettings::jsonRootNotAnObjectFallsBackToDefaults()
{
	QVERIFY(writeRaw("[1, 2, 3]"));

	QVERIFY(ClientSettings::read(AppearanceSetting::fileName()).isEmpty());
	QVERIFY(AppearanceSetting::languageCode().isEmpty());
	QCOMPARE(AppearanceSetting::themeMode(), AppearanceSetting::ThemeMode::System);
}

void TestClientSettings::writeCreatesMissingDirectory()
{
	// 整个设置目录都不存在时，首次写入要自己把目录建出来
	QVERIFY(QDir(m_dir.path()).removeRecursively());
	QVERIFY(!QDir(m_dir.path()).exists());

	AppearanceSetting::setLanguageCode(QStringLiteral("en"));

	QVERIFY(QDir(m_dir.path()).exists());
	QVERIFY(QFile::exists(appearanceFilePath()));
	QCOMPARE(AppearanceSetting::languageCode(), QStringLiteral("en"));
}

void TestClientSettings::ensureFileWritesTemplate()
{
	QVERIFY(!QFile::exists(appearanceFilePath()));

	AppearanceSetting::ensureFile();

	QVERIFY(QFile::exists(appearanceFilePath()));
	QCOMPARE(rawObject().value(QStringLiteral("language")).toString(),
		QStringLiteral("system"));
	QCOMPARE(rawObject().value(QStringLiteral("theme")).toString(),
		QStringLiteral("system"));
}

void TestClientSettings::ensureFileNeverOverwritesExistingValues()
{
	AppearanceSetting::setLanguageCode(QStringLiteral("en"));
	AppearanceSetting::setThemeMode(AppearanceSetting::ThemeMode::Dark);

	AppearanceSetting::ensureFile(); // 每次启动都会调，绝不能把用户的选择抹掉

	QCOMPARE(AppearanceSetting::languageCode(), QStringLiteral("en"));
	QCOMPARE(AppearanceSetting::themeMode(), AppearanceSetting::ThemeMode::Dark);
}