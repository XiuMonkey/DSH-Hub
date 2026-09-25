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

// 常量走函数式 static，避免静态初始化期构造 QString（Qt 老坑）
const QString& ThemeManager::resourcePrefix()
{
	static const QString prefix = QStringLiteral(":/DSHHub/styles/");
	return prefix;
}

const QStringList& ThemeManager::modules()
{
	// 顺序 = 级联顺序（同优先级后来者胜），全局默认在前、各界面在后
	static const QStringList list = {
		QStringLiteral("main-window.qss"),
		QStringLiteral("defaults.qss"),
		QStringLiteral("scrollbars.qss"),
		QStringLiteral("chat.qss"),
		QStringLiteral("sidebar.qss"),  // 左侧会话栏
		QStringLiteral("popups.qss"),  // 浮层与遮罩
		QStringLiteral("tooltip.qss"),  // 悬浮提示气泡
		QStringLiteral("extension-manager.qss"),
		QStringLiteral("topbar.qss"),  // 对话顶栏
		QStringLiteral("settings.qss"),
		QStringLiteral("model-list.qss"),
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

QString ThemeManager::substituteWith(const QString& qss, const QHash<QString, QString>& palette)
{
	// 局部名别用 token：会遮蔽本类静态成员 token()
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
		// 老主题缺按钮色时回退到 accent，避免占位符原样漏进 QSS
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

QString ThemeManager::token(const QHash<QString, QString>& palette, const QString& key,
	const QString& fallback)
{
	const auto it = palette.constFind(key);
	return it == palette.constEnd() ? fallback : it.value();
}

// 把色板落成 QPalette（未覆盖的文字/底色在暗色下会退化成 Qt 亮色）；换全局调色板有副作用，
// 故留在 private，外部只能经 setMode()/reload() 间接触发
void ThemeManager::installPaletteFor(const QHash<QString, QString>& palette)
{
	QPalette pal = QApplication::palette();
	// 色值可能是 CSS rgba(...)，QColor 认不出；一律走 CardShadow::parseColor
	const QColor window(CardShadow::parseColor(token(palette, QStringLiteral("windowBg"), QStringLiteral("#FFFFFF"))));
	const QColor panel(CardShadow::parseColor(token(palette, QStringLiteral("panelBg"), QStringLiteral("#FFFFFF"))));
	const QColor text(CardShadow::parseColor(token(palette, QStringLiteral("textPrimary"), QStringLiteral("#000000"))));
	const QColor textDim(CardShadow::parseColor(token(palette, QStringLiteral("textSecondary"),
		QStringLiteral("#666666"))));
	const QColor accent(CardShadow::parseColor(token(palette, QStringLiteral("accent"), QStringLiteral("#4C8BF5"))));
	const QColor onAccent(CardShadow::parseColor(token(palette, QStringLiteral("textOnAccent"),
		QStringLiteral("#FFFFFF"))));
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
	// 刻意不析构：已登记进 CommonRegistry，静态析构顺序无从保证
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

// 缺失模板从 qrc 拷出，已存在不覆盖（用户定制优先）；与内置不同的只提示，不动用户文件
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

// 外部优先、qrc 兜底
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

// 纯计算，供工作线程调用
std::pair<QHash<QString, QString>, QString> ThemeManager::buildFor(Mode m) const
{
	const auto pal = loadPaletteFor(m);
	return std::make_pair(pal, composeQssFor(pal));
}

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

// 全局兜底：任何滚动区首次 Show 后补一次滚动条解析 —— 新控件漏调 repolishScrollArea 也不会退回原生样式
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
					// Show 派发期间重解析会搅乱事件流，故排到本轮之后；控件销毁后调用自动作废
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

	buildAllCaches();

	setMode(mode);

	static bool showFilterInstalled = false;
	if (!showFilterInstalled && qApp) {
		showFilterInstalled = true;
		qApp->installEventFilter(new ScrollAreaShowFilter(qApp));
	}

	// 登记进全局注册表（插件按 index 取出后转 VirtualTheme*）；必须放最后一步，否则插件会被空样式表闸门挡回
	CommonRegistry::instance().AddToRegistry(DshHostIndex::kThemeManager, this);

	qInfo().noquote() << "[Theme] styles initialized from:" << m_stylesDir
		<< "mode=" << (mode == Mode::Dark ? "Dark" : "Light")
		<< "paletteKeys=" << m_palette.size();
}

void ThemeManager::setMode(Mode mode)
{
	ensureCached(mode);
	activate(mode);
	installPaletteFor(m_palette);
	qInfo().noquote() << "[Theme] set mode:" << (m_mode == Mode::Dark ? "Dark" : "Light")
		<< "qssBytes=" << m_qss.size();
}

// 把合成样式表装到单个窗口及其子树；各顶层窗口自行调用
void ThemeManager::applyToWindow(QWidget* window)
{
	if (!window)
		return;

	// 未 init 时 m_qss 为空，setStyleSheet("") 会清掉窗口已有样式；插件可能随时调进来，闸门必须留在这里
	if (m_qss.isEmpty()) {
		qWarning().noquote() << "[Theme] applyToWindow before init (empty qss), skipped";
		return;
	}

	window->setStyleSheet(m_qss);
}

void ThemeManager::ExternalApplyToWindow(QWidget* window)
{
	// VirtualTheme 的实现只转发：插件与宿主必须走同一条路，别加额外语义
	applyToWindow(window);
}

bool ThemeManager::ExternalReloadStyles()
{
	// 未 init 时没有可信的样式目录，reload() 只会把缓存刷成 qrc 兜底的那份并换掉生效镜像
	if (m_stylesDir.isEmpty()) {
		qWarning().noquote() << "[Theme] ExternalReloadStyles before init, refused";
		return false;
	}

	// 插件先覆盖 <exe>/styles/*.qss 再调这里；reload() 不重建窗口，构造期固化的东西（如 logo）不跟着变
	reload();

	// 空样式表 = 一份都没读到，如实回给插件
	if (m_qss.isEmpty()) {
		qWarning().noquote() << "[Theme] ExternalReloadStyles composed an empty stylesheet;"
			" nothing was readable under:" << m_stylesDir;
		return false;
	}
	return true;
}

void ThemeManager::reload()
{
	buildAllCaches();
	activate(m_mode);
	installPaletteFor(m_palette);

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

	// 只删已知默认模板，保留目录里其它文件
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

QString ThemeManager::windowBg() const { return color(QStringLiteral("windowBg")); }
QString ThemeManager::border() const { return color(QStringLiteral("border")); }
QString ThemeManager::textPrimary() const { return color(QStringLiteral("textPrimary")); }
QString ThemeManager::textSecondary() const { return color(QStringLiteral("textSecondary")); }
QString ThemeManager::inputBg() const { return color(QStringLiteral("inputBg")); }
QString ThemeManager::accent() const { return color(QStringLiteral("accent")); }

// 滚动条建在基类构造期，那时 objectName 未设，匹配不到的 QSS 规则会被 QStyleSheetStyle 缓存下来；
// 故挂两个只跑一次的补丁：首次 Show 后、首次真有范围时各重解析一次
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
				// Show 期间重解析会搅乱事件流，挪到本轮事件之后
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

	// 构造期解析可能太早（还没接进带样式表的窗口）：每控件挂一次"首显 / 首现有范围"补丁
	static const char* const kHooked = "dshScrollAreaRepolishHooked";
	if (area->property(kHooked).toBool())
		return;
	area->setProperty(kHooked, true);
	new ScrollBarRepolisher(area);
}

void ThemeManager::switchTheme(QWidget* currentWindow)
{
	// 先显示“切换中”过渡卡片，免得等 setMode 做完才出现
	auto* popup = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Dialog);
	popup->setAttribute(Qt::WA_TranslucentBackground);

	// 阴影外壳取最高一档；整窗尺寸要把它算进去
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
	popup->setFixedSize(360 + cardPad.left() + cardPad.right(), 200 + cardPad.top() + cardPad.bottom());

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
	applyToWindow(popup); // 窗口级安装，不依赖全局 qApp 表
	popup->show();
	popup->raise();
	// 强制先画一帧，否则会与下方 setMode 同帧出现
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

	// 趁旧窗口还在记下状态与几何：新窗口构造时会 resize 成默认尺寸（见 Main.cpp），不接就丢了
	const Qt::WindowStates oldState =
		currentWindow ? currentWindow->windowState() : Qt::WindowStates(Qt::WindowNoState);
	const QRect oldGeometry = currentWindow ? currentWindow->geometry() : QRect();

	if (currentWindow)
		currentWindow->hide();

	const Mode nextMode = isDark() ? Mode::Light : Mode::Dark;

	// 写进 AppearanceSetting.json（显式选过就不再跟随系统），且必须在 setMode 之前写
	AppearanceSetting::setThemeMode(nextMode == Mode::Dark
		? AppearanceSetting::ThemeMode::Dark
		: AppearanceSetting::ThemeMode::Light);

	setMode(nextMode);
	applyToWindow(popup);

	QUrl oldBaseUrl;
	QProcess* oldServerProcess = nullptr;
	if (auto* oldHub = qobject_cast<DSHHub*>(currentWindow)) {
		// 必须传带令牌的 URL：/api 认 cookie，而 cookie 只能由启动令牌换；只传域名端口会 401
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

		// 新窗口构造时会 resize 成默认尺寸，不恢复就丢了几何；最大化 / 全屏交给
		// showMaximized()/showFullScreen()，普通情形先摆几何再 show
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
		QObject::connect(newWindow, &DSHHub::initializationComplete, newWindow, showNewWindow);
	}
}
