#include "common/appearance/ThemeManager.h"

#include "common/appearance/CardShadow.h"
#include "common/settings/ClientSettings.h"
#include "common/util/CommonRegistry.h"
#include "core/DSHHub.h"
#include "core/HostExports.h"
#include "ui/ShadowPanel.h"
#include "ui/SpinnerWidget.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QObject>
#include <QPalette>
#include <QRegularExpression>
#include <QResource>
#include <QScreen>
#include <QScrollBar>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QDebug>

#include <future>
#include <utility>

// 常量刻意做成"返回引用的函数"而不是 static 数据成员：类/命名空间作用域的 QString、QStringList 会在静态初始化期构造（main 之前就分配堆内存），是 Qt 的老坑；函数内 static 推迟到首次调用，且初始化线程安全。

const QString& ThemeManager::resourcePrefix()
{
	static const QString prefix = QStringLiteral(":/DSHHub/styles/");
	return prefix;
}

const QStringList& ThemeManager::modules()
{
	// 各板块样式文件（与 resources/styles 下的默认模板一一对应）：一个文件 = 一个界面/表面；顺序 = 级联顺序（同优先级的选择器后来者胜），所以全局默认（外壳/通用控件/滚动条）在前、各界面在后，便于界面覆盖默认。
	static const QStringList list = {
		QStringLiteral("main-window.qss"),      // 主窗口外壳与自绘标题栏
		QStringLiteral("defaults.qss"),         // 通用控件默认外观（各界面覆盖它）
		QStringLiteral("scrollbars.qss"),       // 全局滚动条（唯一一份滚动条规则）
		QStringLiteral("chat.qss"),
		QStringLiteral("sidebar.qss"),          // 左侧会话栏
		QStringLiteral("popups.qss"),           // 浮层与各窗口遮罩、弹窗外壳
		QStringLiteral("tooltip.qss"),          // 悬浮提示气泡（同为浮层，紧跟弹窗外壳）
		QStringLiteral("extension-manager.qss"),
		QStringLiteral("topbar.qss"),           // 对话顶栏与工具过滤窗口
		QStringLiteral("settings.qss"),
		QStringLiteral("model-list.qss"),       // 模型列表面板（设置 → 模型列表）
		QStringLiteral("plugin-market.qss"),
	};
	return list;
}

QString ThemeManager::resourcePath(const QString& fileName)
{
	return resourcePrefix() + fileName;
}

QString ThemeManager::modeKey(Mode mode)
{
	return mode == Mode::Dark ? QStringLiteral("dark") : QStringLiteral("light");
}

// 把 {{key}} 占位替换成色板颜色（找不到的 key 原样保留以便排查）
QString ThemeManager::substituteWith(const QString& qss,
	const QHash<QString, QString>& palette)
{
	// 局部变量别叫 token：那会遮蔽本类的静态成员函数 token()
	static const QRegularExpression kPattern(
		QStringLiteral("\\{\\{\\s*([A-Za-z][A-Za-z0-9]*)\\s*\\}\\}"));

	QString out;
	out.reserve(qss.size());

	int last = 0;
	QRegularExpressionMatchIterator it = kPattern.globalMatch(qss);
	while (it.hasNext()) {
		const QRegularExpressionMatch match = it.next();
		out += qss.mid(last, match.capturedStart() - last);

		const QString key = match.captured(1);
		QString value = palette.value(key);
		// 自定义主题可能还没有新的按钮色：回退到 accent，避免占位符原样漏进 QSS。
		if (value.isEmpty()) {
			if (key == QLatin1String("primaryButtonBg"))
				value = palette.value(QStringLiteral("accent"));
			else if (key == QLatin1String("primaryButtonHover"))
				value = palette.value(QStringLiteral("accentHover"));
			else if (key == QLatin1String("primaryButtonPressed"))
				value = palette.value(QStringLiteral("primaryButtonHover"));
			else if (key == QLatin1String("primaryButtonText"))
				value = palette.value(QStringLiteral("textOnAccent"));
			else if (key == QLatin1String("primaryButtonDisabledBg"))
				value = palette.value(QStringLiteral("border"));
			else if (key == QLatin1String("primaryButtonDisabledText"))
				value = palette.value(QStringLiteral("textTertiary"));
		}
		out += value.isEmpty() ? match.captured(0) : value;

		last = match.capturedEnd();
	}
	out += qss.mid(last);
	return out;
}

