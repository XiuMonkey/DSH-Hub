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

	// 样式表管理：颜色与外观不再硬编码在 C++ 里，而是来自外部文件
	// （颜色 -> theme-<mode>.json，规则 -> 各板块 *.qss），由本模块加载、
	// 变量替换并安装到 QApplication。
	//
	// 文件双源策略：
	//   - 默认模板内建在 qrc（:/DSHHub/styles/...）；
	//   - 外部 stylesDir 中不存在的文件在启动时从资源“释放”一份出来供定制，
	//     已存在的文件绝不覆盖；
	//   - 读取时外部文件优先，缺失则回退 qrc 内建。

	// 应用启动期（QApplication 创建之后、主窗口创建之前）调用：
	// 释放默认模板 -> 按 mode 读取调色板 -> 合成 QSS 并安装到 QApplication。
	void init(const QString& stylesDir, Mode mode);

	// 切换主题：换调色板并重装全局样式（现有窗口外观即时更新）
	void setMode(Mode mode);

	// 从磁盘/资源重新加载（开发期改文件后热调）
	void reload();

	// 把当前合成样式表安装到单个顶层窗口（及其子树）。
	// 替代全局 qApp 安装：各顶层窗口自行挂载；切主题时旧窗口不再全局重 polish。
	void applyToWindow(QWidget* window);

	// 丢弃外部定制：删除 styles 目录中的默认模板副本（仅删模板，不删目录/其它文件）、
	// 重新从 qrc 释放并重载 —— 用于“恢复默认外观”入口。
	void resetStyles();

	bool isDark();

	// 当前调色板查询：供少量“运行时按状态拼色”的代码使用。
	// 颜色值只允许出现在 theme-*.json；代码只引用语义 key。
	QString color(const QString& key);

	// 当前合成好的完整样式表（含所有板块）
	QString styleSheet();

	// 常用语义色，方便少量运行时拼接
	inline QString windowBg() { return color(QStringLiteral("windowBg")); }
	inline QString border() { return color(QStringLiteral("border")); }
	inline QString textPrimary() { return color(QStringLiteral("textPrimary")); }
	inline QString textSecondary() { return color(QStringLiteral("textSecondary")); }
	inline QString inputBg() { return color(QStringLiteral("inputBg")); }
	inline QString accent() { return color(QStringLiteral("accent")); }

	// 让某个控件（QAbstractScrollArea 时含它的横/纵两个滚动条）重新匹配当前样式表。
	//
	// 为什么需要：QAbstractScrollArea 的滚动条是在**基类构造**里创建并首次解析规则的，
	// 那时子类构造函数体里的 setObjectName("...") 还没执行，QStyleSheetStyle 就把
	// “匹配不到 #objectName QScrollBar” 缓存了下来；之后再设 objectName 不会触发重新
	// 匹配，滚动条就一直按基础（原生/老式）样式绘制。凡是“先建控件、后设 objectName”
	// 的滚动区都要在设完名字后调一次本函数。
	void repolishScrollArea(QWidget* widget);

	// 切换主题：显示过渡弹窗，后台创建新主窗口，完成后自动切换
	void switchTheme(QWidget* currentWindow);
}
