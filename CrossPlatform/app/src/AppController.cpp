#include "AppController.hpp"

#include "PolishWorkerClient.hpp"
#include "localflow/core/audio_resampler.hpp"
#include "localflow/core/dictation_pipeline.hpp"
#include "localflow/inference/NemoTranscriber.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFutureWatcher>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMetaObject>
#include <QSettings>
#include <QStyle>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QThreadPool>
#include <QTimer>
#include <QUrlQuery>
#include <QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace {
using localflow::core::DictationPipeline;
using localflow::core::DictationPipelineConfiguration;
using localflow::core::DictationPipelineResult;
using localflow::core::DictationRequest;
using localflow::core::ITextInserter;
using localflow::core::ITextPolisher;
using localflow::core::ITranscriber;
using localflow::core::LearnedTerm;
using localflow::core::LearnedTerminologyBank;
using localflow::core::PersonalDictionary;
using localflow::core::PipelineCompletion;
using localflow::core::PolishContext;
using localflow::core::ReplacementEngine;
using localflow::core::Utterance;

PersonalDictionary loadDictionary(bool spokenPunctuation) {
    PersonalDictionary dictionary;
    dictionary.spoken_punctuation_enabled = spokenPunctuation;
    const QJsonDocument document = QJsonDocument::fromJson(
        QSettings().value(QStringLiteral("dictionary/rulesJson"), QByteArrayLiteral("[]")).toByteArray());
    for (const auto& entry : document.array()) {
        const QJsonObject rule = entry.toObject();
        const QString spoken = rule.value(QStringLiteral("spoken")).toString().trimmed();
        const QString written = rule.value(QStringLiteral("written")).toString();
        if (!spoken.isEmpty() && spoken.size() <= 200 && written.size() <= 500) {
            dictionary.rules.push_back({spoken.toStdString(), written.toStdString()});
        }
        if (dictionary.rules.size() >= 500) break;
    }
    return dictionary;
}

bool casualApplication(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return char(std::tolower(character));
    });
    static constexpr const char* identifiers[] = {
        "slack", "discord", "whatsapp", "telegram", "signal", "teams", "messenger",
    };
    for (const auto* identifier : identifiers) {
        if (value.find(identifier) != std::string::npos) return true;
    }
    return false;
}

struct PipelineSettings {
    bool polishEnabled = true;
    QString polishTone;
    int polishTimeoutMs = 3000;
    int polishMaxCharacters = 700;
    bool screenTerminology = true;
    PersonalDictionary dictionary;
};

// QProcess is thread-affine, so every S1 worker operation lives on this one
// persistent executor. Prewarming can no longer occupy the dictation executor
// or make ASR wait behind the worker's startup timeout.
class PolishExecutor final {
public:
    explicit PolishExecutor(const QString& modelPath) : worker_(modelPath) {
        pool_.setMaxThreadCount(1);
        pool_.setExpiryTimeout(-1);
    }

    ~PolishExecutor() { stop(); }

    void prewarm() {
        State expected = State::cold;
        if (stopping_.load() || !state_.compare_exchange_strong(expected, State::warming)) return;
        prewarmFuture_ = QtConcurrent::run(&pool_, [this] {
            QString error;
            const bool ready = worker_.prewarm(&error);
            if (!ready) {
                std::lock_guard lock(errorMutex_);
                startupError_ = error;
            }
            state_.store(ready ? State::ready : State::failed);
        });
    }

    PolishWorkerResult polish(
        QString text, QString tone, int timeoutMs, int maxOutputTokens = 1024) {
        if (stopping_.load()) {
            return {false, {}, QStringLiteral("Polish worker is stopping"), 0};
        }
        auto future = QtConcurrent::run(&pool_, [
            this,
            text = std::move(text),
            tone = std::move(tone),
            timeoutMs,
            maxOutputTokens
        ] {
            State state = state_.load();
            if (state == State::failed) return failedResult();
            if (state == State::cold) {
                QString error;
                if (!worker_.prewarm(&error)) {
                    {
                        std::lock_guard lock(errorMutex_);
                        startupError_ = error;
                    }
                    state_.store(State::failed);
                    return failedResult();
                }
                state_.store(State::ready);
            }
            // A warming task is ahead of this task on the same single-threaded
            // executor, so its state is final by the time this lambda runs.
            if (state_.load() == State::failed) return failedResult();
            return worker_.polish(text, tone, timeoutMs, maxOutputTokens);
        });
        future.waitForFinished();
        return future.result();
    }

