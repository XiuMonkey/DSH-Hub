#pragma once

// ------------------------------------------------------------------
// SettingsStore.h
// ------------------------------------------------------------------
// 本地 QSettings 持久化的统一入口：键名只允许出现在这里，避免在 UI / core
// 各处散落字符串字面量。
//   - server/url ：自定义 DSH 服务地址（空 = 使用内置服务）
//
// 注 1：**界面语言、主题、默认 Agent 预设都不在这里**。
//   - 语言与主题属于"外观"，和这份客户端绑定、需要用户看得见也能手改，
//     落在运行目录的 ClientSetting/AppearanceSetting.json（见 ClientSettings.h）；
//   - 默认 Agent 预设属于**服务端**设置：读 agentPresets/list 的 isDefault、
//     写 settings/update 的 "agent-presets" namespace，落盘到服务端的
//     settings.yaml（见 AgentPresetService）。客户端刻意不留副本 ——
//     之前这里存过一份，但写入路径因缺 organizationName 被静默丢弃，
//     反倒让"改模式没作用"看起来像服务端的问题，已整体移除。
// 注 2：QSettings 的定位依赖 QCoreApplication 的 organizationName +
//   applicationName，本文件的使用方必须保证二者已设置（当前 main.cpp 尚未设置，
//   所以 server/url 这条也存不住，见 ServerManager 的待办）。
// ------------------------------------------------------------------

#include <QString>

namespace SettingsStore
{
	// 自定义 DSH 服务地址；未设置时返回空串
	QString serverUrl();

	// 写入服务地址；传入空串表示删除该键（回到内置服务）
	void setServerUrl(const QString& url);
}
