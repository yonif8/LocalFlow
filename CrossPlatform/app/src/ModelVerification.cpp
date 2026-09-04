#include "ModelVerification.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cmath>
#include <utility>

namespace localflow::models {
namespace {

void setError(QString* destination, QString value) {
    if (destination != nullptr) *destination = std::move(value);
}

bool exactInteger(const QJsonValue& value, qint64* result) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    constexpr double kLargestExactInteger = 9'007'199'254'740'991.0;
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < -kLargestExactInteger || number > kLargestExactInteger) {
        return false;
    }
    *result = qint64(number);
    return double(*result) == number;
}

HashResult changedResult(QString detail) {
    HashResult result;
    result.status = HashStatus::file_changed;
    result.error = std::move(detail);
    return result;
}

HashResult ioErrorResult(QString detail) {
    HashResult result;
    result.status = HashStatus::io_error;
    result.error = std::move(detail);
    return result;
}

}  // namespace

std::optional<FileStamp> captureFileStamp(const QString& path, QString* error) {
    QFileInfo info(path);
    info.refresh();
    if (!info.isFile() || info.size() < 0) {
        setError(error, QStringLiteral("The model file is missing or is not a regular file."));
        return std::nullopt;
    }
    const QDateTime modified = info.lastModified();
    if (!modified.isValid()) {
        setError(error, QStringLiteral("The model file modification time is unavailable."));
        return std::nullopt;
    }
    const QDateTime metadataChanged = info.metadataChangeTime();
    FileStamp result;
    result.size = info.size();
    result.modifiedMs = modified.toMSecsSinceEpoch();
    if (metadataChanged.isValid()) {
        result.metadataChangedMs = metadataChanged.toMSecsSinceEpoch();
    }
    return result;
}

HashResult hashFile(
    const QString& path,
    const qint64 expectedBytes,
    const std::atomic_bool& cancellation,
    const qint64 readChunkBytes) {
    if (cancellation.load()) {
        HashResult result;
        result.status = HashStatus::cancelled;
        return result;
    }
    if (readChunkBytes <= 0) {
        return ioErrorResult(QStringLiteral("The verification read size is invalid."));
    }

    QString snapshotError;
    const auto beforeOpen = captureFileStamp(path, &snapshotError);
    if (!beforeOpen) return ioErrorResult(std::move(snapshotError));
    if (beforeOpen->size != expectedBytes) {
        return changedResult(QStringLiteral("The model file size changed before verification."));
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return ioErrorResult(file.errorString());
    }
    const auto afterOpen = captureFileStamp(path, &snapshotError);
    if (!afterOpen) return ioErrorResult(std::move(snapshotError));
    if (*afterOpen != *beforeOpen || file.size() != expectedBytes) {
        return changedResult(QStringLiteral("The model file changed while it was opened."));
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytesRead = 0;
    while (true) {
        if (cancellation.load()) {
            HashResult result;
            result.status = HashStatus::cancelled;
            return result;
        }
        const QByteArray chunk = file.read(readChunkBytes);
        if (chunk.isEmpty()) {
            if (file.error() != QFileDevice::NoError) {
                return ioErrorResult(file.errorString());
            }
            if (file.atEnd()) break;
            return ioErrorResult(QStringLiteral("The model file could not be read completely."));
        }
        bytesRead += chunk.size();
        if (bytesRead > expectedBytes) {
            return changedResult(QStringLiteral("The model file grew during verification."));
        }
        hash.addData(chunk);
    }
    if (file.error() != QFileDevice::NoError) {
        return ioErrorResult(file.errorString());
    }
    if (bytesRead != expectedBytes) {
        return changedResult(QStringLiteral("The model file size changed during verification."));
    }

    const auto afterRead = captureFileStamp(path, &snapshotError);
    if (!afterRead) return ioErrorResult(std::move(snapshotError));
    if (*afterRead != *beforeOpen || file.size() != expectedBytes) {
        return changedResult(QStringLiteral("The model file changed during verification."));
    }
    if (cancellation.load()) {
        HashResult result;
        result.status = HashStatus::cancelled;
        return result;
    }

    HashResult result;
    result.status = HashStatus::complete;
    result.digest = hash.result();
    result.stamp = *afterRead;
    return result;
}

QByteArray serializeVerificationReceipt(
    const QByteArray& sha256Hex,
    const FileStamp& stamp) {
    QJsonObject record{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("sha256"), QString::fromLatin1(sha256Hex)},
        {QStringLiteral("sizeBytes"), double(stamp.size)},
        {QStringLiteral("modifiedMs"), double(stamp.modifiedMs)},
        {QStringLiteral("metadataChangedMs"),
         stamp.metadataChangedMs
             ? QJsonValue(double(*stamp.metadataChangedMs))
             : QJsonValue(QJsonValue::Null)},
    };
    return QJsonDocument(record).toJson(QJsonDocument::Compact);
}

