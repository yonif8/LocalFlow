#include "ModelVerification.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

bool writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(contents) == contents.size();
}

void testHashOutcomesAndReceiptBinding() {
    QTemporaryDir directory;
    expect(directory.isValid(), "a temporary model directory is available");
    const QString modelPath = directory.filePath(QStringLiteral("model.gguf"));
    const QString receiptPath = modelPath + QStringLiteral(".verified.json");
    const QByteArray contents("a deterministic tiny model fixture");
    expect(writeFile(modelPath, contents), "the model fixture can be written");

    std::atomic_bool cancellation{false};
    const auto result = localflow::models::hashFile(
        modelPath, contents.size(), cancellation, 3);
    expect(result.status == localflow::models::HashStatus::complete,
           "a stable readable file hashes successfully");
    expect(result.digest == QCryptographicHash::hash(contents, QCryptographicHash::Sha256),
           "hashing covers the complete file");

    QString error;
    expect(localflow::models::writeVerificationReceipt(
               modelPath, receiptPath, result.digest.toHex(), result.stamp, &error),
           "a receipt is written only for the verified stamp");
    expect(localflow::models::verificationReceiptMatchesFile(
               modelPath, receiptPath, result.digest.toHex(), contents.size()),
           "the receipt validates the unchanged model");

    expect(writeFile(modelPath, contents + QByteArrayLiteral(" changed")),
           "the model fixture can be changed");
    expect(!localflow::models::verificationReceiptMatchesFile(
               modelPath, receiptPath, result.digest.toHex(), contents.size()),
           "a changed model never matches its old receipt");
}

void testChangedFileCannotReceiveReceipt() {
    QTemporaryDir directory;
    const QString modelPath = directory.filePath(QStringLiteral("model.gguf"));
    const QString receiptPath = modelPath + QStringLiteral(".verified.json");
    const QByteArray contents("verified bytes");
    expect(writeFile(modelPath, contents), "the TOCTOU fixture can be written");

    std::atomic_bool cancellation{false};
    const auto result = localflow::models::hashFile(
        modelPath, contents.size(), cancellation, 2);
    expect(result.status == localflow::models::HashStatus::complete,
           "the TOCTOU fixture hashes successfully");
    expect(writeFile(modelPath, contents + QByteArrayLiteral("!")),
           "the TOCTOU fixture can change after hashing");

    QString error;
    expect(!localflow::models::writeVerificationReceipt(
               modelPath, receiptPath, result.digest.toHex(), result.stamp, &error),
           "receipt writing rejects a post-hash file change");
    expect(!QFile::exists(receiptPath),
           "a rejected post-hash change leaves no trusted receipt");
}

void testFailuresAreDistinctFromDigestMismatch() {
    QTemporaryDir directory;
    std::atomic_bool cancellation{false};
    const QString missing = directory.filePath(QStringLiteral("missing.gguf"));
    const auto ioFailure =
        localflow::models::hashFile(missing, 10, cancellation, 4);
    expect(ioFailure.status == localflow::models::HashStatus::io_error,
           "an unreadable path reports an I/O error instead of a digest");

    const QString modelPath = directory.filePath(QStringLiteral("model.gguf"));
    expect(writeFile(modelPath, QByteArrayLiteral("12345")),
           "the changed-size fixture can be written");
    const auto changed =
        localflow::models::hashFile(modelPath, 6, cancellation, 2);
    expect(changed.status == localflow::models::HashStatus::file_changed,
           "an unexpected size is reported as a changed file");
    expect(QFile::exists(modelPath),
           "verification helpers never delete a file on an I/O/change outcome");

    cancellation.store(true);
    const auto cancelled =
        localflow::models::hashFile(modelPath, 5, cancellation, 2);
    expect(cancelled.status == localflow::models::HashStatus::cancelled,
           "cancellation is reported separately from verification failure");
}

void testReceiptSupportsUnavailableMetadataChangeTime() {
    localflow::models::FileStamp portable;
    portable.size = 42;
    portable.modifiedMs = 1'725'000'000'000;
    portable.metadataChangedMs.reset();
    const QByteArray digest(64, 'a');
    const QByteArray encoded =
        localflow::models::serializeVerificationReceipt(digest, portable);
    expect(localflow::models::verificationReceiptMatches(
               encoded, digest, portable.size, portable),
           "a receipt round-trips when metadata-change time is unavailable");

    auto native = portable;
    native.metadataChangedMs = portable.modifiedMs;
    expect(!localflow::models::verificationReceiptMatches(
               encoded, digest, portable.size, native),
           "a receipt cannot silently switch between unavailable and available metadata");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    testHashOutcomesAndReceiptBinding();
    testChangedFileCannotReceiveReceipt();
    testFailuresAreDistinctFromDigestMismatch();
    testReceiptSupportsUnavailableMetadataChangeTime();
    if (failures == 0) {
        std::cout << "Model verification tests passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
