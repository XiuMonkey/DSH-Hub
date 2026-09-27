#pragma once

// server/url 的本地持久化统一入口：落在 <exe>/ClientSetting/ServerSetting.json，
// 经 ClientSettings 读写（QSaveFile 原子替换；DSHHUB_CLIENT_SETTING_DIR 可覆盖目录，单测靠它指临时目录）。
// 文件名与键名只允许出现在这里，避免在 UI / core 各处散落字符串字面量。
// 界面语言与主题在 AppearanceSetting.json（见 ClientSettings.h）；默认 Agent 预设归服务端设置，
// 客户端刻意不留副本。
// （历史上曾走 QSettings/注册表：因 org/app 名从未设置实际存不住，已整体迁出，
// 注册表残留键也已于 2026-09-26 清空。）

#include <QString>

namespace SettingsStore
{
	// 设置文件名（相对 ClientSettings::dir()）
	QString fileName();

	// 自定义 DSH 服务地址；未设置时返回空串
	QString serverUrl();

	// 写入服务地址；传入空串表示删除该键（回到内置服务）
	void setServerUrl(const QString& url);
}