bool verificationReceiptMatches(
    const QByteArray& encoded,
    const QByteArray& expectedSha256Hex,
    const qint64 expectedBytes,
    const FileStamp& currentStamp) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject record = document.object();
    if (record.size() != 5 ||
        record.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
        record.value(QStringLiteral("sha256")).toString().toLatin1() !=
            expectedSha256Hex) {
        return false;
    }

    qint64 size = -1;
    qint64 modifiedMs = 0;
    if (!exactInteger(record.value(QStringLiteral("sizeBytes")), &size) ||
        !exactInteger(record.value(QStringLiteral("modifiedMs")), &modifiedMs) ||
        size != expectedBytes || size != currentStamp.size ||
        modifiedMs != currentStamp.modifiedMs) {
        return false;
    }

    const QJsonValue changedValue = record.value(QStringLiteral("metadataChangedMs"));
    if (changedValue.isNull()) return !currentStamp.metadataChangedMs.has_value();
    qint64 changedMs = 0;
    return exactInteger(changedValue, &changedMs) &&
           currentStamp.metadataChangedMs.has_value() &&
           changedMs == *currentStamp.metadataChangedMs;
}

bool verificationReceiptMatchesFile(
    const QString& modelPath,
    const QString& receiptPath,
    const QByteArray& expectedSha256Hex,
    const qint64 expectedBytes) {
    const auto stamp = captureFileStamp(modelPath);
    if (!stamp || stamp->size != expectedBytes) return false;

    QFile receipt(receiptPath);
    if (!receipt.open(QIODevice::ReadOnly) || receipt.size() < 0 ||
        receipt.size() > 4096) {
        return false;
    }
    return verificationReceiptMatches(
        receipt.readAll(), expectedSha256Hex, expectedBytes, *stamp);
}

bool writeVerificationReceipt(
    const QString& modelPath,
    const QString& receiptPath,
    const QByteArray& sha256Hex,
    const FileStamp& verifiedStamp,
    QString* error) {
    const auto beforeWrite = captureFileStamp(modelPath, error);
    if (!beforeWrite || *beforeWrite != verifiedStamp) {
        if (beforeWrite) {
            setError(error, QStringLiteral("The model file changed before its verification receipt was saved."));
        }
        return false;
    }

    const QByteArray encoded = serializeVerificationReceipt(sha256Hex, verifiedStamp);
    QSaveFile output(receiptPath);
    if (!output.open(QIODevice::WriteOnly)) {
        setError(error, output.errorString());
        return false;
    }
    if (output.write(encoded) != encoded.size()) {
        setError(error, output.errorString());
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) {
        setError(error, output.errorString());
        return false;
    }

    const auto afterWrite = captureFileStamp(modelPath, error);
    if (!afterWrite || *afterWrite != verifiedStamp) {
        QFile::remove(receiptPath);
        if (afterWrite) {
            setError(error, QStringLiteral("The model file changed while its verification receipt was saved."));
        }
        return false;
    }
    return true;
}

}  // namespace localflow::models
