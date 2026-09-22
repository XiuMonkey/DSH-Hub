#pragma once

#include "VirtualClass/VirtualCommon.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <utility>

class QWidget;

// 样式表管理（程序级唯一实例）：颜色与外观不硬编码在 C++ 里，而是颜色来自 theme-<mode>.json、规则来自各板块 *.qss，由本模块加载、变量替换并安装到各顶层窗口。
// 文件双源：默认模板内建在 qrc（:/DSHHub/styles/...）；外部 stylesDir 中缺失的文件启动时从资源释放一份出来供定制、已存在的绝不覆盖；读取时外部优先、缺失回退 qrc 内建。
// 陷阱：扩展（QPlugin DLL）只能经 CommonRegistry 按 index 取对象再做接口转换 —— 命名空间与自由函数没有 QObject 身份、够不到，故本类必须是 QObject 单例（宿主↔插件符号规则见 core/HostExports.h）；所有方法只在 GUI 线程调用。

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

	// 程序级唯一实例。刻意不析构（见 .cpp）：它是 QObject，而注册表必须活得比所有被登记对象更久，静态析构顺序无从保证，索性一起长存。
	static ThemeManager& instance();

	// 单例不会走到这里，声明它是为了满足 QObject / VirtualTheme 的虚析构。
	~ThemeManager() override;

	// 启动期（QApplication 创建之后、主窗口创建之前）调用：释放默认模板 -> 按 mode 读取调色板 -> 合成 QSS -> 登记进全局注册表；mode 由 main 决定，优先用 ClientSetting/AppearanceSetting.json 里的显式选择，未设置（System）才跟随系统颜色模式 —— 见 ClientSettings.h。
	void init(const QString& stylesDir, Mode mode);

	// 切换主题：换调色板并重装全局样式（现有窗口外观即时更新）
	void setMode(Mode mode);

	// 从磁盘/资源重新加载（开发期改文件后热调）
	void reload();

	// 把当前合成样式表安装到单个顶层窗口（及其子树），替代全局 qApp 安装。未 init（qss 还是空的）时直接返回：setStyleSheet("") 会把窗口上已有的样式清掉，比什么都不做糟得多 —— 插件拿到注册表对象后可能在任何时刻调进来。
	void applyToWindow(QWidget* window);

	// VirtualTheme 接口实现（插件侧唯一入口，语义同 applyToWindow）：插件只需 qobject_cast<VirtualTheme*>(DshHost::findObject("themeManager"))->ExternalApplyToWindow(w) —— 转换走 obj->qt_metacast(IID)，跨边界传的是字符串、插件侧零宿主符号，接口虚函数走 vtable 同样不产生外部符号；接口一旦发布就只能增不能改，故“给外部用的那个名字”与宿主内部 API 分开。⚠️ 别改成 dynamic_cast（Itanium ABI 下跨模块静默返回 nullptr），也别 qobject_cast<ThemeManager*>（要 ThemeManager::staticMetaObject，宿主 exe 的外部符号且零导出 ⇒ 插件 DLL 链接期 LNK2019）。
	void ExternalApplyToWindow(QWidget* window) override;

	// VirtualTheme 接口实现（v2 追加，槽位在末尾）：重读本地样式文件（exe 同目录 styles/ 下的 *.qss 与 theme-*.json，外部优先、qrc 兜底）、重合成亮/暗两套、换 QPalette，并把新样式表挂回所有顶层窗口；返回“重载后当前样式表非空”，false 基本只有一种成因 —— 那些文件根本没读到（写失败、路径不对、权限不足），所以它同时充当事后自检。未 init 时一律拒绝并告警（同 applyToWindow 的理由）。⚠️ 它不重建窗口：主题相关但构造期就已固化的东西（如按 isDark() 选的 logo）不会跟着变 —— 要那种一致，得走 switchTheme() 那条重建窗口的路。
	bool ExternalReloadStyles() override;

	// 丢弃外部定制：删除 styles 目录中的默认模板副本（仅删模板，不删目录/其它文件）、重新从 qrc 释放并重载 —— 用于“恢复默认外观”入口。
	void resetStyles();

	bool isDark() const;

	// 当前调色板查询：颜色值只允许出现在 theme-*.json；代码只引用语义 key。
	QString color(const QString& key) const;

	// 常用语义色，方便少量运行时拼接
	QString windowBg() const;
	QString border() const;
	QString textPrimary() const;
	QString textSecondary() const;
	QString inputBg() const;
	QString accent() const;

	// 让控件（QAbstractScrollArea 时含它的横/纵两个滚动条）重新匹配当前样式表 —— QAbstractScrollArea 的滚动条在基类构造里创建并首次解析规则，那时子类构造函数体里的 setObjectName("...") 还没执行，QStyleSheetStyle 就把“匹配不到 #objectName QScrollBar”缓存了下来；之后再设 objectName 不会触发重新匹配，滚动条就一直按基础（原生/老式）样式绘制，故凡是“先建控件、后设 objectName”的滚动区都要在设完名字后调一次本函数。
	void repolishScrollArea(QWidget* widget);

	// 切换主题：显示过渡弹窗，后台创建新主窗口，完成后自动切换；这是“用户显式选定主题”的唯一入口，会把新主题写进 ClientSetting/AppearanceSetting.json —— 之后启动不再跟随系统。
	void switchTheme(QWidget* currentWindow);

