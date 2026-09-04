#include "SettingsModel.hpp"

#include <QSettings>
#include <algorithm>

namespace Key {
constexpr auto LaunchAtLogin = "general/launchAtLogin";
constexpr auto HudEnabled = "general/hudEnabled";
constexpr auto HistoryLimit = "general/historyLimit";
constexpr auto Hotkey = "dictation/hotkey";
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

SettingsModel::SettingsModel(QObject* parent) : QObject(parent) {}

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
    return true;
}

bool SettingsModel::launchAtLogin() const { return value(Key::LaunchAtLogin, true); }
bool SettingsModel::hudEnabled() const { return value(Key::HudEnabled, true); }
int SettingsModel::historyLimit() const { return std::clamp(value(Key::HistoryLimit, 10), 0, 50); }
QString SettingsModel::hotkey() const { return value(Key::Hotkey, QStringLiteral("RightCtrl")); }
int SettingsModel::holdThresholdMs() const { return std::clamp(value(Key::HoldThresholdMs, 300), 100, 1'000); }
bool SettingsModel::keepMicWarm() const { return value(Key::KeepMicWarm, false); }
bool SettingsModel::duckAudio() const { return value(Key::DuckAudio, true); }
QString SettingsModel::microphoneId() const { return value(Key::MicrophoneId, QString()); }
bool SettingsModel::polishEnabled() const { return value(Key::PolishEnabled, true); }
QString SettingsModel::polishTone() const { return value(Key::PolishTone, QStringLiteral("auto")); }
int SettingsModel::polishTimeoutMs() const { return std::clamp(value(Key::PolishTimeoutMs, 1'500), 500, 5'000); }
int SettingsModel::polishMaxCharacters() const { return std::clamp(value(Key::PolishMaxCharacters, 700), 100, 4'000); }
bool SettingsModel::screenTerminologyEnabled() const { return value(Key::ScreenTerminology, false); }
bool SettingsModel::spokenPunctuationEnabled() const { return value(Key::SpokenPunctuation, false); }
QString SettingsModel::insertionMethod() const { return value(Key::InsertionMethod, QStringLiteral("auto")); }
int SettingsModel::clipboardRestoreDelayMs() const { return std::clamp(value(Key::ClipboardRestoreDelayMs, 300), 50, 1'500); }

void SettingsModel::setLaunchAtLogin(bool next) { if (update(Key::LaunchAtLogin, next, launchAtLogin())) emit launchAtLoginChanged(); }
void SettingsModel::setHudEnabled(bool next) { if (update(Key::HudEnabled, next, hudEnabled())) emit hudEnabledChanged(); }
void SettingsModel::setHistoryLimit(int next) { next = std::clamp(next, 0, 50); if (update(Key::HistoryLimit, next, historyLimit())) emit historyLimitChanged(); }
void SettingsModel::setHotkey(const QString& next) { if (update(Key::Hotkey, next, hotkey())) emit hotkeyChanged(); }
void SettingsModel::setHoldThresholdMs(int next) { next = std::clamp(next, 100, 1'000); if (update(Key::HoldThresholdMs, next, holdThresholdMs())) emit holdThresholdMsChanged(); }
void SettingsModel::setKeepMicWarm(bool next) { if (update(Key::KeepMicWarm, next, keepMicWarm())) emit keepMicWarmChanged(); }
void SettingsModel::setDuckAudio(bool next) { if (update(Key::DuckAudio, next, duckAudio())) emit duckAudioChanged(); }
void SettingsModel::setMicrophoneId(const QString& next) { if (update(Key::MicrophoneId, next, microphoneId())) emit microphoneIdChanged(); }
void SettingsModel::setPolishEnabled(bool next) { if (update(Key::PolishEnabled, next, polishEnabled())) emit polishEnabledChanged(); }
void SettingsModel::setPolishTone(const QString& next) { if (update(Key::PolishTone, next, polishTone())) emit polishToneChanged(); }
void SettingsModel::setPolishTimeoutMs(int next) { next = std::clamp(next, 500, 5'000); if (update(Key::PolishTimeoutMs, next, polishTimeoutMs())) emit polishTimeoutMsChanged(); }
void SettingsModel::setPolishMaxCharacters(int next) { next = std::clamp(next, 100, 4'000); if (update(Key::PolishMaxCharacters, next, polishMaxCharacters())) emit polishMaxCharactersChanged(); }
void SettingsModel::setScreenTerminologyEnabled(bool next) { if (update(Key::ScreenTerminology, next, screenTerminologyEnabled())) emit screenTerminologyEnabledChanged(); }
void SettingsModel::setSpokenPunctuationEnabled(bool next) { if (update(Key::SpokenPunctuation, next, spokenPunctuationEnabled())) emit spokenPunctuationEnabledChanged(); }
void SettingsModel::setInsertionMethod(const QString& next) { if (update(Key::InsertionMethod, next, insertionMethod())) emit insertionMethodChanged(); }
void SettingsModel::setClipboardRestoreDelayMs(int next) { next = std::clamp(next, 50, 1'500); if (update(Key::ClipboardRestoreDelayMs, next, clipboardRestoreDelayMs())) emit clipboardRestoreDelayMsChanged(); }