// 按语义 key 取色，缺失时用 fallback（色板可能来自用户自己的主题文件）
QString ThemeManager::token(const QHash<QString, QString>& palette, const QString& key,
	const QString& fallback)
{
	const auto it = palette.constFind(key);
	return it == palette.constEnd() ? fallback : it.value();
}

// 生成并应用与当前主题一致的 QPalette：QSS 没覆盖到的默认文字/底色（输入框文字、列表项、上下文菜单等）在暗色模式下会退化成 Qt 默认的亮色调色板（黑字），这里统一按当前色板修正。它有副作用（换掉整个应用的 QPalette），所以刻意留在 private，外部只能通过 setMode()/reload() 间接触发。
void ThemeManager::installPaletteFor(const QHash<QString, QString>& palette)
{
	QPalette pal = QApplication::palette();
	// 色板值可能是 CSS 的 rgba(...)，QColor 的字符串构造不认那种写法（会得到无效色），一律走 CardShadow::parseColor（两种写法都认，是 QColor 的超集）。
	const QColor window(CardShadow::parseColor(token(palette, QStringLiteral("windowBg"), QStringLiteral("#FFFFFF"))));
	const QColor panel(CardShadow::parseColor(token(palette, QStringLiteral("panelBg"), QStringLiteral("#FFFFFF"))));
	const QColor text(CardShadow::parseColor(token(palette, QStringLiteral("textPrimary"), QStringLiteral("#000000"))));
	const QColor textDim(CardShadow::parseColor(token(palette, QStringLiteral("textSecondary"), QStringLiteral("#666666"))));
	const QColor accent(CardShadow::parseColor(token(palette, QStringLiteral("accent"), QStringLiteral("#4C8BF5"))));
	const QColor onAccent(CardShadow::parseColor(token(palette, QStringLiteral("textOnAccent"), QStringLiteral("#FFFFFF"))));
	const QColor border(CardShadow::parseColor(token(palette, QStringLiteral("border"), QStringLiteral("#E5E7EB"))));

	const auto apply = [&pal](QPalette::ColorRole role, const QColor& color) {
		for (int i = 0; i < QPalette::NColorGroups; ++i) {
			if (i == QPalette::Disabled) {
				pal.setColor(static_cast<QPalette::ColorGroup>(i), role,
					color.lighter(150));
			}
			else {
				pal.setColor(static_cast<QPalette::ColorGroup>(i), role, color);
			}
		}
		};
	apply(QPalette::Window, window);
	apply(QPalette::WindowText, text);
	apply(QPalette::Base, panel);
	apply(QPalette::AlternateBase, border);
	apply(QPalette::Text, text);
	apply(QPalette::Button, window);
	apply(QPalette::ButtonText, text);
	apply(QPalette::PlaceholderText, textDim);
	apply(QPalette::Highlight, accent);
	apply(QPalette::HighlightedText, onAccent);
	apply(QPalette::ToolTipBase, panel);
	apply(QPalette::ToolTipText, text);

	if (qApp)
		qApp->setPalette(pal);
}

ThemeManager& ThemeManager::instance()
{
	// 刻意不析构：本对象是 QObject，且会在 init() 里被登记进 CommonRegistry（那个单例自己也是"刻意不析构"，见 CommonRegistry.cpp）；静态析构顺序无从保证，与其留一个"注册表里还指着已析构对象、插件刚好这时调进来"的窗口，不如让它跟注册表一样长存。
	static ThemeManager* const inst = new ThemeManager();
	return *inst;
}

ThemeManager::ThemeManager() = default;

ThemeManager::~ThemeManager() = default;

QString ThemeManager::paletteFileFor(Mode m) const
{
	return m == Mode::Dark
		? QStringLiteral("theme-dark.json")
		: QStringLiteral("theme-light.json");
}