private:
	// 只能经由 instance() 取得
	explicit ThemeManager();

	ThemeManager(const ThemeManager&) = delete;
	ThemeManager& operator=(const ThemeManager&) = delete;

	// 常量与无状态工具，一律 private（对外没有入口，插件更拿不到 —— 插件手里只有 VirtualTheme 接口）。两处刻意的选择：常量做成“返回引用的函数”而不是 static 数据成员（类/命名空间作用域的 QString、QStringList 会在静态初始化期构造，main 之前就分配堆内存）；installPaletteFor 有副作用（换掉整个应用的 QPalette），只允许被 setMode()/reload() 间接调用。
	static const QString& resourcePrefix();        // qrc 里样式模板的根路径
	static const QStringList& modules();           // 各板块 qss，顺序 = 级联顺序
	static QString resourcePath(const QString& fileName);
	static QString modeKey(Mode mode);             // 两套主题预合成缓存的键
	static QString substituteWith(const QString& qss,
		const QHash<QString, QString>& palette);
	static QString token(const QHash<QString, QString>& palette, const QString& key,
		const QString& fallback);
	static void installPaletteFor(const QHash<QString, QString>& palette);

	QString paletteFileFor(Mode m) const;

	// 释放默认模板：stylesDir 里缺失的文件从 qrc 拷出；已存在不覆盖
	void ensureDefaults();

	// 外部优先、qrc 兜底地读取一个文本文件
	QString loadText(const QString& fileName) const;

	// 读取某个主题的色板 JSON：{ "key": "颜色" }
	QHash<QString, QString> loadPaletteFor(Mode m) const;

	// 用给定色板合成一份完整 QSS（模块按 modules() 的顺序拼接）
	QString composeQssFor(const QHash<QString, QString>& palette) const;

	// 计算单个主题的（色板, QSS）——纯计算，供工作线程调用
	std::pair<QHash<QString, QString>, QString> buildFor(Mode m) const;

	// 预合成两套主题（计算放后台线程），结果写回缓存
	void buildAllCaches();

	// 若某个主题还没合成过，同步补一次（reload/reset 后首次读取用）
	void ensureCached(Mode m);

	// 把某个主题的缓存镜像到当前生效成员
	void activate(Mode m);

	QString m_stylesDir;                 // 外部可覆盖目录（通常 exe 同目录 /styles）
	Mode m_mode = Mode::Light;

	// 当前生效主题的镜像（供 color()/applyToWindow() 快速读取）
	QHash<QString, QString> m_palette;
	QString m_qss;

	// 两套主题的预合成缓存：键 = modeKey()；切主题只做缓存命中，不做字符串计算
	QHash<QString, QHash<QString, QString>> m_paletteCache;
	QHash<QString, QString> m_qssCache;
};
