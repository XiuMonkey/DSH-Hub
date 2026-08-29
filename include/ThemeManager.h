#pragma once

#include <QString>

class QWidget;

namespace Theme
{
	enum class Mode
	{
		Light,
		Dark
	};

	void setMode(Mode mode);
	bool isDark();

	QString color(const QString& key);

	// 常用语义色，方便在样式表里拼接
	inline QString windowBg() { return color(QStringLiteral("windowBg")); }
	inline QString border() { return color(QStringLiteral("border")); }
	inline QString textPrimary() { return color(QStringLiteral("textPrimary")); }
	inline QString textSecondary() { return color(QStringLiteral("textSecondary")); }
	inline QString inputBg() { return color(QStringLiteral("inputBg")); }
	inline QString accent() { return color(QStringLiteral("accent")); }

	// 切换主题：显示过渡弹窗，后台创建新主窗口，完成后自动切换
	void switchTheme(QWidget* currentWindow);
}