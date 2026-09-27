#pragma once

// ------------------------------------------------------------------
// TestSettingsStore.h
// ------------------------------------------------------------------
// SettingsStore（本地设置持久化）的单元测试。
// server/url 已迁到运行目录的 ClientSetting/ServerSetting.json（经 ClientSettings，
// 见 ClientSettings.h）；界面语言与主题在 AppearanceSetting.json（归 TestClientSettings），
// 默认 Agent 预设已归服务端设置文档（见 AgentPresetService），本地不再存。
// ------------------------------------------------------------------

#include <QObject>
#include <QTemporaryDir>

class TestSettingsStore : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();
	void init();

	void serverUrlDefault();
	void setServerUrl();
	void setServerUrlEmpty();
	void serverUrlTrimmed();

private:
	QTemporaryDir m_dir;
};