    void stop() {
        if (stopping_.exchange(true)) return;
        auto future = QtConcurrent::run(&pool_, [this] { worker_.stop(); });
        future.waitForFinished();
        pool_.waitForDone();
    }

private:
    enum class State { cold, warming, ready, failed };

    PolishWorkerResult failedResult() const {
        std::lock_guard lock(errorMutex_);
        return {
            false,
            {},
            startupError_.isEmpty()
                ? QStringLiteral("Local polish worker could not be prepared")
                : startupError_,
            0,
        };
    }

    PolishWorkerClient worker_;
    QThreadPool pool_;
    QFuture<void> prewarmFuture_;
    std::atomic<State> state_{State::cold};
    std::atomic<bool> stopping_{false};
    mutable std::mutex errorMutex_;
    QString startupError_;
};

class CoreTranscriber final : public ITranscriber {
public:
    explicit CoreTranscriber(localflow::inference::NemoTranscriber& value) : value_(value) {}
    std::string transcribe(const Utterance& utterance) override {
        auto result = value_.transcribe({utterance.samples, int(utterance.sample_rate_hz)});
        if (!result) throw std::runtime_error(result.error());
        return result.take().text;
    }
private:
    localflow::inference::NemoTranscriber& value_;
};

class CorePolisher final : public ITextPolisher {
public:
    CorePolisher(PolishExecutor& worker, PipelineSettings settings)
        : worker_(worker), settings_(std::move(settings)) {}

    std::string polish(const std::string& text, const PolishContext& context) override {
        if (!settings_.polishEnabled) return text;
        const QString input = QString::fromStdString(text);
        if (input.size() > settings_.polishMaxCharacters) return text;
        QString tone = settings_.polishTone;
        if (tone == QStringLiteral("auto")) {
            tone = context.target_app_id && casualApplication(*context.target_app_id)
                ? QStringLiteral("casual") : QStringLiteral("neutral");
        }
        const int scaledTimeout = qMin(
            settings_.polishTimeoutMs * 2,
            settings_.polishTimeoutMs + qMax(0, input.size() - 150) / 100 * 500);
        auto result = worker_.polish(input, tone, scaledTimeout);
        if (!result.ok) throw std::runtime_error(result.error.toStdString());
        return result.text.toStdString();
    }
private:
    PolishExecutor& worker_;
    PipelineSettings settings_;
};

class CoreInserter final : public ITextInserter {
public:
    CoreInserter(PlatformBridge& bridge, std::uint64_t session) : bridge_(bridge), session_(session) {}
    void insert(const std::string& text) override {
        if (!bridge_.insert(session_, text, &error_)) throw std::runtime_error(error_);
    }
    const std::string& error() const { return error_; }
private:
    PlatformBridge& bridge_;
    std::uint64_t session_;
    std::string error_;
};

struct PipelineJobResult {
    DictationPipelineResult pipeline;
    QString detail;
    std::vector<LearnedTerm> learnedTerms;
    std::uint64_t learnedTermsRevision{0};
};

QString completionMessage(PipelineCompletion completion) {
    switch (completion) {
    case PipelineCompletion::inserted: return {};
    case PipelineCompletion::empty_output: return QStringLiteral("I didn’t hear enough speech to insert.");
    case PipelineCompletion::cancelled: return QStringLiteral("Dictation cancelled.");
    case PipelineCompletion::transcription_failed: return QStringLiteral("Local transcription failed.");
    case PipelineCompletion::processing_failed: return QStringLiteral("Local text processing failed.");
    case PipelineCompletion::insertion_failed: return QStringLiteral("The transcript could not be inserted safely.");
    }
    return QStringLiteral("Dictation failed.");
}
}

struct AppController::PressContext {
    std::string targetAppId;
    std::shared_future<std::vector<std::string>> screenTerms;
};

struct AppController::RuntimeState {
    RuntimeState(
        const QString& asrPath,
        const QString& polishPath,
        std::vector<LearnedTerm> initialLearnedTerms)
        : transcriber({asrPath.toStdString(), -1, {}}),
          polishWorker(polishPath),
          learned(std::move(initialLearnedTerms)) {
        pipelinePool.setMaxThreadCount(1);
        pipelinePool.setExpiryTimeout(-1);
        prewarmPool.setMaxThreadCount(1);
        prewarmPool.setExpiryTimeout(-1);
    }

