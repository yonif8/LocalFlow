#include "AppController.hpp"
#include "SingleInstance.hpp"
#include "localflow/inference/NemoTranscriber.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QLockFile>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <cstring>

namespace {
QString instanceName() {
    const QByteArray identity = QCryptographicHash::hash(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).toUtf8(),
        QCryptographicHash::Sha256).toHex().left(20);
    return QStringLiteral("LocalFlow-") + QString::fromLatin1(identity);
}

}

int main(int argc, char* argv[]) {
    bool smokeUi = false;
    bool background = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--probe-runtime") == 0) {
            const auto probe = localflow::inference::NemoTranscriber::probeRuntime();
            std::fputs(probe ? "{\"ready\":true}\n" : "{\"ready\":false}\n", stdout);
            return probe ? 0 : 4;
        }
        if (std::strcmp(argv[index], "--smoke-ui") == 0) smokeUi = true;
        if (std::strcmp(argv[index], "--background") == 0) background = true;
    }
    QApplication::setOrganizationName(QStringLiteral("LocalFlow"));
    QApplication::setOrganizationDomain(QStringLiteral("localflow.app"));
    QApplication::setApplicationName(QStringLiteral("LocalFlow"));
    QApplication::setApplicationVersion(QStringLiteral(LOCALFLOW_VERSION));
    QApplication::setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QApplication application(argc, argv);
    const QString serverName = instanceName();
    const QString lockDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(lockDirectory);
    QLockFile instanceLock(lockDirectory + QStringLiteral("/LocalFlow.instance.lock"));
    localflow::app::SingleInstanceServer instanceServer;
    if (!smokeUi) {
        const auto command = background ? localflow::app::InstanceCommand::background
                                        : localflow::app::InstanceCommand::activate;
        if (localflow::app::notifyExistingInstance(serverName, command)) return 0;
        if (!instanceLock.tryLock()) {
            // The first process may have acquired its lock immediately before
            // opening the local activation socket. Give that short startup
            // window a second chance without ever creating two controllers.
            if (localflow::app::notifyStartingInstance(serverName, command)) return 0;
            if (!instanceLock.removeStaleLockFile() || !instanceLock.tryLock()) {
                if (!background) {
                    QMessageBox::information(
                        nullptr, QStringLiteral("LocalFlow is starting"),
                        QStringLiteral("Another LocalFlow process is already starting. Its window will be available shortly."));
                }
                return 0;
            }
        }
        QString instanceError;
        if (!instanceServer.listen(serverName, &instanceError)) {
            if (!background) {
                QMessageBox::warning(
                    nullptr, QStringLiteral("LocalFlow could not start"),
                    QStringLiteral("LocalFlow could not create its private activation channel. Please close any existing LocalFlow process and try again.\n\n%1")
                        .arg(instanceError));
            }
            return 2;
        }
    }

    AppController controller;
    if (!smokeUi) controller.installTray();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("LocalFlowApp"), &controller);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    engine.loadFromModule(QStringLiteral("LocalFlow"), QStringLiteral("Main"));
#else
    engine.load(QUrl(QStringLiteral("qrc:/LocalFlow/qml/Main.qml")));
#endif
    if (engine.rootObjects().isEmpty()) return 1;
    if (!smokeUi) {
        instanceServer.setCommandHandler([&controller](const auto command) {
            if (command != localflow::app::InstanceCommand::activate) return;
            if (controller.models()->ready()) controller.showSettings();
            else controller.showOnboarding();
        });
    }
    if (smokeUi) {
        QTimer::singleShot(250, &application, &QCoreApplication::quit);
        return application.exec();
    }
    if (controller.models()->ready()) {
        controller.toggleListening();
    } else {
        controller.showOnboarding();
    }
    return application.exec();
}
