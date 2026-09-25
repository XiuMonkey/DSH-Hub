#pragma once

// 样式表管理（程序级唯一 QObject 单例）：颜色来自 theme-<mode>.json，规则来自各板块 *.qss；模板内建在
// qrc，外部 stylesDir 缺失的文件启动时释放一份供定制（已存在的绝不覆盖）。所有方法只在 GUI 线程调用。

#include "VirtualClass/VirtualCommon.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <utility>

class QWidget;

class ThemeManager : public QObject, public VirtualTheme
{
	Q_OBJECT
	Q_INTERFACES(VirtualTheme)

public:
	enum class Mode
	{
		Light,
		Dark
	};

	// 程序级唯一实例；刻意不析构：注册表必须活得比所有被登记对象更久
	static ThemeManager& instance();

	~ThemeManager() override;

	// 启动期（QApplication 之后、主窗口之前）调用：释放模板 → 读调色板 → 合成 QSS → 登记进注册表
	void init(const QString& stylesDir, Mode mode);

	void setMode(Mode mode);

	void reload();

	// ⚠️ 未 init（qss 为空）时直接返回：setStyleSheet("") 会清掉窗口上已有的样式
	void applyToWindow(QWidget* window);

	// ⚠️ 插件侧只能 qobject_cast<VirtualTheme*>（跨模块 dynamic_cast 返回 nullptr）；接口只能增不能改
	void ExternalApplyToWindow(QWidget* window) override;

	// 重合成两套、换 QPalette 并挂回所有顶层窗口；⚠️ 它不重建窗口
	bool ExternalReloadStyles() override;

	// 删除 styles 目录中的默认模板副本，重新从 qrc 释放并重载
	void resetStyles();

	bool isDark() const;

	// 代码只引用语义 key，颜色值只出现在 theme-*.json
	QString color(const QString& key) const;

	QString windowBg() const;
	QString border() const;
	QString textPrimary() const;
	QString textSecondary() const;
	QString inputBg() const;
	QString accent() const;

	// ⚠️ 滚动条在基类构造里首次解析规则，那时 objectName 还没设；之后再设名字也不会触发重匹配
	void repolishScrollArea(QWidget* widget);

	// 用户显式选定主题的唯一入口：过渡弹窗 + 建新主窗口 + 写进 AppearanceSetting.json
	void switchTheme(QWidget* currentWindow);

private:
	explicit ThemeManager();

	ThemeManager(const ThemeManager&) = delete;
	ThemeManager& operator=(const ThemeManager&) = delete;

	// 常量与无状态工具一律 private；刻意做成函数而非 static 成员，避免静态初始化期分配堆内存
	static const QString& resourcePrefix();
	static const QStringList& modules(); // 各板块 qss，顺序 = 级联顺序
	static QString resourcePath(const QString& fileName);
	static QString modeKey(Mode mode);
	static QString substituteWith(const QString& qss, const QHash<QString, QString>& palette);
	static QString token(const QHash<QString, QString>& palette, const QString& key, const QString& fallback);
	static void installPaletteFor(const QHash<QString, QString>& palette);

	QString paletteFileFor(Mode m) const;

	// 缺失的文件从 qrc 拷出；已存在不覆盖
	void ensureDefaults();
	QString loadText(const QString& fileName) const;
	QHash<QString, QString> loadPaletteFor(Mode m) const;
	QString composeQssFor(const QHash<QString, QString>& palette) const;
	// 纯计算，供工作线程调用
	std::pair<QHash<QString, QString>, QString> buildFor(Mode m) const;

	void buildAllCaches();
	void ensureCached(Mode m);
	void activate(Mode m);

	QString m_stylesDir;
	Mode m_mode = Mode::Light;

	QHash<QString, QString> m_palette;
	QString m_qss;

	QHash<QString, QHash<QString, QString>> m_paletteCache;
	QHash<QString, QString> m_qssCache;
};
