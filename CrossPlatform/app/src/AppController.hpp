#pragma once

#include "ModelManager.hpp"
#include "PlatformBridge.hpp"
#include "SettingsModel.hpp"

#include <QObject>
#include <QPointer>
#include <QStringListModel>

#include <map>
#include <memory>

class QMenu;
class QSystemTrayIcon;
class QWindow;

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
    Q_PROPERTY(double inputLevel READ inputLevel NOTIFY inputLevelChanged)
    Q_PROPERTY(QString capabilitySummary READ capabilitySummary NOTIFY capabilitySummaryChanged)
    Q_PROPERTY(SettingsModel* settings READ settings CONSTANT)
    Q_PROPERTY(ModelManager* models READ models CONSTANT)
    Q_PROPERTY(QAbstractItemModel* history READ history CONSTANT)

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    QString state() const { return state_; }
    QString statusText() const;
    bool listening() const { return listening_; }
    double inputLevel() const { return inputLevel_; }
    QString capabilitySummary() const { return capabilitySummary_; }
    SettingsModel* settings() { return &settings_; }
    ModelManager* models() { return &models_; }
    QAbstractItemModel* history() { return &history_; }

    void installTray();

    Q_INVOKABLE void showSettings();
    Q_INVOKABLE void showOnboarding();
    Q_INVOKABLE void hideSettings();
    Q_INVOKABLE void toggleListening();
    Q_INVOKABLE void clearHistory();
    Q_INVOKABLE void copyHistoryItem(int row);
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void openDiagnostics();
    Q_INVOKABLE void quit();

signals:
    void stateChanged();
    void listeningChanged();
    void inputLevelChanged();
    void capabilitySummaryChanged();
    void settingsRequested();
    void settingsDismissed();
    void onboardingRequested();
    void updateCheckRequested();

private:
    struct RuntimeState;
    struct PressContext;

    void rebuildTrayMenu();
    void startListening();
    void stopListening();
    void restartListening();
    void handlePlatformEvent(PlatformEvent event);
    void runPipeline(PlatformEvent event, PressContext context);
    PlatformConfiguration platformConfiguration() const;
    void setState(QString state, double inputLevel = 0.0);

    SettingsModel settings_;
    ModelManager models_;
    QString state_ = QStringLiteral("idle");
    bool listening_ = false;
    double inputLevel_ = 0.0;
    QString capabilitySummary_ = QStringLiteral("Checking system capabilities…");
    QStringListModel history_;
    QSystemTrayIcon* tray_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    std::unique_ptr<RuntimeState> runtime_;
    std::map<std::uint64_t, PressContext> pressContexts_;
};