    PlatformBridge platform;
    localflow::inference::NemoTranscriber transcriber;
    PolishExecutor polishWorker;
    LearnedTerminologyBank learned;
    std::mutex pipelineMutex;
    std::atomic<bool> cancelPipeline{false};
    QFutureWatcher<PipelineJobResult>* watcher = nullptr;
    QThreadPool pipelinePool;
    QThreadPool prewarmPool;
    QFuture<void> prewarmFuture;
};

AppController::AppController(QObject* parent)
    : QObject(parent), settings_(this), models_(this), learnedTerms_(this), updates_(this) {
    runtime_ = std::make_unique<RuntimeState>(
        models_.asrModelPath(), models_.polishModelPath(), learnedTerms_.terms());
    refreshCapabilities();
    refreshMicrophones();
    connect(&models_, &ModelManager::modelsReady, this, [this] {
        refreshCapabilities();
    });
    lastNotifiedUpdateKey_ = QSettings().value(
        QStringLiteral("updates/lastNotifiedVersion")).toString();
    connect(&updates_, &UpdateManager::changed, this, [this] {
        rebuildTrayMenu();
        if (!silentUpdateCheckInFlight_ || updates_.busy()) return;
        silentUpdateCheckInFlight_ = false;
        const QString version = updates_.availableVersion();
        const QString notificationKey = version.isEmpty()
            ? QStringLiteral("unknown-after-%1").arg(QApplication::applicationVersion())
            : version;
        if (!updates_.updateAvailable() ||
            notificationKey == lastNotifiedUpdateKey_ || tray_ == nullptr) {
            return;
        }
        tray_->showMessage(
            QStringLiteral("LocalFlow update available"),
            version.isEmpty()
                ? QStringLiteral("A new version is ready. Click to review and update.")
                : QStringLiteral("Version %1 is ready. Click to review and update.").arg(version),
            QSystemTrayIcon::Information,
            10'000);
        lastNotifiedUpdateKey_ = notificationKey;
        QSettings().setValue(
            QStringLiteral("updates/lastNotifiedVersion"), notificationKey);
    });
    connect(&learnedTerms_, &LearnedTermModel::termsChanged, this, [this] {
        ++learnedTermsRevision_;
        if (runtime_->watcher != nullptr) {
            pendingLearnedTermsSync_ = true;
            return;
        }
        synchronizeLearnedTerms();
    });

    const auto restart = [this] { restartListening(); };
    connect(&settings_, &SettingsModel::hotkeyChanged, this, restart);
    connect(&settings_, &SettingsModel::mouseTriggerChanged, this, restart);
    connect(&settings_, &SettingsModel::microphoneIdChanged, this, restart);
    connect(&settings_, &SettingsModel::keepMicWarmChanged, this, restart);
    connect(&settings_, &SettingsModel::duckAudioChanged, this, restart);
    connect(&settings_, &SettingsModel::screenTerminologyEnabledChanged, this, restart);
    connect(&settings_, &SettingsModel::clipboardRestoreDelayMsChanged, this, restart);
    connect(&settings_, &SettingsModel::holdThresholdMsChanged, this, restart);
    connect(&settings_, &SettingsModel::insertionMethodChanged, this, restart);
    connect(&settings_, &SettingsModel::historyLimitChanged, this, [this] {
        auto values = history_.stringList();
        while (values.size() > settings_.historyLimit()) values.removeLast();
        history_.setStringList(values);
    });

    // Match the macOS edition's update discovery without interrupting startup
    // or turning an offline connection into a user-facing error.
    auto* updateTimer = new QTimer(this);
    updateTimer->setInterval(24 * 60 * 60 * 1000);
    const auto checkSilently = [this] {
        if (updates_.busy() || updates_.updateAvailable()) return;
        silentUpdateCheckInFlight_ = true;
        updates_.checkForUpdatesSilently();
    };
    connect(updateTimer, &QTimer::timeout, this, checkSilently);
    QTimer::singleShot(30'000, this, [checkSilently, updateTimer] {
        checkSilently();
        updateTimer->start();
    });
}

AppController::~AppController() {
    runtime_->cancelPipeline.store(true);
    stopListening();
    if (runtime_->watcher) runtime_->watcher->waitForFinished();
    if (runtime_->prewarmFuture.isRunning()) runtime_->prewarmFuture.waitForFinished();
    runtime_->polishWorker.stop();
    runtime_->pipelinePool.waitForDone();
    runtime_->prewarmPool.waitForDone();
}

