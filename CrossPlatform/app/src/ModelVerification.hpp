#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <optional>

namespace localflow::models {

struct FileStamp {
    qint64 size{-1};
    qint64 modifiedMs{0};
    std::optional<qint64> metadataChangedMs;

    friend bool operator==(const FileStamp& left, const FileStamp& right) {
        return left.size == right.size && left.modifiedMs == right.modifiedMs &&
               left.metadataChangedMs == right.metadataChangedMs;
    }
    friend bool operator!=(const FileStamp& left, const FileStamp& right) {
        return !(left == right);
    }
};

enum class HashStatus {
    complete,
    cancelled,
    io_error,
    file_changed,
};

struct HashResult {
    HashStatus status{HashStatus::io_error};
    QByteArray digest;
    FileStamp stamp;
    QString error;
};

std::optional<FileStamp> captureFileStamp(
    const QString& path, QString* error = nullptr);

HashResult hashFile(
    const QString& path,
    qint64 expectedBytes,
    const std::atomic_bool& cancellation,
    qint64 readChunkBytes = 4 * 1024 * 1024);

QByteArray serializeVerificationReceipt(
    const QByteArray& sha256Hex,
    const FileStamp& stamp);

bool verificationReceiptMatches(
    const QByteArray& encoded,
    const QByteArray& expectedSha256Hex,
    qint64 expectedBytes,
    const FileStamp& currentStamp);

bool verificationReceiptMatchesFile(
    const QString& modelPath,
    const QString& receiptPath,
    const QByteArray& expectedSha256Hex,
    qint64 expectedBytes);

bool writeVerificationReceipt(
    const QString& modelPath,
    const QString& receiptPath,
    const QByteArray& sha256Hex,
    const FileStamp& verifiedStamp,
    QString* error = nullptr);

}  // namespace localflow::models
