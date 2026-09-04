#include "AppController.hpp"

#include "PolishWorkerClient.hpp"
#include "localflow/core/audio_resampler.hpp"
#include "localflow/core/dictation_pipeline.hpp"
#include "localflow/inference/NemoTranscriber.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMetaObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QThreadPool>
#include <QTimer>
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

QString learnedTermsPath() {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(directory);
    return directory + QStringLiteral("/learned-terminology.json");
}

std::vector<LearnedTerm> loadLearnedTerms() {
    QFile file(learnedTermsPath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) return {};
    std::vector<LearnedTerm> terms;
    for (const auto& entry : document.array()) {
        const QJsonObject object = entry.toObject();
        LearnedTerm term;
        term.id = object.value(QStringLiteral("id")).toString().toStdString();
        term.canonical = object.value(QStringLiteral("canonical")).toString().toStdString();
        for (const auto& alias : object.value(QStringLiteral("aliases")).toArray()) {
            term.aliases.push_back(alias.toString().toStdString());
        }
        term.use_count = std::uint32_t(qMax(0, object.value(QStringLiteral("useCount")).toInt(1)));
        term.created_at_ms = qint64(object.value(QStringLiteral("createdAtMs")).toDouble());
        term.last_used_at_ms = qint64(object.value(QStringLiteral("lastUsedAtMs")).toDouble());
        const QString app = object.value(QStringLiteral("sourceAppId")).toString();
        if (!app.isEmpty()) term.source_app_id = app.toStdString();
        terms.push_back(std::move(term));
    }
    return LearnedTerminologyBank::sanitized(terms);
}

