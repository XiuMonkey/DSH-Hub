#include "ThemeManager.h"

#include "ClientSettings.h"
#include "DSHHub.h"
#include "ShadowPanel.h"
#include "SpinnerWidget.h"

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

namespace Theme
{
	namespace
	{
		const QString kResourcePrefix = QStringLiteral(":/DSHHub/styles/");
		// 各板块样式文件（与 resources/styles 下的默认模板一一对应）。
		//
		// 组织约定：一个文件 = 一个界面/表面，文件名就是它负责的那个界面；
		// 每个文件的头部注释写明「归属」与「别把别的板块写进来」。
		// 顺序 = 级联顺序（同优先级的选择器后来者胜），所以下面这份列表的顺序有意义：
		// 全局默认（外壳/通用控件/滚动条）在前，各界面在后，便于界面覆盖默认。
		const QStringList kModules = {
			QStringLiteral("main-window.qss"),      // 主窗口外壳与自绘标题栏
			QStringLiteral("defaults.qss"),         // 通用控件默认外观（各界面覆盖它）
			QStringLiteral("scrollbars.qss"),       // 全局滚动条（唯一一份滚动条规则）
			QStringLiteral("chat.qss"),             // 对话列
			QStringLiteral("sidebar.qss"),          // 左侧会话栏
			QStringLiteral("popups.qss"),           // 浮层与各窗口遮罩、弹窗外壳
			QStringLiteral("tooltip.qss"),          // 悬浮提示气泡（同为浮层，紧跟弹窗外壳）
			QStringLiteral("extension-manager.qss"),// 扩展管理窗口
			QStringLiteral("topbar.qss"),           // 对话顶栏与工具过滤窗口
			QStringLiteral("settings.qss"),         // 设置窗口
			QStringLiteral("model-list.qss"),       // 模型列表面板（设置 → 模型列表）
			QStringLiteral("plugin-market.qss"),    // 插件市场窗口
		};

		QString resourcePath(const QString& fileName)
		{
			return kResourcePrefix + fileName;
		}

		QString modeKey(Theme::Mode mode)
		{
			return mode == Theme::Mode::Dark ? QStringLiteral("dark") : QStringLiteral("light");
		}

		struct Impl
		{
			QString stylesDir;                 // 外部可覆盖目录（通常 exe 同目录 /styles）
			Mode mode = Mode::Light;

			// 当前生效主题的镜像（供 color()/styleSheet()/applyToWindow() 快速读取）
			QHash<QString, QString> palette;
			QString qss;

			// 两套主题的预合成缓存：键 = modeKey()；切主题只做缓存命中，不做字符串计算
			QHash<QString, QHash<QString, QString>> paletteCache;
			QHash<QString, QString> qssCache;

			QString paletteFileFor(Mode m) const
			{
				return m == Mode::Dark
					? QStringLiteral("theme-dark.json")
					: QStringLiteral("theme-light.json");
			}

			// 释放默认模板：stylesDir 里缺失的文件从 qrc 拷出；已存在不覆盖
			// （用户定制优先）。
			//
			// 代价是：程序发布的新版模板会被旧的同名外部文件盖住——界面看不出报错，
			// 只是新控件的样式一直不生效。这里对“已存在但与内置模板不同”的文件打一条
			// 提示（不覆盖，也不删用户的东西），把这种沉默的失效变成可查的日志。
			void ensureDefaults()
			{
				QDir dir(stylesDir);
				if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
					qWarning().noquote() << "[Theme] cannot create styles dir:" << stylesDir;
					return;
				}

				const QStringList files = QStringList(kModules)
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

