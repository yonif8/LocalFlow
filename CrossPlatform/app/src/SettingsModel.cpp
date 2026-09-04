#include "SettingsModel.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

namespace Key {
constexpr auto HudEnabled = "general/hudEnabled";
constexpr auto HistoryLimit = "general/historyLimit";
constexpr auto Hotkey = "dictation/hotkey";
constexpr auto MouseTrigger = "dictation/mouseTrigger";
constexpr auto HoldThresholdMs = "dictation/holdThresholdMs";
constexpr auto KeepMicWarm = "dictation/keepMicWarm";
constexpr auto DuckAudio = "dictation/duckAudio";
constexpr auto MicrophoneId = "dictation/microphoneId";
constexpr auto PolishEnabled = "polish/enabled";
constexpr auto PolishTone = "polish/tone";
constexpr auto PolishTimeoutMs = "polish/timeoutMs";
constexpr auto PolishMaxCharacters = "polish/maxCharacters";
constexpr auto ScreenTerminology = "dictionary/screenTerminology";
constexpr auto SpokenPunctuation = "dictionary/spokenPunctuation";
constexpr auto InsertionMethod = "insertion/method";
constexpr auto ClipboardRestoreDelayMs = "insertion/clipboardRestoreDelayMs";
}

namespace {
#ifndef Q_OS_WIN
QString linuxAutostartPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/autostart/com.localflow.LocalFlow.desktop");
}
#endif

QString launchExecutablePath() {
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
#else
    QString executable = qEnvironmentVariable("APPIMAGE");
    if (executable.isEmpty()) executable = QCoreApplication::applicationFilePath();
    const QFileInfo executableInfo(executable);
    const QString canonical = executableInfo.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty()
        ? executableInfo.absoluteFilePath() : canonical);
#endif
}

#ifdef Q_OS_WIN
QString windowsLaunchCommand() {
    return QStringLiteral("\"") + launchExecutablePath()
        + QStringLiteral("\" --background");
}
#else
QByteArray linuxAutostartEntry() {
    QString executable = launchExecutablePath();
    executable.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    executable.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    executable.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    executable.replace(QLatin1Char('`'), QStringLiteral("\\`"));
    executable.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return QStringLiteral(
        "[Desktop Entry]\nType=Application\nVersion=1.0\nName=LocalFlow\n"
        "Comment=Start private, local dictation in the background\n"
        "Exec=\"%1\" --background\nIcon=com.localflow.LocalFlow\n"
        "Terminal=false\nStartupNotify=false\nX-GNOME-Autostart-enabled=true\n")
        .arg(executable).toUtf8();
}
#endif