// 释放默认模板：stylesDir 里缺失的文件从 qrc 拷出，已存在不覆盖（用户定制优先）。代价是程序发布的新版模板会被旧的同名外部文件盖住 —— 界面看不出报错，只是新控件的样式一直不生效；这里对"已存在但与内置模板不同"的文件打一条提示（不覆盖，也不删用户的东西），把这种沉默的失效变成可查的日志。
void ThemeManager::ensureDefaults()
{
	QDir dir(m_stylesDir);
	if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
		qWarning().noquote() << "[Theme] cannot create styles dir:" << m_stylesDir;
		return;
	}

	const QStringList files = QStringList(modules())
		<< QStringLiteral("theme-light.json")
		<< QStringLiteral("theme-dark.json");
	for (const QString& file : files) {
		QFile res(resourcePath(file));
		if (!res.open(QIODevice::ReadOnly)) {
			qWarning().noquote() << "[Theme] missing default template in qrc:" << file;
			continue;
		}
		const QByteArray data = res.readAll();
		res.close();

		const QString target = dir.filePath(file);
		if (QFile::exists(target)) {
			QFile existing(target);
			const bool readable = existing.open(QIODevice::ReadOnly);
			const QByteArray current = readable ? existing.readAll() : QByteArray();
			if (readable)
				existing.close();

			// 与内置模板不一致时，这份外部文件会在 loadText() 里胜出；只提示，不动它（它可能是用户自己改的）。
			if (current != data) {
				qInfo().noquote()
					<< "[Theme] external style differs from the shipped default:"
					<< target
					<< "-> 这份外部样式优先于程序内置模板；"
					"若刚更新过程序、看不到新样式，请在“设置 - 外观设置”里重置样式为默认";
			}
			continue;
		}

		QFile out(target);
		if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			out.write(data);
			out.close();
			qInfo().noquote() << "[Theme] released default style:" << target;
		}
		else {
			qWarning().noquote() << "[Theme] cannot write style file:" << target;
		}
	}
}

// 外部优先、qrc 兜底地读取一个文本文件
QString ThemeManager::loadText(const QString& fileName) const
{
	const QString external = QDir(m_stylesDir).filePath(fileName);
	if (QFile::exists(external)) {
		QFile f(external);
		if (f.open(QIODevice::ReadOnly | QIODevice::Text))
			return QString::fromUtf8(f.readAll());
	}

	QFile res(resourcePath(fileName));
	if (res.open(QIODevice::ReadOnly | QIODevice::Text))
		return QString::fromUtf8(res.readAll());

	qWarning().noquote() << "[Theme] style file not found (external/qrc):" << fileName;
	return QString();
}

// 读取某个主题的色板 JSON：{ "key": "颜色" }
QHash<QString, QString> ThemeManager::loadPaletteFor(Mode m) const
{
	QHash<QString, QString> result;

	const QString fileName = paletteFileFor(m);
	const QByteArray raw = loadText(fileName).toUtf8();
	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		qWarning().noquote() << "[Theme] palette parse error:" << fileName
			<< parseError.errorString();
		return result;
	}

	const QJsonObject obj = doc.object();
	for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
		result.insert(it.key(), it.value().toString());
	return result;
}

// 用给定色板合成一份完整 QSS（模块按 modules() 的顺序拼接）
QString ThemeManager::composeQssFor(const QHash<QString, QString>& palette) const
{
	QString combined;
	for (const QString& module : modules()) {
		const QString text = loadText(module);
		if (!text.isEmpty())
			combined += substituteWith(text, palette) + QStringLiteral("\n\n");
	}
	return combined;
}

// 计算单个主题的（色板, QSS）——纯计算，供工作线程调用
std::pair<QHash<QString, QString>, QString> ThemeManager::buildFor(Mode m) const
{
	const auto pal = loadPaletteFor(m);
	return std::make_pair(pal, composeQssFor(pal));
}

// 预合成两套主题（计算放后台线程），结果写回缓存
void ThemeManager::buildAllCaches()
{
	auto light = std::async(std::launch::async, [this]() { return buildFor(Mode::Light); });
	auto dark = std::async(std::launch::async, [this]() { return buildFor(Mode::Dark); });

	const auto lightResult = light.get();
	const auto darkResult = dark.get();
	m_paletteCache.insert(modeKey(Mode::Light), lightResult.first);
	m_qssCache.insert(modeKey(Mode::Light), lightResult.second);
	m_paletteCache.insert(modeKey(Mode::Dark), darkResult.first);
	m_qssCache.insert(modeKey(Mode::Dark), darkResult.second);
}

// 若某个主题还没合成过，同步补一次（reload/reset 后首次读取用）
void ThemeManager::ensureCached(Mode m)
{
	const QString key = modeKey(m);
	if (m_qssCache.contains(key))
		return;
	const auto result = buildFor(m);
	m_paletteCache.insert(key, result.first);
	m_qssCache.insert(key, result.second);
}

