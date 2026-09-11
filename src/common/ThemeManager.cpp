#include "ThemeManager.h"

#include "DSHHub.h"
#include "SpinnerWidget.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
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
		// 各板块样式文件（与 resources/styles 下的默认模板一一对应）
		const QStringList kModules = {
			QStringLiteral("base.qss"),
			QStringLiteral("chat.qss"),
			QStringLiteral("sidebar.qss"),
			QStringLiteral("panels.qss"),
			QStringLiteral("settings.qss"),
			QStringLiteral("plugins.qss"),
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
					const QString target = dir.filePath(file);
					if (QFile::exists(target))
						continue; // 已存在 -> 不覆盖（用户定制优先）

					QFile res(resourcePath(file));
					if (!res.open(QIODevice::ReadOnly)) {
						qWarning().noquote() << "[Theme] missing default template in qrc:" << file;
						continue;
					}
					const QByteArray data = res.readAll();
					res.close();

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

					const QString value = palette.value(match.captured(1));
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

	void init(const QString& stylesDir, Mode mode)
	{
		Impl& s = impl();
		s.stylesDir = stylesDir;
		s.ensureDefaults();

		// 计算放线程：启动时一次性预合成亮/暗两套（切主题时主线程零计算）
		s.buildAllCaches();

		setMode(mode);
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

	QString styleSheet()
	{
		return impl().qss;
	}

	// 见头文件注释：滚动条是基类构造时建好的，那时子类的 objectName 还没设，
	// QStyleSheetStyle 会把"匹配不到 #objectName QScrollBar"缓存下来，
	// 这里在设完名字后强制重新解析一次（控件本体 + 两个滚动条）。
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
	}

	void switchTheme(QWidget* currentWindow)
	{
		// 先构造并显示“切换中”过渡卡片（使用当前主题），确保点击后立刻出现，
		// 而不是等 setMode（重建 + 全应用重设样式表，较耗时）做完才显示。
		auto* popup = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Dialog);
		popup->setAttribute(Qt::WA_TranslucentBackground);
		popup->setFixedSize(360, 200);

		auto* outerLayout = new QVBoxLayout(popup);
		outerLayout->setContentsMargins(1, 1, 1, 1);

		auto* body = new QWidget(popup);
		body->setObjectName(QStringLiteral("themeSwitchBody"));
		body->setAttribute(Qt::WA_StyledBackground, true);
		outerLayout->addWidget(body);

		auto* layout = new QVBoxLayout(body);
		layout->setContentsMargins(24, 20, 24, 20);
		layout->setSpacing(12);

		auto* spinner = new SpinnerWidget(body);
		spinner->setFixedSize(40, 40);
		spinner->start();
		layout->addWidget(spinner, 0, Qt::AlignHCenter);

		auto* label = new QLabel(QStringLiteral("正在切换主题..."), body);
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
		setMode(isDark() ? Mode::Light : Mode::Dark);
		// 让过渡卡立刻换上新主题
		applyToWindow(popup);

		// 切换主题时复用当前 DSH server，不创建新 server
		QUrl oldBaseUrl;
		QProcess* oldServerProcess = nullptr;
		if (auto* oldHub = qobject_cast<DSHHub*>(currentWindow)) {
			oldBaseUrl = oldHub->baseUrl();
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