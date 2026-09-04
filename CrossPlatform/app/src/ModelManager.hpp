#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include <atomic>
#include <memory>

class ModelManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString detailText READ detailText NOTIFY changed)
    Q_PROPERTY(QString asrModelPath READ asrModelPath CONSTANT)
    Q_PROPERTY(QString polishModelPath READ polishModelPath CONSTANT)

public:
    explicit ModelManager(QObject* parent = nullptr);
    ~ModelManager() override;

    bool ready() const;
    bool busy() const { return reply_ != nullptr || verifying_; }
    double progress() const;
    QString statusText() const { return statusText_; }
    QString detailText() const { return detailText_; }
    QString asrModelPath() const;
    QString polishModelPath() const;

    Q_INVOKABLE void downloadMissing();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void revealModels();

signals:
    void changed();
    void modelsReady();

private:
    struct Spec {
        QString displayName;
        QString filename;
        QUrl url;
        QByteArray sha256;
        qint64 bytes;
    };

    static const Spec& asrSpec();
    static const Spec& polishSpec();
    static QString modelsDirectory();
    static QString humanBytes(qint64 bytes);

    QString finalPath(const Spec& spec) const;
    QString verificationRecordPath(const Spec& spec) const;
    bool validOnDisk(const Spec& spec) const;
    void removeVerificationRecord(const Spec& spec) const;
    void startNext();
    void start(const Spec& spec);
    void handleMetadata();
    void handleReadyRead();
    void handleFinished();
    void verifyDownloaded(const Spec& spec, const QString& candidatePath, bool installAfterVerification = true);
    void fail(QString summary, QString detail);
    void resetTransfer();

    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QFile output_;
    const Spec* active_ = nullptr;
    qint64 resumedBytes_ = 0;
    qint64 receivedBytes_ = 0;
    bool verifying_ = false;
    bool cancelled_ = false;
    QString transferFailure_;
    std::shared_ptr<std::atomic_bool> verificationCancellation_;
    QString statusText_;
    QString detailText_;
};