void ThemeManager::activate(Mode m)
{
	m_mode = m;
	m_palette = m_paletteCache.value(modeKey(m));
	m_qss = m_qssCache.value(modeKey(m));
}

// 全局兜底：任何滚动区**首次显示**时补一次滚动条解析 —— 有了它，"新加滚动区忘了调 ThemeManager::instance().repolishScrollArea()"不会再变成视觉 bug；显式调用仍然有意义：它在第一次绘制前就弄对了，不会闪一下。挂在 qApp 上（见 init()），只在 Show 事件上做一次廉价的 qobject_cast。
namespace
{
	class ScrollAreaShowFilter : public QObject
	{
	public:
		using QObject::QObject;

	protected:
		bool eventFilter(QObject* watched, QEvent* event) override
		{
			if (event->type() == QEvent::Show) {
				if (auto* area = qobject_cast<QAbstractScrollArea*>(watched)) {
					// Show 派发期间重新解析会搅乱事件流：排到本轮之后；以控件自身为上下文，控件先销毁时这次调用自动作废；repolishScrollArea 自带"每个控件只挂一次补丁"的标记，重复调用无害。
					QMetaObject::invokeMethod(area, [area]() {
						ThemeManager::instance().repolishScrollArea(area);
						}, Qt::QueuedConnection);
				}
			}
			return QObject::eventFilter(watched, event);
		}
	};
} // namespace

void ThemeManager::init(const QString& stylesDir, Mode mode)
{
	m_stylesDir = stylesDir;
	ensureDefaults();

	// 计算放线程：启动时一次性预合成亮/暗两套（切主题时主线程零计算）
	buildAllCaches();

	setMode(mode);

	static bool showFilterInstalled = false;
	if (!showFilterInstalled && qApp) {
		showFilterInstalled = true;
		qApp->installEventFilter(new ScrollAreaShowFilter(qApp));
	}

	// 登记进全局注册表：客户端扩展按 index 取到本对象，再转成 VirtualTheme* 调 ExternalApplyToWindow（同 TopBar/VirtualTopBar 那套，见 HostExports.h）。放在最后一步：登记出去的对象必须已经可用 —— qss 已合成，插件这时候调进来才不会被 applyToWindow 的空样式表闸门挡回。
	CommonRegistry::instance().AddToRegistry(DshHostIndex::kThemeManager, this);

	qInfo().noquote() << "[Theme] styles initialized from:" << m_stylesDir
		<< "mode=" << (mode == Mode::Dark ? "Dark" : "Light")
		<< "paletteKeys=" << m_palette.size();
}

void ThemeManager::setMode(Mode mode)
{
	ensureCached(mode);
	activate(mode);
	// 同步应用 QPalette（QSS 未覆盖的默认文字/底色随主题走）
	installPaletteFor(m_palette);
	qInfo().noquote() << "[Theme] set mode:" << (m_mode == Mode::Dark ? "Dark" : "Light")
		<< "qssBytes=" << m_qss.size();
}

// 把当前合成样式表安装到单个窗口（及其子树）；各顶层窗口（主窗 / 弹窗 / 主题切换卡）自行调用，切主题时旧窗口不再全局重 polish。
void ThemeManager::applyToWindow(QWidget* window)
{
	if (!window)
		return;

	// 没 init 过（或模板全缺）时 m_qss 是空的：setStyleSheet("") 等于把窗口上已有的样式清掉，比什么都不做糟得多；插件拿到注册表对象后可能在任何时刻调进来，所以这道闸门必须留在函数里，不能只靠"调用方都记得先 init"。
	if (m_qss.isEmpty()) {
		qWarning().noquote() << "[Theme] applyToWindow before init (empty qss), skipped";
		return;
	}

	window->setStyleSheet(m_qss);
}

void ThemeManager::ExternalApplyToWindow(QWidget* window)
{
	// VirtualTheme 接口的实现就是转发：插件与宿主内部走的必须是同一条路，别在这里加任何额外语义（否则"插件挂的样式"和"宿主挂的样式"会分家）。
	applyToWindow(window);
}

