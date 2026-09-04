#include "../src/UpdateManager.hpp"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <array>
#include <cstdlib>

#ifdef Q_OS_WIN
#include "ed25519.h"
#endif

namespace {

int failures = 0;

void expect(const bool condition, const char *message) {
  if (!condition) {
    qCritical().noquote() << "FAIL:" << message;
    ++failures;
  }
}

QByteArray validManifest() {
  return R"json({
  "schemaVersion": 1,
  "product": "LocalFlow",
  "channel": "stable",
  "version": "2.4.1",
  "publishedAt": "2026-09-04T01:02:03Z",
  "platform": {
    "os": "windows",
    "architecture": "x86_64",
    "minimumBuild": 22000
  },
  "installer": {
    "fileName": "LocalFlow-2.4.1-windows-x64.exe",
    "url": "https://github.com/yonif8/LocalFlow/releases/download/v2.4.1/LocalFlow-2.4.1-windows-x64.exe",
    "sizeBytes": 12345678,
    "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "ed25519": {
      "algorithm": "ed25519",
      "validAtPublication": true,
      "signature": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=="
    },
    "authenticode": {
      "required": false,
      "validAtPublication": false,
      "signerCertificateSha256": null
    },
    "silentArguments": "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"
  }
})json";
}

void testSemanticVersions() {
  QString error;
  const auto release =
      localflow::updates::parseSemanticVersion(QStringLiteral("1.2.3"), &error);
  const auto prerelease = localflow::updates::parseSemanticVersion(
      QStringLiteral("1.2.3-rc.2+build.9"), &error);
  expect(release.has_value(), "accepts a canonical release version");
  expect(prerelease.has_value(),
         "accepts canonical prerelease and build identifiers");
  if (release && prerelease) {
    expect(localflow::updates::compareSemanticVersions(*prerelease, *release) <
               0,
           "a prerelease sorts before its release");
  }

  const auto rc2 =
      localflow::updates::parseSemanticVersion(QStringLiteral("1.2.3-rc.2"));
  const auto rc10 =
      localflow::updates::parseSemanticVersion(QStringLiteral("1.2.3-rc.10"));
  if (rc2 && rc10) {
    expect(localflow::updates::compareSemanticVersions(*rc2, *rc10) < 0,
           "numeric prerelease identifiers use numeric precedence");
  }
  expect(!localflow::updates::parseSemanticVersion(QStringLiteral("01.2.3")),
         "rejects a leading zero in the major version");
  expect(!localflow::updates::parseSemanticVersion(QStringLiteral("1.2.3-01")),
         "rejects a leading zero in numeric prerelease identifiers");
  expect(!localflow::updates::parseSemanticVersion(QStringLiteral("1.2.3-.")),
         "rejects empty prerelease identifiers");
  expect(!localflow::updates::parseSemanticVersion(
             QStringLiteral("4294967296.0.0")),
         "rejects version components outside the supported range");
  expect(!localflow::updates::parseSemanticVersion(QStringLiteral("1.2.3 rc1")),
         "rejects whitespace and non-SemVer separators");
}

void testFingerprintNormalization() {
  const QString separated =
      QStringLiteral("aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:"
                     "aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa:aa");
  expect(localflow::updates::normalizeSha256Fingerprint(separated) ==
             QString(64, QLatin1Char('A')),
         "normalizes a separated SHA-256 fingerprint");
  expect(localflow::updates::normalizeSha256Fingerprint(
             QStringLiteral("not-a-hash"))
             .isEmpty(),
         "rejects a malformed fingerprint");
}

void testManifestContract() {
  QString error;
  const auto parsed =
      localflow::updates::parseWindowsUpdateManifest(validManifest(), &error);
  expect(parsed.has_value(), "accepts the valid release manifest");
  if (!parsed) {
    qCritical().noquote() << "Parser detail:" << error;
  }
  if (parsed) {
    expect(parsed->version == QStringLiteral("2.4.1"),
           "retains the update version");
    expect(parsed->sizeBytes == 12345678, "retains the exact installer size");
    expect(parsed->sha256.size() == 64, "retains a normalized SHA-256 digest");
  }

  QByteArray wrongRepository = validManifest();
  wrongRepository.replace("yonif8/LocalFlow", "someone/LocalFlow");
  expect(!localflow::updates::parseWindowsUpdateManifest(wrongRepository),
         "rejects installers outside the official repository");

  QByteArray wrongTag = validManifest();
  wrongTag.replace("download/v2.4.1", "download/v2.4.0");
  expect(!localflow::updates::parseWindowsUpdateManifest(wrongTag),
         "requires the release tag to match the manifest version");

  QByteArray unsignedManifest = validManifest();
  unsignedManifest.replace("\"validAtPublication\": true",
                           "\"validAtPublication\": false");
  expect(!localflow::updates::parseWindowsUpdateManifest(unsignedManifest),
         "rejects a manifest without a LocalFlow release signature");

  QByteArray malformedSignature = validManifest();
  malformedSignature.replace("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==",
                             "not-base64");
  expect(!localflow::updates::parseWindowsUpdateManifest(malformedSignature),
         "rejects a malformed LocalFlow release signature");

  QByteArray extraField = validManifest();
  extraField.replace("\"schemaVersion\": 1,",
                     "\"schemaVersion\": 1, \"surprise\": true,");
  expect(!localflow::updates::parseWindowsUpdateManifest(extraField),
         "rejects unknown manifest fields");

  QByteArray oversized(localflow::updates::kMaximumManifestBytes + 1, ' ');
  expect(!localflow::updates::parseWindowsUpdateManifest(oversized),
         "rejects manifests over the fixed byte limit");

  QByteArray fractionalSize = validManifest();
  fractionalSize.replace("12345678", "12345678.5");
  expect(!localflow::updates::parseWindowsUpdateManifest(fractionalSize),
         "rejects a fractional installer size");
}

