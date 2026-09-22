#include "common/settings/ClientSettings.h"
#include "core/DSHHub.h"
#include "common/util/Logger.h"
#include "common/appearance/ThemeManager.h"
#include "ui/Tooltip.h"
#include "common/appearance/TranslationManager.h"

#include <QCoreApplication>
#include <QFont>
#include <QStyleHints>
#include <QtWidgets/QApplication>

int main(int argc, char* argv[])
{
	QApplication app(argc, argv);

	Logger::init();
	TimingLogger::mark(QStringLiteral("Logger init (baseline)"));

	// 运行目录的外观设置文件（ClientSetting/AppearanceSetting.json）：
	// 首次运行落一份带默认值的模板，用户才找得到也改得动；已存在则一个字都不动。
	AppearanceSetting::ensureFile();

	// 界面语言：按保存的设置 -> 系统语言 -> 默认语言（中文）的顺序装 QTranslator。
	// 必须在创建任何窗口之前：窗口构造时的 qtTrId("...") 就要按目标语言取文案。
	Translation::init();
	TimingLogger::mark(QStringLiteral("translation init"));

	// 主题：设置文件里明确写了 light/dark 就照它（用户在标题栏切过主题），
	// 没写或写 system 才跟随系统颜色模式。
	const AppearanceSetting::ThemeMode savedThemeMode = AppearanceSetting::themeMode();
	const bool dark = savedThemeMode == AppearanceSetting::ThemeMode::System
		? app.styleHints()->colorScheme() == Qt::ColorScheme::Dark
		: savedThemeMode == AppearanceSetting::ThemeMode::Dark;

	// 先加载样式表，再创建主窗口：
	// 默认样式模板缺失时从 qrc 释放到 exe 同目录 styles/（已存在不覆盖），
	// 然后按主题读调色板、合成各板块 QSS 并安装到 QApplication。
	const QString stylesDir = QCoreApplication::applicationDirPath()
		+ QStringLiteral("/styles");
	ThemeManager::instance().init(stylesDir,
		dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
	TimingLogger::mark(QStringLiteral("theme styles init"));

	// 悬浮提示：装应用级事件过滤器，接管所有 QEvent::ToolTip，换成自绘气泡。
	// 必须在主题之后 —— 它要把当前样式表挂到气泡窗口上（见 Tooltip.h）。
	// 既有的 setToolTip(...) 调用点一个都不用改，文案仍从 widget->toolTip() 取。
	Tooltip::install();
	TimingLogger::mark(QStringLiteral("tooltip install"));

	// 更现代的标准字体：Windows 下优先使用 Microsoft YaHei UI
	QFont font(QStringLiteral("Microsoft YaHei UI"));
	font.setPointSize(10);
	app.setFont(font);

	auto* window = new DSHHub;
	window->show();
	return app.exec();
}