bool ThemeManager::ExternalReloadStyles()
{
	// 未 init 时没有可信的样式目录：这种情况下 reload() 只会把缓存刷成"仅 qrc 兜底"的那一份，还会顺手 activate(m_mode) 把生效镜像换掉；与 applyToWindow 的空样式表闸门同理 —— 这道闸必须留在函数里，不能指望调用方都记得先 init。
	if (m_stylesDir.isEmpty()) {
		qWarning().noquote() << "[Theme] ExternalReloadStyles before init, refused";
		return false;
	}

	// 典型用法：插件先覆盖 <exe>/styles/*.qss，再调这里 —— reload() 重新读文件、重新合成亮/暗两套、换 QPalette，并把新样式表挂回所有顶层窗口。它不重建窗口：主题相关但构造期就已固化的东西（例如按 isDark() 选的 logo 资源，见 TitleBar/Sidebar/Main 的构造）不会跟着变。
	reload();

	// 空样式表 = 一份都没读到（写失败 / 路径不对 / 权限不足）。如实回给插件，别让它对着一个"看起来调成功了"的 void 去排查。
	if (m_qss.isEmpty()) {
		qWarning().noquote() << "[Theme] ExternalReloadStyles composed an empty stylesheet;"
			" nothing was readable under:" << m_stylesDir;
		return false;
	}
	return true;
}

void ThemeManager::reload()
{
	// 重新从磁盘/资源合成（含用户改动后的外部文件）
	buildAllCaches();
	activate(m_mode);
	installPaletteFor(m_palette);

	// 重挂到所有顶层窗口（开发期热调 / 重置默认后）
	const QList<QWidget*> topLevels = QApplication::topLevelWidgets();
	for (QWidget* window : topLevels)
		applyToWindow(window);
	qInfo().noquote() << "[Theme] styles reloaded from:" << m_stylesDir;
}

void ThemeManager::resetStyles()
{
	if (m_stylesDir.isEmpty()) {
		qWarning().noquote() << "[Theme] resetStyles called before init";
		return;
	}

	// 只删除已知的默认模板文件（qss + 色板），保留目录里其它可能存在的文件
	QDir dir(m_stylesDir);
	const QStringList files = QStringList(modules())
		<< QStringLiteral("theme-light.json")
		<< QStringLiteral("theme-dark.json");
	for (const QString& file : files) {
		const QString target = dir.filePath(file);
		if (QFile::exists(target) && !QFile::remove(target))
			qWarning().noquote() << "[Theme] cannot remove style file:" << target;
	}

	ensureDefaults();
	reload();
	qInfo().noquote() << "[Theme] styles reset to defaults in:" << m_stylesDir;
}

bool ThemeManager::isDark() const
{
	return m_mode == Mode::Dark;
}

QString ThemeManager::color(const QString& key) const
{
	const auto it = m_palette.constFind(key);
	if (it != m_palette.constEnd())
		return it.value();
	return QStringLiteral("#000000");
}

// 常用语义色：都只是 color() 的糖，别在别处再拼一遍 key
QString ThemeManager::windowBg() const { return color(QStringLiteral("windowBg")); }
QString ThemeManager::border() const { return color(QStringLiteral("border")); }
QString ThemeManager::textPrimary() const { return color(QStringLiteral("textPrimary")); }
QString ThemeManager::textSecondary() const { return color(QStringLiteral("textSecondary")); }
QString ThemeManager::inputBg() const { return color(QStringLiteral("inputBg")); }
QString ThemeManager::accent() const { return color(QStringLiteral("accent")); }

// 见头文件注释：滚动条是基类构造时建好的，那时子类的 objectName 还没设，QStyleSheetStyle 会把"匹配不到规则"缓存下来。光在构造后解析一次还不够：样式表是按顶层窗口挂的（见 applyToWindow），控件在构造时可能还没接进那个窗口（预构建的控件树、先建后插的卡片都属于这种），或者那条规则要等滚动条真的出现才谈得上 —— 表现就是"首次渲染是原生老式滚动条，重启/切主题后又好了"（重启会重新 setStyleSheet，整棵子树被重新解析）。所以这里一次挂好两个只跑一次的补丁：1) 首次 Show 之后再解析一次（那时控件一定已经在带样式表的窗口里）；2) 任一滚动条第一次真的有范围（= 它真的会出现）时再解析一次；补丁跑完自动摘掉，对滚动列表没有持续开销。另有一层全局兜底 ScrollAreaShowFilter（见 init()）：任何滚动区首次显示都会被抓一次，所以新控件忘记调用也不会再退回原生滚动条；显式调用仍然值得做，它让控件在第一次绘制之前就已经是对的样子。
namespace
{
	class ScrollBarRepolisher : public QObject
	{
	public:
		explicit ScrollBarRepolisher(QAbstractScrollArea* area)
			: QObject(area)
			, m_area(area)
		{
			area->installEventFilter(this);
			for (QScrollBar* bar : bars()) {
				if (!bar)
					continue;
				m_connections.append(connect(bar, &QScrollBar::rangeChanged, this, [this, bar]() {
					if (m_ranged || bar->maximum() <= bar->minimum())
						return;
					m_ranged = true;
					ThemeManager::instance().repolishScrollArea(m_area);
					retireIfDone();
					}));
			}
		}