						// 与内置模板不一致时，这份外部文件会在 loadText() 里胜出。
						// 只提示，不动它：它可能是用户自己改的。
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
			QString loadText(const QString& fileName) const
			{
				const QString external = QDir(stylesDir).filePath(fileName);
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
			QHash<QString, QString> loadPaletteFor(Mode m) const
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

			// 把 {{key}} 占位替换成色板颜色（找不到的 key 原样保留以便排查）
			static QString substituteWith(const QString& qss,
				const QHash<QString, QString>& palette)
			{
				static const QRegularExpression token(
					QStringLiteral("\\{\\{\\s*([A-Za-z][A-Za-z0-9]*)\\s*\\}\\}"));

				QString out;
				out.reserve(qss.size());

				int last = 0;
				QRegularExpressionMatchIterator it = token.globalMatch(qss);
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

			// 用给定色板合成一份完整 QSS（模块按 kModules 顺序拼接）
			QString composeQssFor(const QHash<QString, QString>& palette) const
			{
				QString combined;
				for (const QString& module : kModules) {
					const QString text = loadText(module);
					if (!text.isEmpty())
						combined += substituteWith(text, palette) + QStringLiteral("\n\n");
				}
				return combined;
			}

			// 计算单个主题的（色板, QSS）——纯计算，供工作线程调用
			std::pair<QHash<QString, QString>, QString> buildFor(Mode m) const
			{
				const auto pal = loadPaletteFor(m);
				return std::make_pair(pal, composeQssFor(pal));
			}

			// 预合成两套主题（计算放后台线程），结果写回缓存
			void buildAllCaches()
			{
				auto light = std::async(std::launch::async, [this]() { return buildFor(Mode::Light); });
				auto dark = std::async(std::launch::async, [this]() { return buildFor(Mode::Dark); });

				const auto lightResult = light.get();
				const auto darkResult = dark.get();
				paletteCache.insert(modeKey(Mode::Light), lightResult.first);
				qssCache.insert(modeKey(Mode::Light), lightResult.second);
				paletteCache.insert(modeKey(Mode::Dark), darkResult.first);
				qssCache.insert(modeKey(Mode::Dark), darkResult.second);
			}

			// 若某个主题还没合成过，同步补一次（reload/reset 后首次读取用）
			void ensureCached(Mode m)
			{
				const QString key = modeKey(m);
				if (qssCache.contains(key))
					return;
				const auto result = buildFor(m);
				paletteCache.insert(key, result.first);
				qssCache.insert(key, result.second);
			}

			// 把某个主题的缓存镜像到当前生效成员
			void activate(Mode m)
			{
				mode = m;
				palette = paletteCache.value(modeKey(m));
				qss = qssCache.value(modeKey(m));
			}
		};

		Impl& impl()
		{
			static Impl instance;
			return instance;
		}

		// 生成并应用与当前主题一致的 QPalette：QSS 没覆盖到的默认文字/底色
		// （输入框文字、列表项、上下文菜单等）在暗色模式下会退化成 Qt 默认的
		// 亮色调色板（黑字），这里统一按当前色板修正。
		QString token(const QHash<QString, QString>& p, const QString& key,
			const QString& fallback)
		{
			const auto it = p.constFind(key);
			return it == p.constEnd() ? fallback : it.value();
		}

		void installPaletteFor(const QHash<QString, QString>& p)
		{
			QPalette pal = QApplication::palette();
			const QColor window(token(p, QStringLiteral("windowBg"), QStringLiteral("#FFFFFF")));
			const QColor panel(token(p, QStringLiteral("panelBg"), QStringLiteral("#FFFFFF")));
			const QColor text(token(p, QStringLiteral("textPrimary"), QStringLiteral("#000000")));
			const QColor textDim(token(p, QStringLiteral("textSecondary"), QStringLiteral("#666666")));
			const QColor accent(token(p, QStringLiteral("accent"), QStringLiteral("#4C8BF5")));
			const QColor onAccent(token(p, QStringLiteral("textOnAccent"), QStringLiteral("#FFFFFF")));
			const QColor border(token(p, QStringLiteral("border"), QStringLiteral("#E5E7EB")));

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
	} // namespace

	// 全局兜底：任何滚动区**首次显示**时补一次滚动条解析。
	//   · 有了它，"新控件忘了调 repolishScrollArea" 不会再变成视觉 bug；
	//   · 显式调用仍然有意义：它在第一次绘制前就弄对了，不会闪一下。
	// 挂在 qApp 上（见 init()），只在 Show 事件上做一次廉价的 qobject_cast。
	// （repolishScrollArea 的声明在头文件里，所以这里定义在它之前也没问题。）
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
						// Show 派发期间重新解析会搅乱事件流：排到本轮之后；以控件自身为
						// 上下文，控件先销毁时这次调用自动作废。
						// repolishScrollArea 自带"每个控件只挂一次补丁"的标记，重复调用无害。
						QMetaObject::invokeMethod(area, [area]() { repolishScrollArea(area); },
							Qt::QueuedConnection);
					}
				}
				return QObject::eventFilter(watched, event);
			}
		};
	} // namespace

	void init(const QString& stylesDir, Mode mode)
	{
		Impl& s = impl();
		s.stylesDir = stylesDir;
		s.ensureDefaults();

		// 计算放线程：启动时一次性预合成亮/暗两套（切主题时主线程零计算）
		s.buildAllCaches();

		setMode(mode);

		// 全局兜底：滚动区首次显示时补一次滚动条解析（见 ScrollAreaShowFilter）。
		// 这样"新加滚动区忘了调 Theme::repolishScrollArea()"不会再退回原生老式滚动条。
		static bool showFilterInstalled = false;
		if (!showFilterInstalled && qApp) {
			showFilterInstalled = true;
			qApp->installEventFilter(new ScrollAreaShowFilter(qApp));
		}

		qInfo().noquote() << "[Theme] styles initialized from:" << stylesDir
			<< "mode=" << (mode == Mode::Dark ? "Dark" : "Light")
			<< "paletteKeys=" << s.palette.size();
	}

	void setMode(Mode mode)
	{
		Impl& s = impl();
		s.ensureCached(mode);
		s.activate(mode);
		// 同步应用 QPalette（QSS 未覆盖的默认文字/底色随主题走）
		installPaletteFor(s.palette);
		qInfo().noquote() << "[Theme] set mode:" << (mode == Mode::Dark ? "Dark" : "Light")
			<< "qssBytes=" << s.qss.size();
	}

	// 把当前合成样式表安装到单个窗口（及其子树）。
	// 各顶层窗口（主窗 / 弹窗 / 主题切换卡）自行调用；切主题时旧窗口不再全局重 polish。
	void applyToWindow(QWidget* window)
	{
		if (window)
			window->setStyleSheet(impl().qss);
	}

	void reload()
	{
		Impl& s = impl();
		// 重新从磁盘/资源合成（含用户改动后的外部文件）
		s.buildAllCaches();
		s.activate(s.mode);
		installPaletteFor(s.palette);

		// 重挂到所有顶层窗口（开发期热调 / 重置默认后）
		const QList<QWidget*> topLevels = QApplication::topLevelWidgets();
		for (QWidget* window : topLevels)
			applyToWindow(window);
		qInfo().noquote() << "[Theme] styles reloaded from:" << s.stylesDir;
	}

	void resetStyles()
	{
		Impl& s = impl();
		if (s.stylesDir.isEmpty()) {
			qWarning().noquote() << "[Theme] resetStyles called before init";
			return;
		}

		// 只删除已知的默认模板文件（qss + 色板），保留目录里其它可能存在的文件
		QDir dir(s.stylesDir);
		const QStringList files = QStringList(kModules)
			<< QStringLiteral("theme-light.json")
			<< QStringLiteral("theme-dark.json");
		for (const QString& file : files) {
			const QString target = dir.filePath(file);
			if (QFile::exists(target) && !QFile::remove(target))
				qWarning().noquote() << "[Theme] cannot remove style file:" << target;
		}

		s.ensureDefaults();
		reload();
		qInfo().noquote() << "[Theme] styles reset to defaults in:" << s.stylesDir;
	}

	bool isDark()
	{
		return impl().mode == Mode::Dark;
	}

	QString color(const QString& key)
	{
		const Impl& s = impl();
		const auto it = s.palette.constFind(key);
		if (it != s.palette.constEnd())
			return it.value();
		return QStringLiteral("#000000");
	}

	// 见头文件注释：滚动条是基类构造时建好的，那时子类的 objectName 还没设，
	// QStyleSheetStyle 会把"匹配不到规则"缓存下来。
	//
	// 光在构造后解析一次还不够：样式表是**按顶层窗口**挂的（见 applyToWindow），
	// 控件在构造时可能还没接进那个窗口（预构建的控件树、先建后插的卡片都属于这种），
	// 或者那条规则要等滚动条真的出现才谈得上。表现就是"首次渲染是原生老式滚动条，
	// 重启/切主题后又好了"——重启会重新 setStyleSheet，整棵子树被重新解析。
	//
	// 所以这里一次挂好两个只跑一次的补丁：
	//   1) 首次 Show 之后再解析一次（那时控件一定已经在带样式表的窗口里）；
	//   2) 任一滚动条第一次真的有范围（= 它真的会出现）时再解析一次。
	// 补丁跑完自动摘掉，对滚动列表没有持续开销。
	//
	// 另有一层全局兜底 ScrollAreaShowFilter（见 init()）：任何滚动区首次显示都会被
	// 抓一次，所以**新控件忘记调用也不会再退回原生滚动条**；显式调用仍然值得做，
	// 它让控件在第一次绘制之前就已经是对的样子。
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
						repolishScrollArea(m_area);
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
						repolishScrollArea(m_area);
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

	void repolishScrollArea(QWidget* widget)
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

		// 构造期解析可能太早（那时还没接进带样式表的窗口）：挂上"首次显示/首次
		// 真出现滚动条"的补丁（每个控件只挂一次）
		static const char* const kHooked = "dshScrollAreaRepolishHooked";
		if (area->property(kHooked).toBool())
			return;
		area->setProperty(kHooked, true);
		new ScrollBarRepolisher(area);
	}

	void switchTheme(QWidget* currentWindow)
	{
		// 先构造并显示“切换中”过渡卡片（使用当前主题），确保点击后立刻出现，
		// 而不是等 setMode（重建 + 全应用重设样式表，较耗时）做完才显示。
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

		if (currentWindow)
			currentWindow->hide();

		// 再切换主题：预合成缓存命中，主线程只做缓存取用，不再重建字符串
		const Mode nextMode = isDark() ? Mode::Light : Mode::Dark;

		// 记进 AppearanceSetting.json：用户已经显式选过主题了，下次启动就照它，
		// 不再跟随系统（把 system 固化成明确的 light/dark）。写在 setMode 之前，
		// 免得后面重建窗口若出岔子，用户的选择反而丢了。
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
			// 带令牌的 URL：0.1.5 的 /api 需要 cookie，而 cookie 只能用启动令牌换，
			// 只传域名端口的话新窗口会 401（表现为"切主题后窗口不再出现"）。
			oldBaseUrl = oldHub->authenticatedBaseUrl();
			oldServerProcess = oldHub->takeServerProcess();
		}

		auto* newWindow = new DSHHub(nullptr, oldBaseUrl, oldServerProcess);
		newWindow->hide();

		const auto showNewWindow = [currentWindow, newWindow, popup]() {
			popup->close();
			popup->deleteLater();

			if (currentWindow)
				currentWindow->deleteLater();

			newWindow->show();
			qInfo().noquote() << "[Theme] switch theme completed";
			};

		if (newWindow->isInitializationComplete()) {
			showNewWindow();
		}
		else {
			QObject::connect(newWindow, &DSHHub::initializationComplete,
				newWindow, showNewWindow);
		}
	}
}