QString AppController::statusText() const {
    if (!listening_) return QStringLiteral("Push-to-talk is off");
    if (state_ == QStringLiteral("recording")) return QStringLiteral("Listening…");
    if (state_ == QStringLiteral("processing")) return QStringLiteral("Transcribing locally…");
    if (state_ == QStringLiteral("error")) return QStringLiteral("LocalFlow needs attention");
    return QStringLiteral("Ready — hold your push-to-talk key");
}

QString AppController::diagnosticsReport() const {
    QStringList lines{
        QStringLiteral("LocalFlow diagnostics"),
        QStringLiteral("Version: %1").arg(QApplication::applicationVersion()),
        QStringLiteral("Operating system: %1").arg(QSysInfo::prettyProductName()),
        QStringLiteral("Architecture: %1").arg(QSysInfo::currentCpuArchitecture()),
        QStringLiteral("Models ready: %1").arg(models_.ready() ? QStringLiteral("yes")
                                                               : QStringLiteral("no")),
        QStringLiteral("Platform ready: %1").arg(platformReady_ ? QStringLiteral("yes")
                                                                 : QStringLiteral("no")),
        QStringLiteral("Polishing enabled: %1").arg(settings_.polishEnabled()
                                                         ? QStringLiteral("yes")
                                                         : QStringLiteral("no")),
        QStringLiteral("Screen terminology enabled: %1").arg(
            settings_.screenTerminologyEnabled() ? QStringLiteral("yes")
                                                  : QStringLiteral("no")),
        QStringLiteral("Insertion mode: %1").arg(settings_.insertionMethod()),
        QStringLiteral("Shortcut: %1").arg(settings_.hotkey()),
        QStringLiteral("Mouse trigger: %1").arg(settings_.mouseTrigger()),
        QString(),
        QStringLiteral("Capabilities:"),
    };
    for (const auto& item : capabilities_) {
        const auto capability = item.toMap();
        lines.append(QStringLiteral("- %1: %2 — %3")
                         .arg(capability.value(QStringLiteral("label")).toString(),
                              capability.value(QStringLiteral("state")).toString(),
                              capability.value(QStringLiteral("detail")).toString()));
    }
    lines.append(QString());
    lines.append(QStringLiteral(
        "This report contains configuration and capability status only. It does not include dictated text, screen text, screenshots, audio, history, file paths, usernames, or device identifiers."));
    return lines.join(QLatin1Char('\n'));
}

void AppController::installTray() {
    if (tray_ != nullptr) return;
    tray_ = new QSystemTrayIcon(this);
    connect(tray_, &QSystemTrayIcon::messageClicked,
            this, &AppController::showSettings);
    const auto icon = QApplication::style()->standardIcon(QStyle::SP_MediaVolume);
    tray_->setIcon(icon);
    tray_->setToolTip(QStringLiteral("LocalFlow — fully local dictation"));
    trayMenu_ = new QMenu();
    tray_->setContextMenu(trayMenu_);
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) showSettings();
    });
    rebuildTrayMenu();
    tray_->show();
}

void AppController::rebuildTrayMenu() {
    if (trayMenu_ == nullptr) return;
    trayMenu_->clear();
    auto* status = trayMenu_->addAction(statusText());
    status->setEnabled(false);
    trayMenu_->addSeparator();
    auto* toggle = trayMenu_->addAction(listening_ ? QStringLiteral("Stop Listening")
                                                   : QStringLiteral("Start Listening"));
    connect(toggle, &QAction::triggered, this, &AppController::toggleListening);
    if (state_ == QStringLiteral("recording")) {
        auto* cancel = trayMenu_->addAction(QStringLiteral("Cancel Dictation"));
        connect(cancel, &QAction::triggered, this, &AppController::cancelDictation);
    }
    if (!attentionText_.isEmpty()) {
        trayMenu_->addSeparator();
        auto* attention = trayMenu_->addAction(attentionText_.left(120));
        attention->setEnabled(false);
        if (!recoveryTranscript_.isEmpty()) {
            auto* copyRecovery = trayMenu_->addAction(QStringLiteral("Copy Recovered Transcript"));
            connect(copyRecovery, &QAction::triggered, this, &AppController::copyRecoveryTranscript);
        }
        auto* dismiss = trayMenu_->addAction(QStringLiteral("Dismiss Message"));
        connect(dismiss, &QAction::triggered, this, &AppController::dismissAttention);
    }
    auto* settingsAction = trayMenu_->addAction(QStringLiteral("Settings…"));
    connect(settingsAction, &QAction::triggered, this, &AppController::showSettings);
    auto* updateAction = trayMenu_->addAction(QStringLiteral("Check for Updates…"));
    connect(updateAction, &QAction::triggered, this, &AppController::checkForUpdates);
    if (updates_.updateAvailable()) {
        auto* installUpdate = trayMenu_->addAction(updates_.readyToInstall()
            ? QStringLiteral("Install Available Update…")
            : QStringLiteral("Download Available Update…"));
        connect(installUpdate, &QAction::triggered, this, [this] {
            showSettings();
        });
    }
    auto* diagnosticsAction = trayMenu_->addAction(QStringLiteral("Diagnostics…"));
    connect(diagnosticsAction, &QAction::triggered, this, &AppController::openDiagnostics);
    trayMenu_->addSeparator();
    auto* quitAction = trayMenu_->addAction(QStringLiteral("Quit LocalFlow"));
    connect(quitAction, &QAction::triggered, this, &AppController::quit);
}

