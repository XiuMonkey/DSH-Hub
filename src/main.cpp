#include "DSHHub.h"

#include <QFont>
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 更现代的标准字体：Windows 下优先使用 Microsoft YaHei UI
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSize(10);
    app.setFont(font);

    DSHHub window;
    window.show();
    return app.exec();
}
