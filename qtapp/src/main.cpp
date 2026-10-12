#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    /* 与 Config 的组织/应用名保持一致（QSettings 落点） */
    QApplication::setOrganizationName(QStringLiteral("Greenhouse"));
    QApplication::setApplicationName(QStringLiteral("LightControl"));

    MainWindow w;
    w.show();
    return app.exec();
}
