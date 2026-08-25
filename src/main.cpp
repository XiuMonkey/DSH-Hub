#include "DSHHub.h"
#include "ThemeManager.h"

#include <QFont>
#include <QStyleHints>
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 根据系统颜色模式自动切换亮色/暗色主题
    Theme::setMode(app.styleHints()->colorScheme() == Qt::ColorScheme::Dark
                       ? Theme::Mode::Dark
                       : Theme::Mode::Light);

    // 更现代的标准字体：Windows 下优先使用 Microsoft YaHei UI
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSize(10);
    app.setFont(font);

    auto *window = new DSHHub;
    window->show();
    return app.exec();
}
