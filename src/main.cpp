#include <QApplication>
#include <QStyleFactory>
#include "mainwindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("岩心光谱智能解译系统"));
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    app.setOrganizationName(QStringLiteral("地调中心"));

    // Dark style
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette dark;
    dark.setColor(QPalette::Window,          QColor(30,  32,  48));
    dark.setColor(QPalette::WindowText,      QColor(238, 242, 255));
    dark.setColor(QPalette::Base,            QColor(22,  24,  37));
    dark.setColor(QPalette::AlternateBase,   QColor(35,  38,  58));
    dark.setColor(QPalette::Text,            QColor(238, 242, 255));
    dark.setColor(QPalette::Button,          QColor(44,  47,  73));
    dark.setColor(QPalette::ButtonText,      QColor(238, 242, 255));
    dark.setColor(QPalette::Highlight,       QColor(80,  250, 123));
    dark.setColor(QPalette::HighlightedText, QColor(22,  24,  37));
    dark.setColor(QPalette::Link,            QColor(139, 233, 253));
    dark.setColor(QPalette::ToolTipBase,     QColor(44,  47,  73));
    dark.setColor(QPalette::ToolTipText,     QColor(238, 242, 255));
    app.setPalette(dark);

    MainWindow w;
    w.show();
    return app.exec();
}