#ifdef Q_OS_WIN
void testWindowsVerificationHelperProtocol() {
  using localflow::updates::detail::acceptWindowsVerificationHelperResult;
  using localflow::updates::detail::runWindowsVerificationHelper;

  QString error;
  const QByteArray success = R"json({"ok":true,"schemaVersion":1})json";
  expect(acceptWindowsVerificationHelperResult(0, true, success, &error),
         "accepts an exact successful verifier response");
  expect(error.isEmpty(), "successful verifier response has no error");

  const QByteArray rejected =
      R"json({"error":"Windows rejected the signature.","ok":false,"schemaVersion":1})json";
  expect(!acceptWindowsVerificationHelperResult(2, true, rejected, &error),
         "accepts a coherent verifier rejection as failure");
  expect(error == QStringLiteral("Windows rejected the signature."),
         "preserves the bounded verifier rejection detail");
  expect(!acceptWindowsVerificationHelperResult(2, true, success, &error),
         "rejects success JSON with a failure exit code");
  expect(!acceptWindowsVerificationHelperResult(0, true, rejected, &error),
         "rejects failure JSON with a success exit code");
  expect(!acceptWindowsVerificationHelperResult(0, false, success, &error),
         "rejects a crashed verifier even if it wrote success JSON");
  expect(!acceptWindowsVerificationHelperResult(
             0, true, R"json({"extra":1,"ok":true,"schemaVersion":1})json",
             &error),
         "rejects extra verifier response fields");
  expect(!acceptWindowsVerificationHelperResult(
             0, true, QByteArrayLiteral("{not-json}"), &error),
         "rejects malformed verifier JSON");
  expect(!acceptWindowsVerificationHelperResult(
             0, true, R"json({"ok":"true","schemaVersion":1})json", &error),
         "rejects a verifier response with the wrong field types");
  expect(!acceptWindowsVerificationHelperResult(7, true, rejected, &error),
         "rejects an unrecognized verifier exit code");
  expect(!acceptWindowsVerificationHelperResult(
             0, true, QByteArrayLiteral(" {\"ok\":true,\"schemaVersion\":1}"),
             &error),
         "rejects noncanonical surrounding whitespace");
  expect(!acceptWindowsVerificationHelperResult(
             0, true,
             QByteArray(localflow::updates::detail::
                                kMaximumWindowsVerificationResponseBytes +
                            1,
                        'x'),
             &error),
         "rejects an oversized verifier response");

  QByteArray response;
  const int invalidRequest = runWindowsVerificationHelper({}, &response);
  expect(invalidRequest != 0, "rejects a verifier request with missing fields");
  expect(!acceptWindowsVerificationHelperResult(invalidRequest, true, response,
                                                &error),
         "returns a coherent bounded response for an invalid request");

  QTemporaryDir root;
  expect(root.isValid(), "creates Windows verifier test directory");
  const QString unsignedPath =
      root.filePath(QStringLiteral("unsigned update fixture.exe"));
  QFile unsignedInstaller(unsignedPath);
  expect(unsignedInstaller.open(QIODevice::WriteOnly),
         "creates unsigned Windows installer fixture");
  const QByteArray unsignedBytes = QByteArrayLiteral("not-authenticode-signed");
  expect(unsignedInstaller.write(unsignedBytes) == unsignedBytes.size(),
         "writes unsigned Windows installer fixture");
  unsignedInstaller.close();
  const QString unsignedHash = QString::fromLatin1(
      QCryptographicHash::hash(unsignedBytes, QCryptographicHash::Sha256)
          .toHex());
  const QString invalidReleaseSignature =
      QStringLiteral("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                     "AAAAAAAAAAAAAAAAAAAAAAAAAAAAA==");
  const int unsignedStatus = runWindowsVerificationHelper(
      {unsignedPath, QString::number(unsignedBytes.size()), unsignedHash,
       QStringLiteral("1.2.3"), invalidReleaseSignature},
      &response);
  expect(unsignedStatus != 0,
         "rejects an installer without a valid release signature");
  expect(!acceptWindowsVerificationHelperResult(unsignedStatus, true, response,
                                                &error),
         "reports an invalid release signature as a coherent verification "
         "failure");
  expect(error.contains(QStringLiteral("release signature")),
         "explains release signature rejection");

  const QByteArray testSeed = QByteArray::fromHex(
      "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  std::array<unsigned char, 32> testPublicKey{};
  std::array<unsigned char, 64> testPrivateKey{};
  std::array<unsigned char, 64> testSignature{};
  ed25519_create_keypair(
      testPublicKey.data(), testPrivateKey.data(),
      reinterpret_cast<const unsigned char *>(testSeed.constData()));
  ed25519_sign(
      testSignature.data(),
      reinterpret_cast<const unsigned char *>(unsignedBytes.constData()),
      static_cast<size_t>(unsignedBytes.size()), testPublicKey.data(),
      testPrivateKey.data());
  const QString validReleaseSignature = QString::fromLatin1(
      QByteArray(reinterpret_cast<const char *>(testSignature.data()),
                 static_cast<qsizetype>(testSignature.size()))
          .toBase64());
  const int freeInstallerStatus = runWindowsVerificationHelper(
      {unsignedPath, QString::number(unsignedBytes.size()), unsignedHash,
       QStringLiteral("1.2.3"), validReleaseSignature},
      &response);
  expect(freeInstallerStatus != 0,
         "continues validating a release-signed free installer");
  expect(!acceptWindowsVerificationHelperResult(freeInstallerStatus, true,
                                                response, &error),
         "reports a missing installer version identity coherently");
  expect(error.contains(QStringLiteral("version identity")),
         "passes a valid release signature without requiring Authenticode");

  const int uppercaseHashStatus = runWindowsVerificationHelper(
      {unsignedPath, QString::number(unsignedBytes.size()),
       unsignedHash.toUpper(), QStringLiteral("1.2.3"),
       invalidReleaseSignature},
      &response);
  expect(uppercaseHashStatus != 0,
         "rejects a noncanonical uppercase helper hash");
}
#endif

#ifdef Q_OS_LINUX
QByteArray readFile(const QString &path) {
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) {
    return {};
  }
  return input.readAll();
}

void testLinuxStagedCommit() {
  QTemporaryDir root;
  expect(root.isValid(), "creates Linux update test directory");
  const QString livePath = root.filePath(QStringLiteral("LocalFlow.AppImage"));
  QFile live(livePath);
  expect(live.open(QIODevice::WriteOnly), "creates live AppImage fixture");
  expect(live.write("current") == 7, "writes live AppImage fixture");
  live.close();
  const QFileDevice::Permissions permissions =
      QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
      QFileDevice::ReadGroup;
  expect(QFile::setPermissions(livePath, permissions),
         "sets live AppImage fixture permissions");

  const QString stagingDirectory =
      root.filePath(QStringLiteral("private-stage"));
  expect(QDir().mkpath(stagingDirectory), "creates private staging directory");
  const QString stagedPath =
      QDir(stagingDirectory).filePath(QStringLiteral("LocalFlow.AppImage"));
  QString error;
  expect(localflow::updates::detail::copyLinuxAppImageForStaging(
             livePath, stagedPath, permissions, &error),
         "copies live AppImage into staging");
  expect(readFile(livePath) == QByteArrayLiteral("current"),
         "staging does not mutate the live AppImage");

  QFile staged(stagedPath);
  expect(staged.open(QIODevice::WriteOnly | QIODevice::Truncate),
         "opens staged AppImage fixture");
  expect(staged.write("validated-update") == 16,
         "writes staged AppImage fixture");
  staged.close();
  expect(readFile(livePath) == QByteArrayLiteral("current"),
         "updated staging remains isolated before commit");
  expect(localflow::updates::detail::commitLinuxStagedAppImage(
             stagedPath, livePath, permissions, &error),
         "atomically commits validated staged AppImage");
  expect(readFile(livePath) == QByteArrayLiteral("validated-update"),
         "atomic commit replaces the live AppImage");
  expect(!QFileInfo::exists(stagedPath),
         "atomic commit consumes the staged path");
  expect((QFileInfo(livePath).permissions() & permissions) == permissions,
         "atomic commit preserves executable permissions");

  error.clear();
  expect(!localflow::updates::detail::commitLinuxStagedAppImage(
             stagedPath, livePath, permissions, &error),
         "rejects a missing staged AppImage");
  expect(!error.isEmpty(), "explains a rejected staged commit");
  expect(readFile(livePath) == QByteArrayLiteral("validated-update"),
         "a rejected commit leaves the live AppImage unchanged");
}
#endif

} // namespace

int main() {
  testSemanticVersions();
  testFingerprintNormalization();
  testManifestContract();
#ifdef Q_OS_WIN
  testWindowsVerificationHelperProtocol();
#endif
#ifdef Q_OS_LINUX
  testLinuxStagedCommit();
#endif
  if (failures == 0) {
    qInfo() << "UpdateManager tests passed";
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