bool saveLearnedTerms(const LearnedTerminologyBank& bank) {
    QJsonArray array;
    for (const auto& term : bank.terms()) {
        QJsonArray aliases;
        for (const auto& alias : term.aliases) aliases.append(QString::fromStdString(alias));
        QJsonObject object{
            {QStringLiteral("id"), QString::fromStdString(term.id)},
            {QStringLiteral("canonical"), QString::fromStdString(term.canonical)},
            {QStringLiteral("aliases"), aliases},
            {QStringLiteral("useCount"), int(term.use_count)},
            {QStringLiteral("createdAtMs"), double(term.created_at_ms)},
            {QStringLiteral("lastUsedAtMs"), double(term.last_used_at_ms)},
        };
        if (term.source_app_id) {
            object.insert(QStringLiteral("sourceAppId"), QString::fromStdString(*term.source_app_id));
        }
        array.append(object);
    }
    QSaveFile output(learnedTermsPath());
    if (!output.open(QIODevice::WriteOnly)) return false;
    if (output.write(QJsonDocument(array).toJson(QJsonDocument::Compact)) < 0) return false;
    return output.commit();
}

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
    int polishTimeoutMs = 1500;
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
    RuntimeState(const QString& asrPath, const QString& polishPath)
        : transcriber({asrPath.toStdString(), -1, {}}),
          polishWorker(polishPath),
          learned(loadLearnedTerms()) {
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
    : QObject(parent), settings_(this), models_(this) {
    runtime_ = std::make_unique<RuntimeState>(models_.asrModelPath(), models_.polishModelPath());
    connect(&models_, &ModelManager::modelsReady, this, [this] {
        if (!listening_) startListening();
    });

    const auto restart = [this] { restartListening(); };
    connect(&settings_, &SettingsModel::hotkeyChanged, this, restart);
    connect(&settings_, &SettingsModel::microphoneIdChanged, this, restart);
    connect(&settings_, &SettingsModel::keepMicWarmChanged, this, restart);
    connect(&settings_, &SettingsModel::duckAudioChanged, this, restart);
    connect(&settings_, &SettingsModel::screenTerminologyEnabledChanged, this, restart);
    connect(&settings_, &SettingsModel::clipboardRestoreDelayMsChanged, this, restart);
    connect(&settings_, &SettingsModel::historyLimitChanged, this, [this] {
        auto values = history_.stringList();
        while (values.size() > settings_.historyLimit()) values.removeLast();
        history_.setStringList(values);
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

void AppController::installTray() {
    if (tray_ != nullptr) return;
    tray_ = new QSystemTrayIcon(this);
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
    auto* settingsAction = trayMenu_->addAction(QStringLiteral("Settings…"));
    connect(settingsAction, &QAction::triggered, this, &AppController::showSettings);
    auto* updateAction = trayMenu_->addAction(QStringLiteral("Check for Updates…"));
    connect(updateAction, &QAction::triggered, this, &AppController::checkForUpdates);
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
        settings_.microphoneId().toStdString(),
        settings_.keepMicWarm(),
        settings_.duckAudio(),
        settings_.screenTerminologyEnabled(),
        settings_.clipboardRestoreDelayMs(),
    };
}

void AppController::startListening() {
    if (listening_) return;
    if (!models_.ready()) {
        showOnboarding();
        return;
    }
    std::string error;
    const bool started = runtime_->platform.start(
        platformConfiguration(),
        [this](PlatformEvent event) {
            QMetaObject::invokeMethod(this, [this, event = std::move(event)]() mutable {
                handlePlatformEvent(std::move(event));
            }, Qt::QueuedConnection);
        },
        &error);
    capabilitySummary_ = QString::fromStdString(runtime_->platform.capabilitySummary());
    emit capabilitySummaryChanged();
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
    runtime_->cancelPipeline.store(true);
    runtime_->platform.stop();
    pressContexts_.clear();
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
        if (state_ != QStringLiteral("idle")) return;
        pressContexts_[event.sessionId] = {std::move(event.targetAppId), std::move(event.screenTerms)};
        setState(QStringLiteral("recording"));
        break;
    case PlatformEventKind::level:
        if (state_ == QStringLiteral("recording")) setState(state_, event.inputLevel);
        break;
    case PlatformEventKind::cancelled:
        runtime_->platform.discardSession(event.sessionId);
        pressContexts_.erase(event.sessionId);
        setState(QStringLiteral("idle"));
        break;
    case PlatformEventKind::ended: {
        const auto context = pressContexts_.find(event.sessionId);
        if (context == pressContexts_.end()) {
            runtime_->platform.discardSession(event.sessionId);
            setState(QStringLiteral("idle"));
            return;
        }
        PressContext captured = std::move(context->second);
        pressContexts_.erase(context);
        if (event.samples.size() < std::size_t(event.sampleRate * 0.15)) {
            runtime_->platform.discardSession(event.sessionId);
            setState(QStringLiteral("idle"));
            return;
        }
        runtime_->platform.setAcceptingInput(false);
        setState(QStringLiteral("processing"));
        runPipeline(std::move(event), std::move(captured));
        break;
    }
    case PlatformEventKind::error:
        runtime_->platform.stop();
        listening_ = false;
        capabilitySummary_ = QString::fromStdString(event.message);
        emit listeningChanged();
        emit capabilitySummaryChanged();
        setState(QStringLiteral("error"));
        break;
    }
}

void AppController::runPipeline(PlatformEvent event, PressContext context) {
    if (runtime_->watcher != nullptr) return;
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
    connect(watcher, &QFutureWatcher<PipelineJobResult>::finished, this, [this, watcher, session] {
        const PipelineJobResult job = watcher->result();
        runtime_->watcher = nullptr;
        runtime_->platform.discardSession(session);
        runtime_->platform.setAcceptingInput(true);
        watcher->deleteLater();

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
            QApplication::clipboard()->setText(QString::fromStdString(job.pipeline.output_text));
            capabilitySummary_ = job.detail + QStringLiteral(" The transcript was copied to your clipboard.");
        } else {
            capabilitySummary_ = job.detail;
        }
        emit capabilitySummaryChanged();
        setState(QStringLiteral("error"));
        QTimer::singleShot(4000, this, [this] {
            if (listening_ && state_ == QStringLiteral("error")) setState(QStringLiteral("idle"));
        });
    });

    watcher->setFuture(QtConcurrent::run(&runtime_->pipelinePool, [
        runtime = runtime_.get(),
        event = std::move(event),
        context = std::move(context),
        settings = std::move(settings)
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
            if (job.pipeline.inserted()) (void)saveLearnedTerms(runtime->learned);
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

void AppController::clearHistory() { history_.setStringList({}); }

void AppController::copyHistoryItem(int row) {
    const auto values = history_.stringList();
    if (row >= 0 && row < values.size()) QApplication::clipboard()->setText(values.at(row));
}

void AppController::checkForUpdates() { emit updateCheckRequested(); }
void AppController::openDiagnostics() { emit settingsRequested(); }
void AppController::quit() { QApplication::quit(); }
