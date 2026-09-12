#pragma once

// ------------------------------------------------------------------
// SettingsStore.h
// ------------------------------------------------------------------
// 本地 QSettings 持久化的统一入口：键名只允许出现在这里，避免在 UI / core
// 各处散落字符串字面量。
//   - server/url            ：自定义 DSH 服务地址（空 = 使用内置服务）
//   - agent/defaultPreset   ：默认 Agent 预设 id
//   - ui/language           ：界面语言代码（空 = 跟随系统）
// ------------------------------------------------------------------

#include <QString>

namespace SettingsStore
{
	// 自定义 DSH 服务地址；未设置时返回空串
	QString serverUrl();

	// 写入服务地址；传入空串表示删除该键（回到内置服务）
	void setServerUrl(const QString& url);

	// 默认 Agent 预设 id；未设置时返回空串
	QString defaultAgentPresetId();

	// 记住用户选择的 Agent 预设
	void setDefaultAgentPresetId(const QString& presetId);

	// 界面语言代码（如 "en"）；空串表示跟随系统
	QString loadLanguageCode();

	// 记住界面语言；传入空串表示跟随系统
	void saveLanguageCode(const QString& code);
}
