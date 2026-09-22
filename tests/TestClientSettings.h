#pragma once

// ------------------------------------------------------------------
// TestClientSettings.h
// ------------------------------------------------------------------
// ClientSettings（运行目录里的 ClientSetting/*.json）的单元测试。
//
// 用环境变量 DSHHUB_CLIENT_SETTING_DIR 把设置根指到一个临时目录，
// 否则用例会把真实运行目录（x64\<Config>\ClientSetting\）里的用户设置改掉。
// ------------------------------------------------------------------

#include <QObject>
#include <QTemporaryDir>

class TestClientSettings : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();
	void init(); // 每个用例前清掉设置文件，保证从"干净状态"起步

	// 目录与路径
	void settingsDirFollowsEnvOverride();
	void filePathBuiltUnderSettingsDir();

	// 语言
	void languageDefaultsToSystemWhenFileMissing();
	void languageRoundTrip();
	void languageSystemIsStoredAsReadableWord();
	void languageTrimmed();

	// 主题
	void themeDefaultsToSystemWhenFileMissing();
	void themeRoundTrip();
	void themeNameRoundTrip();
	void themeNameIsCaseInsensitive();
	void themeUnknownValueFallsBackToSystem();

	// 容错：文件坏了也不能让调用方拿到垃圾
	void malformedJsonFallsBackToDefaults();
	void jsonRootNotAnObjectFallsBackToDefaults();

	// 写入行为
	void writeCreatesMissingDirectory();
	void ensureFileWritesTemplate();
	void ensureFileNeverOverwritesExistingValues();

private:
	QTemporaryDir m_dir;
};
