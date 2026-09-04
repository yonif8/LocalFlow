#pragma once

#include "ModelManager.hpp"
#include "LearnedTermModel.hpp"
#include "PlatformBridge.hpp"
#include "SettingsModel.hpp"
#include "UpdateManager.hpp"

#include <QObject>
#include <QPointer>
#include <QStringListModel>
#include <QVariantList>

#include <map>
#include <cstdint>
#include <memory>
#include <optional>

class QMenu;
class QSystemTrayIcon;
class QWindow;

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY trayAvailableChanged)
    Q_PROPERTY(double inputLevel READ inputLevel NOTIFY inputLevelChanged)
    Q_PROPERTY(QString capabilitySummary READ capabilitySummary NOTIFY capabilitySummaryChanged)
    Q_PROPERTY(QVariantList capabilities READ capabilities NOTIFY capabilitySummaryChanged)
    Q_PROPERTY(bool platformReady READ platformReady NOTIFY capabilitySummaryChanged)
    Q_PROPERTY(QString diagnosticsReport READ diagnosticsReport NOTIFY capabilitySummaryChanged)
    Q_PROPERTY(QString attentionText READ attentionText NOTIFY attentionChanged)
    Q_PROPERTY(QString recoveryTranscript READ recoveryTranscript NOTIFY attentionChanged)
    Q_PROPERTY(bool attentionRequired READ attentionRequired NOTIFY attentionChanged)
    Q_PROPERTY(SettingsModel* settings READ settings CONSTANT)
    Q_PROPERTY(ModelManager* models READ models CONSTANT)
    Q_PROPERTY(UpdateManager* updates READ updates CONSTANT)
    Q_PROPERTY(QAbstractItemModel* history READ history CONSTANT)
    Q_PROPERTY(QAbstractItemModel* learnedTerms READ learnedTerms CONSTANT)
    Q_PROPERTY(QVariantList microphones READ microphones NOTIFY microphonesChanged)

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    QString state() const { return state_; }
    QString statusText() const;
    bool listening() const { return listening_; }
    bool trayAvailable() const { return trayAvailable_; }
    double inputLevel() const { return inputLevel_; }
    QString capabilitySummary() const { return capabilitySummary_; }
    QVariantList capabilities() const { return capabilities_; }
    bool platformReady() const { return platformReady_; }
    QString diagnosticsReport() const;
    QString attentionText() const { return attentionText_; }
    QString recoveryTranscript() const { return recoveryTranscript_; }
    bool attentionRequired() const { return !attentionText_.isEmpty(); }
    SettingsModel* settings() { return &settings_; }
    ModelManager* models() { return &models_; }
    UpdateManager* updates() { return &updates_; }
    QAbstractItemModel* history() { return &history_; }
    QAbstractItemModel* learnedTerms() { return &learnedTerms_; }
    QVariantList microphones() const { return microphones_; }

    void installTray();

    Q_INVOKABLE void showSettings();
    Q_INVOKABLE void showOnboarding();
    Q_INVOKABLE void hideSettings();
    Q_INVOKABLE void toggleListening();
    Q_INVOKABLE void cancelDictation();
    Q_INVOKABLE void clearHistory();
    Q_INVOKABLE void copyHistoryItem(int row);
    Q_INVOKABLE void copyRecoveryTranscript();
    Q_INVOKABLE void dismissAttention();
    Q_INVOKABLE void refreshMicrophones();
    Q_INVOKABLE void refreshCapabilities();
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void openDiagnostics();
    Q_INVOKABLE void copyDiagnostics();
    Q_INVOKABLE void openIssue();
    Q_INVOKABLE void quit();

signals:
    void stateChanged();
    void listeningChanged();
    void trayAvailableChanged();
    void inputLevelChanged();
    void capabilitySummaryChanged();
    void attentionChanged();
    void settingsRequested();
    void updatesRequested();
    void diagnosticsRequested();
    void settingsDismissed();
    void onboardingRequested();
    void microphonesChanged();

private:
    struct RuntimeState;
    struct PressContext;

    void rebuildTrayMenu();
    void startListening();
    void stopListening();
    void restartListening();
    bool applyPendingListeningRestartIfSafe();
    void handlePlatformEvent(PlatformEvent event);
    void runPipeline(PlatformEvent event, PressContext context);
    PlatformConfiguration platformConfiguration() const;
    void setState(QString state, double inputLevel = 0.0);
    void synchronizeLearnedTerms();

    SettingsModel settings_;
    ModelManager models_;
    LearnedTermModel learnedTerms_;
    UpdateManager updates_;
    QString state_ = QStringLiteral("idle");
    bool listening_ = false;
    bool trayAvailable_ = false;
    double inputLevel_ = 0.0;
    QString capabilitySummary_ = QStringLiteral("Checking system capabilities…");
    QVariantList capabilities_;
    bool platformReady_ = false;
    QString attentionText_;
    QString recoveryTranscript_;
    QStringListModel history_;
    QVariantList microphones_;
    QSystemTrayIcon* tray_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    std::unique_ptr<RuntimeState> runtime_;
    std::map<std::uint64_t, PressContext> pressContexts_;
    std::optional<std::uint64_t> activePressSession_;
    std::uint64_t listeningGeneration_ = 0;
    bool startAfterPipeline_ = false;
    bool pendingListeningRestart_ = false;
    bool pendingRestartMayRecoverError_ = false;
    bool pendingLearnedTermsSync_ = false;
    std::uint64_t learnedTermsRevision_ = 0;
    bool silentUpdateCheckInFlight_ = false;
    QString lastNotifiedUpdateKey_;
};