void AppController::showSettings() { emit settingsRequested(); }
void AppController::showOnboarding() { emit onboardingRequested(); }
void AppController::hideSettings() { emit settingsDismissed(); }

PlatformConfiguration AppController::platformConfiguration() const {
    return {
        settings_.hotkey().toStdString(),
        settings_.mouseTrigger().toStdString(),
        settings_.microphoneId().toStdString(),
        settings_.keepMicWarm(),
        settings_.duckAudio(),
        settings_.screenTerminologyEnabled(),
        settings_.clipboardRestoreDelayMs(),
        settings_.holdThresholdMs(),
        settings_.insertionMethod().toStdString(),
    };
}

void AppController::startListening() {
    if (listening_) return;
    if (runtime_->watcher != nullptr) {
        startAfterPipeline_ = true;
        capabilitySummary_ = QStringLiteral("Finishing the previous dictation before listening again…");
        emit capabilitySummaryChanged();
        return;
    }
    if (!models_.ready()) {
        showOnboarding();
        return;
    }
    refreshCapabilities();
    if (!platformReady_) {
        showOnboarding();
        return;
    }
    startAfterPipeline_ = false;
    const std::uint64_t generation = ++listeningGeneration_;
    std::string error;
    const bool started = runtime_->platform.start(
        platformConfiguration(),
        [this, generation](PlatformEvent event) {
            QMetaObject::invokeMethod(this, [this, generation, event = std::move(event)]() mutable {
                if (generation != listeningGeneration_) return;
                handlePlatformEvent(std::move(event));
            }, Qt::QueuedConnection);
        },
        &error);
    refreshCapabilities();
    if (!started) {
        listening_ = false;
        state_ = QStringLiteral("error");
        capabilitySummary_ = QString::fromStdString(error);
        emit listeningChanged();
        emit stateChanged();
        emit capabilitySummaryChanged();
        rebuildTrayMenu();
        showOnboarding();
        return;
    }
    listening_ = true;
    setState(QStringLiteral("idle"));
    emit listeningChanged();
    rebuildTrayMenu();

    runtime_->prewarmFuture = QtConcurrent::run(&runtime_->prewarmPool, [runtime = runtime_.get()] {
        (void)runtime->transcriber.prepare();
    });
    if (settings_.polishEnabled()) runtime_->polishWorker.prewarm();
}

void AppController::stopListening() {
    if (!runtime_) return;
    ++listeningGeneration_;
    startAfterPipeline_ = false;
    runtime_->cancelPipeline.store(true);
    runtime_->platform.stop();
    pressContexts_.clear();
    activePressSession_.reset();
    if (listening_) {
        listening_ = false;
        emit listeningChanged();
    }
    setState(QStringLiteral("idle"));
    rebuildTrayMenu();
}

void AppController::restartListening() {
    if (!listening_ || state_ != QStringLiteral("idle")) return;
    stopListening();
    startListening();
}

void AppController::toggleListening() {
    if (listening_) stopListening();
    else startListening();
}

void AppController::cancelDictation() {
    if (!listening_ || state_ != QStringLiteral("recording")) return;
    runtime_->platform.cancelCurrentSession();
}

