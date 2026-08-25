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
Mode mode();
bool isDark();

QString color(const QString &key);

// 常用语义色，方便在样式表里拼接
inline QString windowBg() { return color(QStringLiteral("windowBg")); }
inline QString panelBg() { return color(QStringLiteral("panelBg")); }
inline QString cardBg() { return color(QStringLiteral("cardBg")); }
inline QString hoverBg() { return color(QStringLiteral("hoverBg")); }
inline QString activeBg() { return color(QStringLiteral("activeBg")); }
inline QString border() { return color(QStringLiteral("border")); }
inline QString textPrimary() { return color(QStringLiteral("textPrimary")); }
inline QString textSecondary() { return color(QStringLiteral("textSecondary")); }
inline QString accent() { return color(QStringLiteral("accent")); }
inline QString accentHover() { return color(QStringLiteral("accentHover")); }
inline QString danger() { return color(QStringLiteral("danger")); }
inline QString dangerBg() { return color(QStringLiteral("dangerBg")); }
inline QString inputBg() { return color(QStringLiteral("inputBg")); }
inline QString userBubbleBg() { return color(QStringLiteral("userBubbleBg")); }
inline QString userBubbleText() { return color(QStringLiteral("userBubbleText")); }
inline QString scrollbar() { return color(QStringLiteral("scrollbar")); }
inline QString scrollbarHover() { return color(QStringLiteral("scrollbarHover")); }

// 切换主题：显示过渡弹窗，后台创建新主窗口，完成后自动切换
void switchTheme(QWidget *currentWindow);

}