bool launchRegistrationExists() {
#ifdef Q_OS_WIN
    QSettings startup(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
    return startup.contains(QStringLiteral("LocalFlow"));
#else
    return QFileInfo::exists(linuxAutostartPath());
#endif
}

bool launchRegistrationMatches() {
#ifdef Q_OS_WIN
    QSettings startup(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
    return startup.value(QStringLiteral("LocalFlow")).toString().compare(
               windowsLaunchCommand(), Qt::CaseInsensitive) == 0;
#else
    QFile input(linuxAutostartPath());
    if (!input.open(QIODevice::ReadOnly) || input.size() > 32 * 1024) return false;
    const QByteArray expectedEntry = linuxAutostartEntry();
    // Do not require byte-identical metadata: the optional integration helper
    // adds desktop-specific fields. The executable and background argument are
    // the behavior this switch owns.
    const QList<QByteArray> lines = input.readAll().split('\n');
    const QList<QByteArray> expectedLines = expectedEntry.split('\n');
    const auto expectedLine = std::find_if(
        expectedLines.cbegin(), expectedLines.cend(),
        [](const QByteArray& line) { return line.startsWith("Exec="); });
    if (expectedLine == expectedLines.cend()) return false;
    return std::find(lines.cbegin(), lines.cend(), *expectedLine) != lines.cend();
#endif
}

bool applyLaunchAtLogin(bool enabled) {
#ifdef Q_OS_WIN
    QSettings startup(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
    if (enabled) {
        startup.setValue(QStringLiteral("LocalFlow"), windowsLaunchCommand());
    } else {
        startup.remove(QStringLiteral("LocalFlow"));
    }
    startup.sync();
    return startup.status() == QSettings::NoError;
#else
    const QString destination = linuxAutostartPath();
    if (!enabled) return !QFile::exists(destination) || QFile::remove(destination);
    QDir().mkpath(QFileInfo(destination).absolutePath());
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    const QByteArray entry = linuxAutostartEntry();
    return output.write(entry) == entry.size() && output.commit();
#endif
}
}

SettingsModel::SettingsModel(QObject* parent) : QObject(parent), dictionary_(this) {}

template <typename T>
T SettingsModel::value(const char* key, T fallback) const {
    return QSettings().value(QString::fromLatin1(key), QVariant::fromValue(fallback)).template value<T>();
}

template <typename T>
bool SettingsModel::update(const char* key, const T& next, const T& current) {
    if (next == current) return false;
    QSettings settings;
    settings.setValue(QString::fromLatin1(key), QVariant::fromValue(next));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        // Keep subsequent per-press reads aligned with what the UI still
        // reports if the settings backend becomes unavailable. Returning true
        // asks the caller to emit its change signal so a bound control snaps
        // back to the persisted value instead of implying the write worked.
        settings.setValue(QString::fromLatin1(key), QVariant::fromValue(current));
        settings.sync();
        return true;
    }
    return true;
}

bool SettingsModel::launchAtLogin() const { return launchRegistrationMatches(); }
bool SettingsModel::hudEnabled() const { return value(Key::HudEnabled, true); }
int SettingsModel::historyLimit() const { return std::clamp(value(Key::HistoryLimit, 10), 0, 50); }
QString SettingsModel::hotkey() const {
#ifdef Q_OS_LINUX
    // Modifier-only shortcuts are rejected by a number of Wayland
    // compositors. F8 works consistently through both the portal and X11.
    const QString fallback = QStringLiteral("F8");
#else
    const QString fallback = QStringLiteral("RightCtrl");
#endif
    const QString configured = value(Key::Hotkey, fallback);
    return configured == QStringLiteral("RightCtrl")
            || configured == QStringLiteral("RightAlt")
            || configured == QStringLiteral("F8")
            || configured == QStringLiteral("F9")
        ? configured
        : fallback;
}
QString SettingsModel::mouseTrigger() const {
    const auto configured = value(Key::MouseTrigger, QStringLiteral("off"));
    return configured == QStringLiteral("middle")
            || configured == QStringLiteral("side1")
            || configured == QStringLiteral("side2")
        ? configured
        : QStringLiteral("off");
}
int SettingsModel::holdThresholdMs() const { return std::clamp(value(Key::HoldThresholdMs, 300), 100, 1'000); }
bool SettingsModel::keepMicWarm() const { return value(Key::KeepMicWarm, false); }
bool SettingsModel::duckAudio() const { return value(Key::DuckAudio, true); }
QString SettingsModel::microphoneId() const { return value(Key::MicrophoneId, QString()); }
bool SettingsModel::polishEnabled() const { return value(Key::PolishEnabled, true); }
QString SettingsModel::polishTone() const {
    const QString configured = value(Key::PolishTone, QStringLiteral("auto"));
    return configured == QStringLiteral("casual") || configured == QStringLiteral("neutral")
        ? configured
        : QStringLiteral("auto");
}
int SettingsModel::polishTimeoutMs() const { return std::clamp(value(Key::PolishTimeoutMs, 3'000), 500, 5'000); }
int SettingsModel::polishMaxCharacters() const { return std::clamp(value(Key::PolishMaxCharacters, 700), 100, 4'000); }
bool SettingsModel::screenTerminologyEnabled() const { return value(Key::ScreenTerminology, false); }
bool SettingsModel::spokenPunctuationEnabled() const { return value(Key::SpokenPunctuation, false); }
QString SettingsModel::insertionMethod() const {
    const QString configured = value(Key::InsertionMethod, QStringLiteral("auto"));
    return configured == QStringLiteral("paste") || configured == QStringLiteral("type")
        ? configured
        : QStringLiteral("auto");
}
int SettingsModel::clipboardRestoreDelayMs() const { return std::clamp(value(Key::ClipboardRestoreDelayMs, 300), 50, 1'500); }

void SettingsModel::setLaunchAtLogin(bool next) {
    if ((next && launchRegistrationMatches()) ||
        (!next && !launchRegistrationExists())) {
        if (!launchAtLoginError_.isEmpty()) {
            launchAtLoginError_.clear();
            emit launchAtLoginErrorChanged();
        }
        return;
    }
    const bool applied = applyLaunchAtLogin(next);
    const bool matches = next ? launchRegistrationMatches()
                              : !launchRegistrationExists();
    const QString error = applied && matches
        ? QString()
        : QStringLiteral("LocalFlow could not change the sign-in setting. Check that your account can update startup apps, then try again.");
    if (launchAtLoginError_ != error) {
        launchAtLoginError_ = error;
        emit launchAtLoginErrorChanged();
    }
    emit launchAtLoginChanged();
}
void SettingsModel::setHudEnabled(bool next) { if (update(Key::HudEnabled, next, hudEnabled())) emit hudEnabledChanged(); }
void SettingsModel::setHistoryLimit(int next) { next = std::clamp(next, 0, 50); if (update(Key::HistoryLimit, next, historyLimit())) emit historyLimitChanged(); }
void SettingsModel::setHotkey(const QString& next) {
#ifdef Q_OS_LINUX
    const QString fallback = QStringLiteral("F8");
#else
    const QString fallback = QStringLiteral("RightCtrl");
#endif
    const QString normalized = next == QStringLiteral("RightCtrl")
            || next == QStringLiteral("RightAlt")
            || next == QStringLiteral("F8") || next == QStringLiteral("F9")
        ? next
        : fallback;
    if (update(Key::Hotkey, normalized, hotkey())) emit hotkeyChanged();
}
void SettingsModel::setMouseTrigger(const QString& next) {
    const QString normalized = next == QStringLiteral("middle")
            || next == QStringLiteral("side1")
            || next == QStringLiteral("side2")
        ? next
        : QStringLiteral("off");
    if (update(Key::MouseTrigger, normalized, mouseTrigger())) emit mouseTriggerChanged();
}
void SettingsModel::setHoldThresholdMs(int next) { next = std::clamp(next, 100, 1'000); if (update(Key::HoldThresholdMs, next, holdThresholdMs())) emit holdThresholdMsChanged(); }
void SettingsModel::setKeepMicWarm(bool next) { if (update(Key::KeepMicWarm, next, keepMicWarm())) emit keepMicWarmChanged(); }
void SettingsModel::setDuckAudio(bool next) { if (update(Key::DuckAudio, next, duckAudio())) emit duckAudioChanged(); }
void SettingsModel::setMicrophoneId(const QString& next) { if (update(Key::MicrophoneId, next, microphoneId())) emit microphoneIdChanged(); }
void SettingsModel::setPolishEnabled(bool next) { if (update(Key::PolishEnabled, next, polishEnabled())) emit polishEnabledChanged(); }
void SettingsModel::setPolishTone(const QString& next) {
    const QString normalized = next == QStringLiteral("casual")
            || next == QStringLiteral("neutral")
        ? next
        : QStringLiteral("auto");
    if (update(Key::PolishTone, normalized, polishTone())) emit polishToneChanged();
}
void SettingsModel::setPolishTimeoutMs(int next) { next = std::clamp(next, 500, 5'000); if (update(Key::PolishTimeoutMs, next, polishTimeoutMs())) emit polishTimeoutMsChanged(); }
void SettingsModel::setPolishMaxCharacters(int next) { next = std::clamp(next, 100, 4'000); if (update(Key::PolishMaxCharacters, next, polishMaxCharacters())) emit polishMaxCharactersChanged(); }
void SettingsModel::setScreenTerminologyEnabled(bool next) { if (update(Key::ScreenTerminology, next, screenTerminologyEnabled())) emit screenTerminologyEnabledChanged(); }
void SettingsModel::setSpokenPunctuationEnabled(bool next) { if (update(Key::SpokenPunctuation, next, spokenPunctuationEnabled())) emit spokenPunctuationEnabledChanged(); }
void SettingsModel::setInsertionMethod(const QString& next) {
    const QString normalized = next == QStringLiteral("paste")
            || next == QStringLiteral("type")
        ? next
        : QStringLiteral("auto");
    if (update(Key::InsertionMethod, normalized, insertionMethod())) emit insertionMethodChanged();
}
void SettingsModel::setClipboardRestoreDelayMs(int next) { next = std::clamp(next, 50, 1'500); if (update(Key::ClipboardRestoreDelayMs, next, clipboardRestoreDelayMs())) emit clipboardRestoreDelayMsChanged(); }