void AppController::setState(QString state, double inputLevel) {
    const bool stateDidChange = state_ != state;
    const bool levelDidChange = inputLevel_ != inputLevel;
    state_ = std::move(state);
    inputLevel_ = inputLevel;
    if (stateDidChange) emit stateChanged();
    if (levelDidChange) emit inputLevelChanged();
    if (stateDidChange) rebuildTrayMenu();
}

void AppController::handlePlatformEvent(PlatformEvent event) {
    if (!listening_) return;
    switch (event.kind) {
    case PlatformEventKind::began:
        if (state_ != QStringLiteral("idle") || activePressSession_.has_value()) {
            // A began event outside the idle admission state indicates a stale
            // or duplicate native edge. Fail closed and cancel its quarantined
            // recording without disturbing an in-flight pipeline.
            runtime_->platform.setAcceptingInput(false);
            runtime_->platform.discardSession(event.sessionId);
            runtime_->platform.cancelCurrentSession();
            return;
        }
        activePressSession_ = event.sessionId;
        pressContexts_[event.sessionId] = {std::move(event.targetAppId), std::move(event.screenTerms)};
        setState(QStringLiteral("recording"));
        break;
    case PlatformEventKind::level:
        if (state_ == QStringLiteral("recording") && activePressSession_ == event.sessionId) {
            setState(state_, event.inputLevel);
        }
        break;
    case PlatformEventKind::cancelled: {
        runtime_->platform.discardSession(event.sessionId);
        pressContexts_.erase(event.sessionId);
        if (activePressSession_ != event.sessionId) return;
        activePressSession_.reset();
        runtime_->platform.setAcceptingInput(true);
        setState(QStringLiteral("idle"));
        break;
    }
    case PlatformEventKind::rejected:
        runtime_->platform.discardSession(event.sessionId);
        recoveryTranscript_.clear();
        capabilitySummary_ = QString::fromStdString(event.message);
        attentionText_ = capabilitySummary_;
        emit capabilitySummaryChanged();
        emit attentionChanged();
        setState(QStringLiteral("error"));
        break;
    case PlatformEventKind::ended: {
        const bool ownsActivePress = activePressSession_ == event.sessionId;
        const auto context = pressContexts_.find(event.sessionId);
        if (!ownsActivePress || context == pressContexts_.end()) {
            runtime_->platform.discardSession(event.sessionId);
            pressContexts_.erase(event.sessionId);
            if (ownsActivePress) activePressSession_.reset();
            // A missing context is recoverable only when no other recording or
            // pipeline owns the controller state.
            if (listening_ && !activePressSession_.has_value() &&
                runtime_->watcher == nullptr &&
                state_ != QStringLiteral("processing") &&
                state_ != QStringLiteral("error")) {
                runtime_->platform.setAcceptingInput(true);
                setState(QStringLiteral("idle"));
            }
            return;
        }
        activePressSession_.reset();
        PressContext captured = std::move(context->second);
        pressContexts_.erase(context);
        if (event.samples.size() < std::size_t(event.sampleRate * 0.15)) {
            runtime_->platform.discardSession(event.sessionId);
            runtime_->platform.setAcceptingInput(true);
            setState(QStringLiteral("idle"));
            return;
        }
        setState(QStringLiteral("processing"));
        runPipeline(std::move(event), std::move(captured));
        break;
    }
    case PlatformEventKind::error:
        ++listeningGeneration_;
        startAfterPipeline_ = false;
        runtime_->cancelPipeline.store(true);
        runtime_->platform.stop();
        pressContexts_.clear();
        activePressSession_.reset();
        listening_ = false;
        capabilitySummary_ = QString::fromStdString(event.message);
        attentionText_ = capabilitySummary_;
        emit listeningChanged();
        emit capabilitySummaryChanged();
        emit attentionChanged();
        setState(QStringLiteral("error"));
        break;
    }
}