	protected:
		bool eventFilter(QObject* watched, QEvent* event) override
		{
			if (event->type() == QEvent::Show && !m_shown) {
				m_shown = true;
				// Show 期间重新解析会把正在派发的事件搅乱，挪到本轮事件之后
				QMetaObject::invokeMethod(this, [this]() {
					ThemeManager::instance().repolishScrollArea(m_area);
					retireIfDone();
					}, Qt::QueuedConnection);
			}
			return QObject::eventFilter(watched, event);
		}

	private:
		QList<QScrollBar*> bars() const
		{
			return { m_area->horizontalScrollBar(), m_area->verticalScrollBar() };
		}

		void retireIfDone()
		{
			// 两个触发都见过就不再需要自己了（只挂一次的补丁）
			if (!m_shown || !m_ranged)
				return;
			for (const QMetaObject::Connection& connection : m_connections)
				disconnect(connection);
			m_connections.clear();
			m_area->removeEventFilter(this);
			deleteLater();
		}

		QAbstractScrollArea* m_area = nullptr;
		QList<QMetaObject::Connection> m_connections;
		bool m_shown = false;
		bool m_ranged = false;
	};
} // namespace

void ThemeManager::repolishScrollArea(QWidget* widget)
{
	if (!widget)
		return;

	if (QStyle* style = widget->style()) {
		style->unpolish(widget);
		style->polish(widget);
	}

	auto* area = qobject_cast<QAbstractScrollArea*>(widget);
	if (!area)
		return;

	for (QScrollBar* bar : { area->horizontalScrollBar(), area->verticalScrollBar() }) {
		if (!bar)
			continue;
		if (QStyle* style = bar->style()) {
			style->unpolish(bar);
			style->polish(bar);
		}
	}

	// 构造期解析可能太早（那时还没接进带样式表的窗口）：挂上"首次显示 / 首次真出现滚动条"的补丁（每个控件只挂一次）
	static const char* const kHooked = "dshScrollAreaRepolishHooked";
	if (area->property(kHooked).toBool())
		return;
	area->setProperty(kHooked, true);
	new ScrollBarRepolisher(area);
}

