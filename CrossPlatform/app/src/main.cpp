#include "AppController.hpp"

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

int main(int argc, char* argv[]) {
    QApplication::setOrganizationName(QStringLiteral("LocalFlow"));
    QApplication::setOrganizationDomain(QStringLiteral("localflow.app"));
    QApplication::setApplicationName(QStringLiteral("LocalFlow"));
    QApplication::setApplicationVersion(QStringLiteral(LOCALFLOW_VERSION));
    QApplication::setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QApplication application(argc, argv);
    AppController controller;
    controller.installTray();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("LocalFlowApp"), &controller);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    engine.loadFromModule(QStringLiteral("LocalFlow"), QStringLiteral("Main"));
#else
    engine.load(QUrl(QStringLiteral("qrc:/LocalFlow/qml/Main.qml")));
#endif
    if (engine.rootObjects().isEmpty()) return 1;
    if (controller.models()->ready()) {
        controller.toggleListening();
    } else {
        controller.showOnboarding();
    }
    return application.exec();
}