void AppController::runPipeline(PlatformEvent event, PressContext context) {
    if (runtime_->watcher != nullptr) {
        runtime_->platform.discardSession(event.sessionId);
        capabilitySummary_ = QStringLiteral("The previous dictation is still finishing. Please try again.");
        emit capabilitySummaryChanged();
        attentionText_ = capabilitySummary_;
        emit attentionChanged();
        setState(QStringLiteral("error"));
        return;
    }
    runtime_->cancelPipeline.store(false);
    PipelineSettings settings;
    settings.polishEnabled = settings_.polishEnabled();
    settings.polishTone = settings_.polishTone();
    settings.polishTimeoutMs = settings_.polishTimeoutMs();
    settings.polishMaxCharacters = settings_.polishMaxCharacters();
    settings.screenTerminology = settings_.screenTerminologyEnabled();
    settings.dictionary = loadDictionary(settings_.spokenPunctuationEnabled());

    auto* watcher = new QFutureWatcher<PipelineJobResult>(this);
    runtime_->watcher = watcher;
    const std::uint64_t session = event.sessionId;
    const std::uint64_t generation = listeningGeneration_;
    const std::uint64_t learnedTermsRevision = learnedTermsRevision_;
    connect(watcher, &QFutureWatcher<PipelineJobResult>::finished, this, [this, watcher, session, generation] {
        const PipelineJobResult job = watcher->result();
        if (runtime_->watcher == watcher) runtime_->watcher = nullptr;
        runtime_->platform.discardSession(session);
        watcher->deleteLater();

        if (job.pipeline.inserted() && job.learnedTermsRevision == learnedTermsRevision_) {
            if (!learnedTerms_.replaceTermsAndSave(job.learnedTerms)) {
                synchronizeLearnedTerms();
            }
        } else if (pendingLearnedTermsSync_) {
            synchronizeLearnedTerms();
        }

        if (generation != listeningGeneration_) {
            const bool shouldStart = startAfterPipeline_ && !listening_;
            startAfterPipeline_ = false;
            if (shouldStart) startListening();
            return;
        }
        if (listening_) runtime_->platform.setAcceptingInput(true);

        if (job.pipeline.inserted()) {
            if (settings_.historyLimit() > 0) {
                auto values = history_.stringList();
                values.prepend(QString::fromStdString(job.pipeline.output_text));
                while (values.size() > settings_.historyLimit()) values.removeLast();
                history_.setStringList(values);
            }
            setState(QStringLiteral("idle"));
            return;
        }
        if (job.pipeline.diagnostics.completion == PipelineCompletion::cancelled) {
            setState(QStringLiteral("idle"));
            return;
        }
        if (!job.pipeline.output_text.empty()) {
            recoveryTranscript_ = QString::fromStdString(job.pipeline.output_text);
            QApplication::clipboard()->setText(recoveryTranscript_);
            capabilitySummary_ = job.detail + QStringLiteral(" Your transcript is safe and has been copied.");
        } else {
            recoveryTranscript_.clear();
            capabilitySummary_ = job.detail;
        }
        attentionText_ = capabilitySummary_;
        emit capabilitySummaryChanged();
        emit attentionChanged();
        setState(QStringLiteral("error"));
    });

    watcher->setFuture(QtConcurrent::run(&runtime_->pipelinePool, [
        runtime = runtime_.get(),
        event = std::move(event),
        context = std::move(context),
        settings = std::move(settings),
        learnedTermsRevision
    ]() mutable {
        std::lock_guard lock(runtime->pipelineMutex);
        PipelineJobResult job;
        try {
            std::vector<float> audio = event.sampleRate == 16000
                ? std::move(event.samples)
                : localflow::core::resample_mono_to_16khz(event.samples, event.sampleRate);
            CoreTranscriber transcriber(runtime->transcriber);
            CorePolisher polisher(runtime->polishWorker, settings);
            CoreInserter inserter(runtime->platform, event.sessionId);
            DictationPipeline pipeline(
                transcriber,
                ReplacementEngine(settings.dictionary),
                runtime->learned,
                polisher,
                inserter,
                DictationPipelineConfiguration{settings.screenTerminology});
            DictationRequest request;
            request.utterance = {std::move(audio), 16000};
            request.press_context.target_app_id = std::move(context.targetAppId);
            request.press_context.screen_terms_if_ready = [future = context.screenTerms]() mutable {
                if (!future.valid() || future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                    return std::vector<std::string>{};
                }
                return future.get();
            };
            request.is_cancelled = [runtime] { return runtime->cancelPipeline.load(); };
            job.pipeline = pipeline.run(request);
            job.learnedTerms = runtime->learned.terms();
            job.learnedTermsRevision = learnedTermsRevision;
            job.detail = completionMessage(job.pipeline.diagnostics.completion);
            if (job.pipeline.diagnostics.completion == PipelineCompletion::insertion_failed
                && !inserter.error().empty()) {
                job.detail = QString::fromStdString(inserter.error());
            }
        } catch (const std::exception& error) {
            job.detail = QString::fromUtf8(error.what());
            job.pipeline.diagnostics.completion = PipelineCompletion::processing_failed;
        } catch (...) {
            job.detail = QStringLiteral("Unexpected local processing error.");
            job.pipeline.diagnostics.completion = PipelineCompletion::processing_failed;
        }
        return job;
    }));
}