void ThemeManager::switchTheme(QWidget* currentWindow)
{
	// 先构造并显示“切换中”过渡卡片（使用当前主题），确保点击后立刻出现，而不是等 setMode（重建 + 全应用重设样式表，较耗时）做完才显示。
	auto* popup = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Dialog);
	popup->setAttribute(Qt::WA_TranslucentBackground);

	// 卡片外面套阴影外壳（浮层最高一档），整窗尺寸里要把它算进去
	const CardShadow::Spec cardShadow = CardShadow::level3();
	const QMargins cardPad = CardShadow::padding(cardShadow);

	auto* outerLayout = new QVBoxLayout(popup);
	outerLayout->setContentsMargins(0, 0, 0, 0);

	auto* body = new QWidget(popup);
	body->setObjectName(QStringLiteral("themeSwitchBody"));
	body->setAttribute(Qt::WA_StyledBackground, true);

	auto* bodyPanel = new ShadowPanel(QStringLiteral("shadowFloat"), cardShadow, popup);
	bodyPanel->setRadius(20); // 与 #themeSwitchBody 的 QSS 圆角一致
	bodyPanel->setCard(body);
	body->setFixedSize(360, 200);
	outerLayout->addWidget(bodyPanel);
	popup->setFixedSize(360 + cardPad.left() + cardPad.right(),
		200 + cardPad.top() + cardPad.bottom());

	auto* layout = new QVBoxLayout(body);
	layout->setContentsMargins(24, 20, 24, 20);
	layout->setSpacing(12);

	auto* spinner = new SpinnerWidget(body);
	spinner->setFixedSize(40, 40);
	spinner->start();
	layout->addWidget(spinner, 0, Qt::AlignHCenter);

	auto* label = new QLabel(qtTrId("theme_switching"), body);
	label->setObjectName(QStringLiteral("themeSwitchLabel"));
	label->setAlignment(Qt::AlignCenter);
	layout->addWidget(label);

	layout->addStretch(1);

	popup->move(QGuiApplication::primaryScreen()->geometry().center() - popup->rect().center());
	applyToWindow(popup); // 窗口级安装：卡片自己挂样式（不再依赖全局 qApp 表）
	popup->show();
	popup->raise();
	// 强制先画出一帧（否则会与下方 setMode 的耗时操作同一帧出现，观感仍是“卡”）
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

	// 趁旧窗口还活着，记下它的窗口状态与几何：新窗口是重新构造的，构造函数里会 resize 成默认尺寸（见 Main.cpp），不接过来就等于把用户调好的大小、位置、最大化状态一起重置掉。
	const Qt::WindowStates oldState =
		currentWindow ? currentWindow->windowState() : Qt::WindowStates(Qt::WindowNoState);
	const QRect oldGeometry = currentWindow ? currentWindow->geometry() : QRect();

	if (currentWindow)
		currentWindow->hide();

	// 再切换主题：预合成缓存命中，主线程只做缓存取用，不再重建字符串
	const Mode nextMode = isDark() ? Mode::Light : Mode::Dark;

	// 记进 AppearanceSetting.json：用户已经显式选过主题了，下次启动就照它，不再跟随系统（把 system 固化成明确的 light/dark）；写在 setMode 之前，免得后面重建窗口若出岔子、用户的选择反而丢了。
	AppearanceSetting::setThemeMode(nextMode == Mode::Dark
		? AppearanceSetting::ThemeMode::Dark
		: AppearanceSetting::ThemeMode::Light);

	setMode(nextMode);
	// 让过渡卡立刻换上新主题
	applyToWindow(popup);

	// 切换主题时复用当前 DSH server，不创建新 server
	QUrl oldBaseUrl;
	QProcess* oldServerProcess = nullptr;
	if (auto* oldHub = qobject_cast<DSHHub*>(currentWindow)) {
		// 带令牌的 URL：0.1.5 的 /api 需要 cookie，而 cookie 只能用启动令牌换；只传域名端口的话新窗口会 401（表现为"切主题后窗口不再出现"）。
		oldBaseUrl = oldHub->authenticatedBaseUrl();
		oldServerProcess = oldHub->takeServerProcess();
	}

	auto* newWindow = new DSHHub(nullptr, oldBaseUrl, oldServerProcess);
	newWindow->hide();

	const auto showNewWindow = [currentWindow, newWindow, popup, oldState, oldGeometry]() {
		popup->close();
		popup->deleteLater();

		if (currentWindow)
			currentWindow->deleteLater();

		// 把旧窗口的"窗口状态"和几何接过来：新窗口是重新构造的，构造函数里会 resize 成默认尺寸，不显式恢复的话用户调好的大小 / 位置 / 最大化就全丢了。三种情形分开处理，别混：最大化 / 全屏交给 showMaximized()/showFullScreen() —— 这时候再去 setGeometry 会和窗口管理器打架（那两句话把几何交给系统算）；普通情形先摆几何再 show —— 隐藏的窗口上摆好，出现时不闪。
		if (oldState & Qt::WindowMaximized) {
			newWindow->showMaximized();
		}
		else if (oldState & Qt::WindowFullScreen) {
			newWindow->showFullScreen();
		}
		else {
			if (oldGeometry.isValid())
				newWindow->setGeometry(oldGeometry);
			newWindow->show();
		}

		qInfo().noquote() << "[Theme] switch theme completed"
			<< (oldState & Qt::WindowMaximized ? "(kept maximized)"
				: oldState & Qt::WindowFullScreen ? "(kept fullscreen)"
				: QStringLiteral("(kept geometry %1x%2)").arg(oldGeometry.width()).arg(oldGeometry.height()));
		};

	if (newWindow->isInitializationComplete()) {
		showNewWindow();
	}
	else {
		QObject::connect(newWindow, &DSHHub::initializationComplete,
			newWindow, showNewWindow);
	}
}