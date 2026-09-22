#pragma once

// 本地 QSettings 持久化的统一入口：键名只允许出现在这里，避免在 UI / core 各处散落字符串字面量（当前只有 server/url）。
// 界面语言与主题不在 QSettings（属“外观”，落在 <exe>/ClientSetting/AppearanceSetting.json，见 ClientSettings.h）；默认 Agent 预设归服务端设置，客户端刻意不留副本。
// 陷阱：QSettings 定位依赖 QCoreApplication 的 organizationName + applicationName，使用方必须保证二者已设置（main.cpp 目前未设置，所以 server/url 也存不住）。

#include <QString>

namespace SettingsStore
{
	// 自定义 DSH 服务地址；未设置时返回空串
	QString serverUrl();

	// 写入服务地址；传入空串表示删除该键（回到内置服务）
	void setServerUrl(const QString& url);
}