void AppController::synchronizeLearnedTerms() {
    if (!runtime_) return;
    std::lock_guard lock(runtime_->pipelineMutex);
    runtime_->learned.replace(learnedTerms_.terms());
    pendingLearnedTermsSync_ = false;
}

void AppController::clearHistory() { history_.setStringList({}); }

void AppController::copyHistoryItem(int row) {
    const auto values = history_.stringList();
    if (row >= 0 && row < values.size()) QApplication::clipboard()->setText(values.at(row));
}

void AppController::copyRecoveryTranscript() {
    if (!recoveryTranscript_.isEmpty()) QApplication::clipboard()->setText(recoveryTranscript_);
}

void AppController::dismissAttention() {
    if (attentionText_.isEmpty() && recoveryTranscript_.isEmpty()) return;
    attentionText_.clear();
    recoveryTranscript_.clear();
    emit attentionChanged();
    if (state_ == QStringLiteral("error")) {
        if (listening_) runtime_->platform.setAcceptingInput(true);
        setState(QStringLiteral("idle"));
    }
    rebuildTrayMenu();
}

void AppController::refreshMicrophones() {
    QVariantList values;
    bool selectedWasFound = settings_.microphoneId().isEmpty();
    values.append(QVariantMap{
        {QStringLiteral("id"), QString()},
        {QStringLiteral("name"), QStringLiteral("System default")},
    });
    for (const auto& device : runtime_->platform.microphones()) {
        if (device.id.empty()) continue;
        if (QString::fromStdString(device.id) == settings_.microphoneId()) selectedWasFound = true;
        values.append(QVariantMap{
            {QStringLiteral("id"), QString::fromStdString(device.id)},
            {QStringLiteral("name"), QString::fromStdString(device.name)},
        });
    }
    if (!selectedWasFound) {
        values.append(QVariantMap{
            {QStringLiteral("id"), settings_.microphoneId()},
            {QStringLiteral("name"), QStringLiteral("Selected microphone (disconnected — using default)")},
        });
    }
    microphones_ = std::move(values);
    emit microphonesChanged();
}

void AppController::refreshCapabilities() {
    runtime_->platform.refreshCapabilities();
    QVariantList values;
    for (const auto& capability : runtime_->platform.capabilities()) {
        values.append(QVariantMap{
            {QStringLiteral("id"), QString::fromStdString(capability.id)},
            {QStringLiteral("label"), QString::fromStdString(capability.label)},
            {QStringLiteral("state"), QString::fromStdString(capability.state)},
            {QStringLiteral("detail"), QString::fromStdString(capability.detail)},
            {QStringLiteral("remediation"), QString::fromStdString(capability.remediation)},
            {QStringLiteral("required"), capability.required},
        });
    }
    capabilities_ = std::move(values);
    platformReady_ = runtime_->platform.readyForDictation();
    capabilitySummary_ = QString::fromStdString(runtime_->platform.capabilitySummary());
    emit capabilitySummaryChanged();
}

void AppController::checkForUpdates() {
    silentUpdateCheckInFlight_ = false;
    updates_.checkForUpdates();
    showSettings();
}
void AppController::copyDiagnostics() {
    QApplication::clipboard()->setText(diagnosticsReport());
}

void AppController::openIssue() {
    QUrl url(QStringLiteral("https://github.com/yonif8/LocalFlow/issues/new"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("title"), QStringLiteral("LocalFlow issue"));
    query.addQueryItem(QStringLiteral("body"),
                       QStringLiteral("Please describe what happened above this report.\n\n") +
                           diagnosticsReport());
    url.setQuery(query);
    if (!QDesktopServices::openUrl(url)) {
        attentionText_ = QStringLiteral(
            "The LocalFlow issue page could not be opened. Copy the diagnostics report and open github.com/yonif8/LocalFlow/issues in your browser.");
        emit attentionChanged();
        setState(QStringLiteral("error"));
    }
}

void AppController::openDiagnostics() {
    refreshCapabilities();
    emit diagnosticsRequested();
}
void AppController::quit() { QApplication::quit(); }
