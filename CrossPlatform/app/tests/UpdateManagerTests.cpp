#include "../src/UpdateManager.hpp"

#include <QDebug>

#include <cstdlib>

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
    "minimumBuild": 19041
  },
  "installer": {
    "fileName": "LocalFlow-2.4.1-windows-x64.exe",
    "url": "https://github.com/yonif8/LocalFlow/releases/download/v2.4.1/LocalFlow-2.4.1-windows-x64.exe",
    "sizeBytes": 12345678,
    "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "authenticode": {
      "required": true,
      "validAtPublication": true,
      "signerCertificateSha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    },
    "silentArguments": "/CURRENTUSER /VERYSILENT /SUPPRESSMSGBOXES /NORESTART"
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
  unsignedManifest.replace("\"required\": true", "\"required\": false");
  expect(!localflow::updates::parseWindowsUpdateManifest(unsignedManifest),
         "rejects a manifest that does not require Authenticode");

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

} // namespace

int main() {
  testSemanticVersions();
  testFingerprintNormalization();
  testManifestContract();
  if (failures == 0) {
    qInfo() << "UpdateManager tests passed";
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
