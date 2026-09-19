#pragma once

// ------------------------------------------------------------------
// ClientSettings.h
// ------------------------------------------------------------------
// 「运行目录里的客户端设置」统一入口：<exe>/ClientSetting/<名字>Setting.json
//
// 为什么另起一套而不是继续用 QSettings：QSettings 在 Windows 上落进注册表，
// 用户看不见、拷不走、也不便随安装目录一起备份/搬移。像外观这种"跟着这份客户端走"
// 的设置更适合明文文件 —— 可以手改、可以随目录带走。
//
// 分工（尚未迁移的部分仍留在 QSettings，见 SettingsStore.h）：
//   本文件      ：AppearanceSetting.json —— 界面语言 + 主题
//   SettingsStore：server/url、agent/defaultPreset
//
// 目录在首次写入时按需创建（QDir::mkpath），不需要预先建好。
// 目录可被环境变量 DSHHUB_CLIENT_SETTING_DIR 覆盖 —— 单元测试靠它把设置指到
// 临时目录，避免污染真实运行目录里的设置。
//
// 写文件走 QSaveFile（先写临时文件再原子替换），避免中途崩溃/掉电留下半截 JSON
// 把用户设置弄坏。
// ------------------------------------------------------------------

#include <QString>

class QJsonObject;

namespace ClientSettings
{
	// 设置目录：DSHHUB_CLIENT_SETTING_DIR（已设置且非空时优先）否则 <exe>/ClientSetting
	QString dir();

	// 设置文件的完整路径（只拼路径，不创建任何东西）
	QString filePath(const QString& fileName);

	// 读设置文件。文件不存在、读不动、JSON 有语法错、或根不是对象 —— 一律返回
	// **空对象**并打告警，由调用方用默认值兜底。这样"用户手改坏了一个字符"
	// 只丢设置，不会让客户端起不来。
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

	// 首次运行时落一份带默认值的模板，便于用户找到并手改。
	// **文件已存在就原样不动** —— 绝不覆盖用户已经做出的选择。
	void ensureFile();

	// 界面语言代码（"zh_CN"、"en"…）；空串 = 跟随系统。
	// 文件里写的是可读的 "system"，这里是代码里的语义（空串），由本函数转换。
	QString languageCode();
	void setLanguageCode(const QString& code);

	// 主题；文件里没有 / 值不存在 / 拼错 一律当 System
	ThemeMode themeMode();
	void setThemeMode(ThemeMode mode);

	// 与文件里字符串取值的互转："system" / "light" / "dark"
	QString themeModeName(ThemeMode mode);
	ThemeMode themeModeFromName(const QString& name);
}
