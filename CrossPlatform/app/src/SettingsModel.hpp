#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class SettingsModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool launchAtLogin READ launchAtLogin WRITE setLaunchAtLogin NOTIFY launchAtLoginChanged)
    Q_PROPERTY(bool hudEnabled READ hudEnabled WRITE setHudEnabled NOTIFY hudEnabledChanged)
    Q_PROPERTY(int historyLimit READ historyLimit WRITE setHistoryLimit NOTIFY historyLimitChanged)
    Q_PROPERTY(QString hotkey READ hotkey WRITE setHotkey NOTIFY hotkeyChanged)
    Q_PROPERTY(int holdThresholdMs READ holdThresholdMs WRITE setHoldThresholdMs NOTIFY holdThresholdMsChanged)
    Q_PROPERTY(bool keepMicWarm READ keepMicWarm WRITE setKeepMicWarm NOTIFY keepMicWarmChanged)
    Q_PROPERTY(bool duckAudio READ duckAudio WRITE setDuckAudio NOTIFY duckAudioChanged)
    Q_PROPERTY(QString microphoneId READ microphoneId WRITE setMicrophoneId NOTIFY microphoneIdChanged)
    Q_PROPERTY(bool polishEnabled READ polishEnabled WRITE setPolishEnabled NOTIFY polishEnabledChanged)
    Q_PROPERTY(QString polishTone READ polishTone WRITE setPolishTone NOTIFY polishToneChanged)
    Q_PROPERTY(int polishTimeoutMs READ polishTimeoutMs WRITE setPolishTimeoutMs NOTIFY polishTimeoutMsChanged)
    Q_PROPERTY(int polishMaxCharacters READ polishMaxCharacters WRITE setPolishMaxCharacters NOTIFY polishMaxCharactersChanged)
    Q_PROPERTY(bool screenTerminologyEnabled READ screenTerminologyEnabled WRITE setScreenTerminologyEnabled NOTIFY screenTerminologyEnabledChanged)
    Q_PROPERTY(bool spokenPunctuationEnabled READ spokenPunctuationEnabled WRITE setSpokenPunctuationEnabled NOTIFY spokenPunctuationEnabledChanged)
    Q_PROPERTY(QString insertionMethod READ insertionMethod WRITE setInsertionMethod NOTIFY insertionMethodChanged)
    Q_PROPERTY(int clipboardRestoreDelayMs READ clipboardRestoreDelayMs WRITE setClipboardRestoreDelayMs NOTIFY clipboardRestoreDelayMsChanged)

public:
    explicit SettingsModel(QObject* parent = nullptr);

    bool launchAtLogin() const;
    bool hudEnabled() const;
    int historyLimit() const;
    QString hotkey() const;
    int holdThresholdMs() const;
    bool keepMicWarm() const;
    bool duckAudio() const;
    QString microphoneId() const;
    bool polishEnabled() const;
    QString polishTone() const;
    int polishTimeoutMs() const;
    int polishMaxCharacters() const;
    bool screenTerminologyEnabled() const;
    bool spokenPunctuationEnabled() const;
    QString insertionMethod() const;
    int clipboardRestoreDelayMs() const;

public slots:
    void setLaunchAtLogin(bool value);
    void setHudEnabled(bool value);
    void setHistoryLimit(int value);
    void setHotkey(const QString& value);
    void setHoldThresholdMs(int value);
    void setKeepMicWarm(bool value);
    void setDuckAudio(bool value);
    void setMicrophoneId(const QString& value);
    void setPolishEnabled(bool value);
    void setPolishTone(const QString& value);
    void setPolishTimeoutMs(int value);
    void setPolishMaxCharacters(int value);
    void setScreenTerminologyEnabled(bool value);
    void setSpokenPunctuationEnabled(bool value);
    void setInsertionMethod(const QString& value);
    void setClipboardRestoreDelayMs(int value);

signals:
    void launchAtLoginChanged();
    void hudEnabledChanged();
    void historyLimitChanged();
    void hotkeyChanged();
    void holdThresholdMsChanged();
    void keepMicWarmChanged();
    void duckAudioChanged();
    void microphoneIdChanged();
    void polishEnabledChanged();
    void polishToneChanged();
    void polishTimeoutMsChanged();
    void polishMaxCharactersChanged();
    void screenTerminologyEnabledChanged();
    void spokenPunctuationEnabledChanged();
    void insertionMethodChanged();
    void clipboardRestoreDelayMsChanged();

private:
    template <typename T>
    T value(const char* key, T fallback) const;

    template <typename T>
    bool update(const char* key, const T& next, const T& current);
};
