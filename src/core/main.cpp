#include "DSHHub.h"
#include "Logger.h"
#include "ThemeManager.h"

#include <QCoreApplication>
#include <QFont>
#include <QStyleHints>
#include <QtWidgets/QApplication>

int main(int argc, char* argv[])
{
	QApplication app(argc, argv);

	Logger::init();

	// 根据系统颜色模式自动切换亮色/暗色主题
	const bool dark = app.styleHints()->colorScheme() == Qt::ColorScheme::Dark;

	// 先加载样式表，再创建主窗口：
	// 默认样式模板缺失时从 qrc 释放到 exe 同目录 styles/（已存在不覆盖），
	// 然后按主题读调色板、合成各板块 QSS 并安装到 QApplication。
	const QString stylesDir = QCoreApplication::applicationDirPath()
		+ QStringLiteral("/styles");
	Theme::init(stylesDir, dark ? Theme::Mode::Dark : Theme::Mode::Light);

	// 更现代的标准字体：Windows 下优先使用 Microsoft YaHei UI
	QFont font(QStringLiteral("Microsoft YaHei UI"));
	font.setPointSize(10);
	app.setFont(font);

	auto* window = new DSHHub;
	window->show();
	return app.exec();
}
