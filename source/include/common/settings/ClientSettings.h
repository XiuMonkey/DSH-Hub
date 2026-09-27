#pragma once

// "运行目录里的客户端设置"统一入口：<exe>/ClientSetting/<名字>Setting.json（当前有
// AppearanceSetting.json = 界面语言 + 主题；ServerSetting.json = server/url，见 SettingsStore.h）。
// 明文文件而不是注册表：用户看得见、拷得走，能随安装目录一起备份/搬移；
// 外观这类"跟着这份客户端走"的设置尤其适合。
// 陷阱：目录首次写入时按需 mkpath、可被环境变量 DSHHUB_CLIENT_SETTING_DIR 覆盖（单测靠它指向临时
// 目录）；写文件走 QSaveFile 原子替换，避免崩溃/掉电留下半截 JSON。

#include <QString>

class QJsonObject;

namespace ClientSettings
{
	// 设置目录：DSHHUB_CLIENT_SETTING_DIR（已设置且非空时优先）否则 <exe>/ClientSetting
	QString dir();

	// 设置文件的完整路径（只拼路径，不创建任何东西）
	QString filePath(const QString& fileName);

	// 读设置文件：不存在 / 读不动 / JSON 语法错 / 根不是对象 —— 一律返回空对象并打告警，由调用方用
	// 默认值兜底（手改坏一个字符只丢设置，不会让客户端起不来）。
	QJsonObject read(const QString& fileName);

	// 写设置文件（自动建目录）；成功返回 true
	bool write(const QString& fileName, const QJsonObject& object);
}

// AppearanceSetting.json：界面语言与主题
namespace AppearanceSetting
{
	// 主题模式。System = 跟随系统（Windows 的浅色/深色应用模式）
	enum class ThemeMode
	{
		System,
		Light,
		Dark
	};

	// 文件名（相对 ClientSettings::dir()）
	QString fileName();

	// 首次运行时落一份带默认值的模板，便于用户找到并手改；文件已存在就原样不动，绝不覆盖用户已做的选择。
	void ensureFile();

	// 界面语言代码（"zh_CN"、"en"…）；空串 = 跟随系统（文件里写的是可读的 "system"，转换在本函数里做）。
	QString languageCode();
	void setLanguageCode(const QString& code);

	// 主题；文件里没有 / 值不存在 / 拼错 一律当 System
	ThemeMode themeMode();
	void setThemeMode(ThemeMode mode);

	// 与文件里字符串取值的互转："system" / "light" / "dark"
	QString themeModeName(ThemeMode mode);
	ThemeMode themeModeFromName(const QString& name);
}
