#pragma once

// ------------------------------------------------------------------
// TestSettingsStore.h
// ------------------------------------------------------------------
// SettingsStore（注册表里的本地设置持久化）的单元测试。
// 只覆盖仍走 QSettings 的 server/url：界面语言与主题在
// ClientSetting/AppearanceSetting.json（归 TestClientSettings），
// 默认 Agent 预设已归服务端设置文档（见 AgentPresetService），本地不再存。
// ------------------------------------------------------------------

#include <QObject>

class TestSettingsStore : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	void serverUrlDefault();
	void setServerUrl();
	void setServerUrlEmpty();
	void serverUrlTrimmed();
};
