#include "AppController.hpp"

#include "PolishWorkerClient.hpp"
#include "SentenceCarry.hpp"
#include "localflow/core/audio_resampler.hpp"
#include "localflow/core/dictation_pipeline.hpp"
#include "localflow/core/incremental_dictation.hpp"
#include "localflow/inference/NemoTranscriber.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFutureWatcher>
#include <QFuture>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMetaObject>
#include <QSet>
#include <QSettings>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QThreadPool>
#include <QTimer>
#include <QDebug>
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
    QSet<QString> spokenForms;
    const QJsonDocument document = QJsonDocument::fromJson(
        QSettings().value(QStringLiteral("dictionary/rulesJson"), QByteArrayLiteral("[]")).toByteArray());
    for (const auto& entry : document.array()) {
        const QJsonObject rule = entry.toObject();
        const QString spoken = rule.value(QStringLiteral("spoken")).toString().trimmed();
        const QString written = rule.value(QStringLiteral("written")).toString();
        const QString folded = spoken.toCaseFolded();
        if (!spoken.isEmpty() && spoken.size() <= 200 &&
            !written.trimmed().isEmpty() && written.size() <= 500 &&
            !spokenForms.contains(folded)) {
            dictionary.rules.push_back({spoken.toStdString(), written.toStdString()});
            spokenForms.insert(folded);
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
    int polishMaxCharacters = 4000;
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

// Runs the existing replacement/terminology/polish contract on already
// recognized text. The staging inserter has no OS access; learning stays in
// the session's private bank until the final real insertion succeeds.
class RecognizedText final : public ITranscriber {
public:
    explicit RecognizedText(std::string value) : value_(std::move(value)) {}
    std::string transcribe(const Utterance&) override { return value_; }
private:
    std::string value_;
};

class StagingInserter final : public ITextInserter {
public:
    void insert(const std::string&) override {}
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
        : transcriber({asrPath.toUtf8().toStdString(), -1, {}}),
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
    std::shared_ptr<IncrementalJob> incremental;
    localflow::core::DictationSegmentBudget segmentBudget;
    std::chrono::steady_clock::time_point segmentStartedAt{};
    std::optional<std::chrono::steady_clock::time_point> quietSince;
};

struct AppController::IncrementalJob {
    explicit IncrementalJob(std::vector<LearnedTerm> terms) : learned(std::move(terms)) {}

    std::atomic<bool> cancelled{false};
    std::atomic<double> observedRate{0.503 / 12.0};
    std::size_t submittedSamples{0}; // UI-thread only; offsets in native samples.
    PipelineSettings settings;
    localflow::core::PressTimeContext context;
    std::uint64_t learnedRevision{0};
    // These fields are exclusively owned by the serial pipeline executor.
    localflow::core::IncrementalDictation stream;
    LearnedTerminologyBank learned;
    bool failed{false};

    std::string advance(RuntimeState& runtime, const Utterance& audio, bool final) {
        if (failed) throw std::runtime_error("A background segment needs recovery");
        CoreTranscriber transcriber(runtime.transcriber);
        CorePolisher polisher(runtime.polishWorker, settings);
        const auto isCancelled = [this] { return cancelled.load(); };
        const auto transcribe = [&](const Utterance& value) { return transcriber.transcribe(value); };
        const auto transform = [&](const std::string& text) {
            RecognizedText recognized(text);
            StagingInserter staging;
            DictationPipeline pipeline(recognized, ReplacementEngine(settings.dictionary),
                learned, polisher, staging, {settings.screenTerminology});
            DictationRequest request;
            request.press_context = context;
            request.is_cancelled = isCancelled;
            auto result = pipeline.run(request);
            if (!result.inserted()) throw std::runtime_error("Background text processing failed");
            return result.output_text;
        };
        const auto start = std::chrono::steady_clock::now();
        try {
            if (final) return stream.finish(audio, transcribe, transform, isCancelled);
            stream.append(audio, transcribe, transform, localflow::app::holdLastSentence, isCancelled);
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (audio.duration_seconds() >= 1) {
                observedRate.store(std::max(observedRate.load(), seconds / audio.duration_seconds()));
            }
            qInfo() << "Background chunk completed: audioSeconds=" << audio.duration_seconds()
                    << "processingSeconds=" << seconds;
            return {};
        } catch (...) {
            failed = true;
            throw;
        }
    }
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
        if (runtime_->watcher != nullptr || runtime_->incremental) {
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
    runtime_->pipelinePool.waitForDone();
    runtime_->polishWorker.stop();
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
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

    trayAvailable_ = true;
    emit trayAvailableChanged();
    tray_ = new QSystemTrayIcon(this);
    connect(tray_, &QSystemTrayIcon::messageClicked, this, [this] {
        emit updatesRequested();
    });
    tray_->setIcon(QIcon(QStringLiteral(":/LocalFlow/assets/LocalFlow.png")));
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
            emit updatesRequested();
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
        capabilitySummary_ = error.empty()
            ? QStringLiteral("LocalFlow could not start listening. Review the system access checks below and try again.")
            : QString::fromStdString(error);
        recoveryTranscript_.clear();
        attentionText_ = capabilitySummary_;
        emit listeningChanged();
        emit stateChanged();
        emit capabilitySummaryChanged();
        emit attentionChanged();
        rebuildTrayMenu();
        showOnboarding();
        return;
    }
    listening_ = true;
    setState(QStringLiteral("idle"));
    emit listeningChanged();
    rebuildTrayMenu();
    if (!trayAvailable_) showSettings();

    runtime_->prewarmFuture = QtConcurrent::run(&runtime_->prewarmPool, [runtime = runtime_.get()] {
        (void)runtime->transcriber.prepare();
    });
    if (settings_.polishEnabled()) runtime_->polishWorker.prewarm();
}

void AppController::stopListening() {
    if (!runtime_) return;
    resetSegments();
    ++listeningGeneration_;
    startAfterPipeline_ = false;
    pendingListeningRestart_ = false;
    pendingRestartMayRecoverError_ = false;
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
    if (!listening_ && state_ != QStringLiteral("error")) return;
    pendingListeningRestart_ = true;
    if (state_ == QStringLiteral("error")) {
        // A settings edit made after an error is an explicit recovery attempt.
        // A restart merely queued before the error must wait for dismissal so
        // the failure is not hidden by immediately reopening native input.
        pendingRestartMayRecoverError_ = true;
    }
    // Settings panels can update several native options in one UI action.
    // Defer one event-loop turn so those signals collapse into one restart.
    QMetaObject::invokeMethod(this, [this] {
        (void)applyPendingListeningRestartIfSafe();
    }, Qt::QueuedConnection);
}

bool AppController::applyPendingListeningRestartIfSafe() {
    if (!pendingListeningRestart_ || activePressSession_.has_value() ||
        runtime_->watcher != nullptr || state_ == QStringLiteral("recording") ||
        state_ == QStringLiteral("processing") ||
        (state_ == QStringLiteral("error") &&
         !pendingRestartMayRecoverError_)) {
        return false;
    }
    pendingListeningRestart_ = false;
    pendingRestartMayRecoverError_ = false;
    stopListening();
    startListening();
    return true;
}

void AppController::toggleListening() {
    if (listening_) stopListening();
    else {
        pendingListeningRestart_ = false;
        pendingRestartMayRecoverError_ = false;
        startListening();
    }
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

void AppController::resetSegments() {
    if (runtime_->incremental) {
        runtime_->incremental->cancelled.store(true);
        runtime_->segmentBudget.processing_seconds_per_audio_second = std::max(
            runtime_->segmentBudget.processing_seconds_per_audio_second,
            runtime_->incremental->observedRate.load());
        runtime_->incremental.reset();
    }
    runtime_->quietSince.reset();
    runtime_->segmentStartedAt = std::chrono::steady_clock::now();
    if (pendingLearnedTermsSync_ && runtime_->watcher == nullptr) synchronizeLearnedTerms();
}

void AppController::considerSegment(const PlatformEvent& event) {
    const auto now = std::chrono::steady_clock::now();
    if (event.inputLevel > 0.18f) { runtime_->quietSince.reset(); return; }
    if (!runtime_->quietSince) runtime_->quietSince = now;
    if (runtime_->incremental) {
        runtime_->segmentBudget.processing_seconds_per_audio_second = std::max(
            runtime_->segmentBudget.processing_seconds_per_audio_second,
            runtime_->incremental->observedRate.load());
    }
    const double threshold = runtime_->segmentBudget.pause_search_seconds();
    if (now - *runtime_->quietSince < std::chrono::milliseconds(650) ||
        std::chrono::duration<double>(now - runtime_->segmentStartedAt).count() < threshold) return;
    const auto context = pressContexts_.find(event.sessionId);
    if (context == pressContexts_.end()) return;
    auto chunk = runtime_->platform.snapshot(event.sessionId,
        runtime_->incremental ? runtime_->incremental->submittedSamples : 0);
    if (chunk.sampleRate == 0 || double(chunk.samples.size()) / chunk.sampleRate < threshold ||
        !localflow::core::DictationSegmentBudget::has_quiet_tail(chunk.samples, chunk.sampleRate)) return;
    if (!runtime_->incremental) {
        auto job = std::make_shared<IncrementalJob>(learnedTerms_.terms());
        job->settings.polishEnabled = settings_.polishEnabled();
        job->settings.polishTone = settings_.polishTone();
        job->settings.polishTimeoutMs = settings_.polishTimeoutMs();
        job->settings.polishMaxCharacters = settings_.polishMaxCharacters();
        job->settings.screenTerminology = settings_.screenTerminologyEnabled();
        job->settings.dictionary = loadDictionary(settings_.spokenPunctuationEnabled());
        job->context.target_app_id = context->second.targetAppId;
        job->context.screen_terms_if_ready = [future = context->second.screenTerms] {
            if (!future.valid() || future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                return std::vector<std::string>{};
            return future.get();
        };
        job->learnedRevision = learnedTermsRevision_;
        runtime_->incremental = std::move(job);
    }
    auto job = runtime_->incremental;
    job->submittedSamples += chunk.samples.size();
    runtime_->segmentStartedAt = now;
    runtime_->quietSince.reset();
    qInfo() << "Background chunk queued: samples=" << chunk.samples.size()
            << "pauseSearchSeconds=" << threshold;
    (void)QtConcurrent::run(&runtime_->pipelinePool, [runtime = runtime_.get(), job, chunk = std::move(chunk)] {
        std::lock_guard lock(runtime->pipelineMutex);
        if (job->cancelled.load() || job->failed) return;
        try {
            auto samples = chunk.sampleRate == 16000 ? chunk.samples
                : localflow::core::resample_mono_to_16khz(chunk.samples, chunk.sampleRate);
            (void)job->advance(*runtime, {std::move(samples), 16000}, false);
        } catch (...) {
            // Keep original PCM in the capture owner. Finish retries the full
            // recording, rather than inserting the chunks that happened to pass.
            job->failed = true;
            qWarning() << "Background chunk needs recovery";
        }
    });
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
        resetSegments();
        activePressSession_ = event.sessionId;
        pressContexts_[event.sessionId] = {std::move(event.targetAppId), std::move(event.screenTerms)};
        setState(QStringLiteral("recording"));
        break;
    case PlatformEventKind::level:
        if (state_ == QStringLiteral("recording") && activePressSession_ == event.sessionId) {
            setState(state_, event.inputLevel);
            considerSegment(event);
        }
        break;
    case PlatformEventKind::cancelled: {
        runtime_->platform.discardSession(event.sessionId);
        pressContexts_.erase(event.sessionId);
        if (activePressSession_ != event.sessionId) return;
        resetSegments();
        activePressSession_.reset();
        setState(QStringLiteral("idle"));
        if (!applyPendingListeningRestartIfSafe()) {
            runtime_->platform.setAcceptingInput(true);
        }
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
                setState(QStringLiteral("idle"));
                if (!applyPendingListeningRestartIfSafe()) {
                    runtime_->platform.setAcceptingInput(true);
                }
            }
            return;
        }
        activePressSession_.reset();
        PressContext captured = std::move(context->second);
        pressContexts_.erase(context);
        if (event.samples.size() < std::size_t(event.sampleRate * 0.15)) {
            runtime_->platform.discardSession(event.sessionId);
            setState(QStringLiteral("idle"));
            if (!applyPendingListeningRestartIfSafe()) {
                runtime_->platform.setAcceptingInput(true);
            }
            return;
        }
        setState(QStringLiteral("processing"));
        runPipeline(std::move(event), std::move(captured));
        break;
    }
    case PlatformEventKind::error:
        resetSegments();
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
        (void)applyPendingListeningRestartIfSafe();
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

    auto incremental = std::exchange(runtime_->incremental, nullptr);
    const auto releaseStarted = std::chrono::steady_clock::now();
    const double audioSeconds = event.sampleRate == 0 ? 0 : double(event.samples.size()) / event.sampleRate;

    auto* watcher = new QFutureWatcher<PipelineJobResult>(this);
    runtime_->watcher = watcher;
    const std::uint64_t session = event.sessionId;
    const std::uint64_t generation = listeningGeneration_;
    const std::uint64_t learnedTermsRevision = incremental ? incremental->learnedRevision : learnedTermsRevision_;
    connect(watcher, &QFutureWatcher<PipelineJobResult>::finished, this, [this, watcher, session, generation, incremental, releaseStarted, audioSeconds] {
        const PipelineJobResult job = watcher->result();
        if (incremental) {
            runtime_->segmentBudget.processing_seconds_per_audio_second = std::max(
                runtime_->segmentBudget.processing_seconds_per_audio_second, incremental->observedRate.load());
        } else if (audioSeconds >= 12 && job.pipeline.inserted()) {
            runtime_->segmentBudget.observe(audioSeconds,
                double((job.pipeline.diagnostics.total_elapsed - job.pipeline.diagnostics.insertion.elapsed).count()) / 1e6);
        }
        qInfo() << "Dictation finished: releaseSeconds="
                << std::chrono::duration<double>(std::chrono::steady_clock::now() - releaseStarted).count()
                << "background=" << bool(incremental) << "inserted=" << job.pipeline.inserted();
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
            if (shouldStart) {
                pendingListeningRestart_ = false;
                pendingRestartMayRecoverError_ = false;
                startListening();
            } else {
                (void)applyPendingListeningRestartIfSafe();
            }
            return;
        }
        if (job.pipeline.inserted()) {
            if (settings_.historyLimit() > 0) {
                auto values = history_.stringList();
                values.prepend(QString::fromStdString(job.pipeline.output_text));
                while (values.size() > settings_.historyLimit()) values.removeLast();
                history_.setStringList(values);
            }
            setState(QStringLiteral("idle"));
            if (!applyPendingListeningRestartIfSafe() && listening_) {
                runtime_->platform.setAcceptingInput(true);
            }
            return;
        }
        if (job.pipeline.diagnostics.completion == PipelineCompletion::cancelled) {
            setState(QStringLiteral("idle"));
            if (!applyPendingListeningRestartIfSafe() && listening_) {
                runtime_->platform.setAcceptingInput(true);
            }
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
        learnedTermsRevision,
        incremental
    ]() mutable {
        std::lock_guard lock(runtime->pipelineMutex);
        PipelineJobResult job;
        try {
            if (incremental) {
                const auto cancelled = [&] { return incremental->cancelled.load() || runtime->cancelPipeline.load(); };
                const auto start = std::chrono::steady_clock::now();
                bool prepared = false;
                std::string text;
                try {
                    if (cancelled()) throw std::runtime_error("Dictation cancelled");
                    const auto offset = std::min(incremental->submittedSamples, event.samples.size());
                    std::vector<float> tail(event.samples.begin() + offset, event.samples.end());
                    if (event.sampleRate != 16000) tail = localflow::core::resample_mono_to_16khz(tail, event.sampleRate);
                    text = incremental->advance(*runtime, {std::move(tail), 16000}, true);
                    prepared = true;
                } catch (...) {
                    if (cancelled()) {
                        job.pipeline.diagnostics.completion = PipelineCompletion::cancelled;
                        return job;
                    }
                    qWarning() << "Retrying retained whole recording after background failure";
                }
                if (prepared) {
                    job.pipeline.output_text = std::move(text);
                    if (cancelled()) {
                        job.pipeline.diagnostics.completion = PipelineCompletion::cancelled;
                    } else if (job.pipeline.output_text.empty()) {
                        job.pipeline.diagnostics.completion = PipelineCompletion::empty_output;
                    } else {
                        CoreInserter inserter(runtime->platform, event.sessionId);
                        const auto insertionStart = std::chrono::steady_clock::now();
                        try {
                            inserter.insert(job.pipeline.output_text);
                            job.pipeline.diagnostics.completion = PipelineCompletion::inserted;
                            job.pipeline.diagnostics.insertion.outcome = localflow::core::PipelineStageOutcome::succeeded;
                        } catch (...) {
                            job.pipeline.diagnostics.completion = PipelineCompletion::insertion_failed;
                            job.pipeline.diagnostics.insertion.outcome = localflow::core::PipelineStageOutcome::failed;
                            job.detail = QString::fromStdString(inserter.error());
                        }
                        job.pipeline.diagnostics.insertion.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - insertionStart);
                        if (job.pipeline.inserted()) {
                            // Only commit the private bank after the real insertion.
                            try { runtime->learned.replace(incremental->learned.terms()); } catch (...) {}
                        }
                    }
                    job.pipeline.diagnostics.total_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start);
                    job.learnedTerms = runtime->learned.terms();
                    job.learnedTermsRevision = learnedTermsRevision;
                    if (job.detail.isEmpty()) job.detail = completionMessage(job.pipeline.diagnostics.completion);
                    return job;
                }
            }
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
    pendingLearnedTermsSync_ = false;
    // Queue behind any cancelled background job; never block the capture/UI
    // thread on inference. Later pipeline work uses this updated bank in order.
    (void)QtConcurrent::run(&runtime_->pipelinePool,
        [runtime = runtime_.get(), terms = learnedTerms_.terms()] {
            std::lock_guard lock(runtime->pipelineMutex);
            runtime->learned.replace(terms);
        });
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
        setState(QStringLiteral("idle"));
        if (!applyPendingListeningRestartIfSafe() && listening_ &&
            runtime_->watcher == nullptr && !activePressSession_.has_value()) {
            runtime_->platform.setAcceptingInput(true);
        }
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
    emit updatesRequested();
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
