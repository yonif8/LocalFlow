#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "UpdateManager.hpp"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOperatingSystemVersion>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>

#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wintrust.lib")
#endif
#endif

#ifndef LOCALFLOW_VERSION
#define LOCALFLOW_VERSION "0.0.0"
#endif

#ifndef LOCALFLOW_WINDOWS_SIGNER_SHA256
#define LOCALFLOW_WINDOWS_SIGNER_SHA256 ""
#endif

namespace {

constexpr auto kManifestUrl = "https://github.com/yonif8/LocalFlow/releases/"
                              "latest/download/windows-update.json";
constexpr auto kReleasesUrl = "https://github.com/yonif8/LocalFlow/releases";
constexpr int kManifestTimeoutMs = 15'000;
constexpr int kDownloadTimeoutMs = 10 * 60 * 1000;

void assignError(QString *error, const QString &message) {
  if (error != nullptr) {
    *error = message;
  }
}

bool isAsciiAlphaNumericOrHyphen(const QChar character) {
  const ushort value = character.unicode();
  return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z') || value == '-';
}

bool isAsciiNumeric(const QString &value) {
  if (value.isEmpty()) {
    return false;
  }
  return std::all_of(value.cbegin(), value.cend(), [](const QChar character) {
    const ushort code = character.unicode();
    return code >= '0' && code <= '9';
  });
}

bool validateIdentifiers(const QString &value,
                         const bool rejectLeadingZeroForNumeric,
                         QStringList *output) {
  if (value.isEmpty()) {
    return false;
  }

  const QStringList identifiers = value.split('.', Qt::KeepEmptyParts);
  for (const QString &identifier : identifiers) {
    if (identifier.isEmpty() ||
        !std::all_of(identifier.cbegin(), identifier.cend(),
                     isAsciiAlphaNumericOrHyphen)) {
      return false;
    }
    if (rejectLeadingZeroForNumeric && isAsciiNumeric(identifier) &&
        identifier.size() > 1 && identifier.front() == '0') {
      return false;
    }
  }
  *output = identifiers;
  return true;
}

bool parseCoreNumber(const QString &value, std::uint32_t *output) {
  if (!isAsciiNumeric(value) || (value.size() > 1 && value.front() == '0')) {
    return false;
  }
  bool converted = false;
  const qulonglong number = value.toULongLong(&converted, 10);
  if (!converted || number > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  *output = static_cast<std::uint32_t>(number);
  return true;
}

bool hasExactKeys(const QJsonObject &object, QStringList expected) {
  QStringList actual = object.keys();
  actual.sort();
  expected.sort();
  return actual == expected;
}

std::optional<qint64> exactInteger(const QJsonValue &value,
                                   const qint64 minimum, const qint64 maximum) {
  if (!value.isDouble()) {
    return std::nullopt;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < static_cast<double>(minimum) ||
      number > static_cast<double>(maximum)) {
    return std::nullopt;
  }
  return static_cast<qint64>(number);
}

bool hasSafeHttpsShape(const QUrl &url) {
  return url.isValid() &&
         url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) ==
             0 &&
         url.port(-1) == -1 && url.userInfo().isEmpty() &&
         url.query().isEmpty() && url.fragment().isEmpty();
}

bool isAllowedDownloadedUrl(const QUrl &url) {
  if (!url.isValid() ||
      url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
      !url.userInfo().isEmpty() || !url.fragment().isEmpty() ||
      (url.port(-1) != -1 && url.port(-1) != 443)) {
    return false;
  }
  const QString host = url.host().toLower();
  return host == QStringLiteral("github.com") ||
         host == QStringLiteral("release-assets.githubusercontent.com");
}

QNetworkRequest networkRequest(const QUrl &url, const QString &currentVersion) {
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setMaximumRedirectsAllowed(5);
  request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                       QNetworkRequest::AlwaysNetwork);
  request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
  request.setRawHeader("Accept-Encoding", "identity");
  request.setRawHeader(
      "User-Agent",
      QStringLiteral("LocalFlow/%1").arg(currentVersion).toUtf8());
  return request;
}

QString networkFailureMessage(const QNetworkReply *reply,
                              const QString &operation) {
  if (reply->error() != QNetworkReply::NoError) {
    return QStringLiteral("%1 failed: %2").arg(operation, reply->errorString());
  }
  const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (status != 200) {
    return QStringLiteral("%1 failed: the server returned HTTP %2.")
        .arg(operation)
        .arg(status);
  }
  return {};
}

#ifdef Q_OS_WIN
QString windowsErrorCode(const LONG code) {
  return QStringLiteral("0x%1")
      .arg(static_cast<unsigned long>(code), 8, 16, QLatin1Char('0'))
      .toUpper();
}

struct NativeVerificationResult {
  bool ok = false;
  QString error;
};

NativeVerificationResult
verifyAuthenticodeAndSigner(const QString &path, const qint64 expectedSize,
                            const QByteArray &expectedSha256,
                            const QString &expectedFingerprint) {
  const QString normalizedExpected =
      localflow::updates::normalizeSha256Fingerprint(expectedFingerprint);
  if (normalizedExpected.size() != 64) {
    return {false,
            QStringLiteral(
                "This build has no valid trusted update signer configured.")};
  }

  QFile installer(path);
  if (!installer.open(QIODevice::ReadOnly) ||
      installer.size() != expectedSize) {
    return {false, QStringLiteral(
                       "The downloaded update changed before verification.")};
  }
  QCryptographicHash fileHash(QCryptographicHash::Sha256);
  QByteArray buffer(1024 * 1024, Qt::Uninitialized);
  qint64 totalRead = 0;
  while (!installer.atEnd()) {
    const qint64 bytesRead = installer.read(buffer.data(), buffer.size());
    if (bytesRead <= 0 || totalRead > expectedSize - bytesRead) {
      return {false,
              QStringLiteral(
                  "The downloaded update could not be verified completely.")};
    }
    fileHash.addData(QByteArrayView(buffer.constData(), bytesRead));
    totalRead += bytesRead;
  }
  if (totalRead != expectedSize ||
      fileHash.result().toHex().toLower() != expectedSha256) {
    return {false,
            QStringLiteral(
                "The downloaded update failed its final SHA-256 check.")};
  }
  installer.close();

  const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
  WINTRUST_FILE_INFO fileInfo{};
  fileInfo.cbStruct = static_cast<DWORD>(sizeof(fileInfo));
  fileInfo.pcwszFilePath = nativePath.c_str();

  WINTRUST_DATA trustData{};
  trustData.cbStruct = static_cast<DWORD>(sizeof(trustData));
  trustData.dwUIChoice = WTD_UI_NONE;
  trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
  trustData.dwUnionChoice = WTD_CHOICE_FILE;
  trustData.pFile = &fileInfo;
  trustData.dwStateAction = WTD_STATEACTION_VERIFY;
  trustData.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG trustStatus = WinVerifyTrust(nullptr, &action, &trustData);
  trustData.dwStateAction = WTD_STATEACTION_CLOSE;
  static_cast<void>(WinVerifyTrust(nullptr, &action, &trustData));
  if (trustStatus != ERROR_SUCCESS) {
    return {false,
            QStringLiteral(
                "Windows rejected the installer's digital signature (%1).")
                .arg(windowsErrorCode(trustStatus))};
  }

  HCERTSTORE certificateStore = nullptr;
  HCRYPTMSG cryptographicMessage = nullptr;
  DWORD encoding = 0;
  DWORD contentType = 0;
  DWORD formatType = 0;
  if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, nativePath.c_str(),
                       CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                       CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding,
                       &contentType, &formatType, &certificateStore,
                       &cryptographicMessage, nullptr) == FALSE) {
    return {false,
            QStringLiteral("The update signer certificate could not be read.")};
  }

  CERT_INFO signerCertificateInfo{};
  DWORD signerCertificateInfoSize =
      static_cast<DWORD>(sizeof(signerCertificateInfo));
  if (CryptMsgGetParam(cryptographicMessage, CMSG_SIGNER_CERT_INFO_PARAM, 0,
                       &signerCertificateInfo,
                       &signerCertificateInfoSize) == FALSE) {
    CryptMsgClose(cryptographicMessage);
    CertCloseStore(certificateStore, 0);
    return {false,
            QStringLiteral("The update contains no readable signer identity.")};
  }
  PCCERT_CONTEXT signerCertificate = CertFindCertificateInStore(
      certificateStore, encoding, 0, CERT_FIND_SUBJECT_CERT,
      &signerCertificateInfo, nullptr);

  QString actualFingerprint;
  if (signerCertificate != nullptr) {
    BYTE digest[32]{};
    DWORD digestSize = static_cast<DWORD>(sizeof(digest));
    if (CertGetCertificateContextProperty(signerCertificate,
                                          CERT_SHA256_HASH_PROP_ID, digest,
                                          &digestSize) != FALSE &&
        digestSize == static_cast<DWORD>(sizeof(digest))) {
      actualFingerprint =
          QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(digest),
                                         static_cast<qsizetype>(digestSize))
                                  .toHex())
              .toUpper();
    }
    CertFreeCertificateContext(signerCertificate);
  }
  CryptMsgClose(cryptographicMessage);
  CertCloseStore(certificateStore, 0);

  if (actualFingerprint.isEmpty()) {
    return {false,
            QStringLiteral(
                "The update signer's SHA-256 identity could not be read.")};
  }
  if (actualFingerprint != normalizedExpected) {
    return {false, QStringLiteral(
                       "The update was signed by an untrusted certificate.")};
  }
  return {true, {}};
}
#endif

} // namespace

namespace localflow::updates {

std::optional<SemanticVersion> parseSemanticVersion(const QString &text,
                                                    QString *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (text.isEmpty() || text.size() > 128 || text.trimmed() != text) {
    assignError(
        error,
        QStringLiteral("Version must be a non-empty canonical SemVer string."));
    return std::nullopt;
  }

  QString versionAndPrerelease = text;
  QString build;
  const qsizetype plus = versionAndPrerelease.indexOf('+');
  if (plus >= 0) {
    if (versionAndPrerelease.indexOf('+', plus + 1) >= 0) {
      assignError(
          error,
          QStringLiteral("Version contains more than one build separator."));
      return std::nullopt;
    }
    build = versionAndPrerelease.mid(plus + 1);
    versionAndPrerelease.truncate(plus);
  }

  QString core = versionAndPrerelease;
  QString prerelease;
  const qsizetype dash = versionAndPrerelease.indexOf('-');
  if (dash >= 0) {
    prerelease = versionAndPrerelease.mid(dash + 1);
    core.truncate(dash);
  }

  const QStringList coreNumbers = core.split('.', Qt::KeepEmptyParts);
  SemanticVersion parsed;
  if (coreNumbers.size() != 3 ||
      !parseCoreNumber(coreNumbers[0], &parsed.major) ||
      !parseCoreNumber(coreNumbers[1], &parsed.minor) ||
      !parseCoreNumber(coreNumbers[2], &parsed.patch)) {
    assignError(
        error,
        QStringLiteral("Version core must contain three canonical integers."));
    return std::nullopt;
  }
  if (dash >= 0 && !validateIdentifiers(prerelease, true, &parsed.prerelease)) {
    assignError(
        error, QStringLiteral("Version has an invalid prerelease identifier."));
    return std::nullopt;
  }
  if (plus >= 0 && !validateIdentifiers(build, false, &parsed.buildMetadata)) {
    assignError(error, QStringLiteral("Version has invalid build metadata."));
    return std::nullopt;
  }
  return parsed;
}

int compareSemanticVersions(const SemanticVersion &left,
                            const SemanticVersion &right) {
  if (left.major != right.major) {
    return left.major < right.major ? -1 : 1;
  }
  if (left.minor != right.minor) {
    return left.minor < right.minor ? -1 : 1;
  }
  if (left.patch != right.patch) {
    return left.patch < right.patch ? -1 : 1;
  }
  if (left.prerelease.isEmpty() || right.prerelease.isEmpty()) {
    if (left.prerelease.isEmpty() == right.prerelease.isEmpty()) {
      return 0;
    }
    return left.prerelease.isEmpty() ? 1 : -1;
  }

  const qsizetype count =
      std::min(left.prerelease.size(), right.prerelease.size());
  for (qsizetype index = 0; index < count; ++index) {
    const QString &leftIdentifier = left.prerelease[index];
    const QString &rightIdentifier = right.prerelease[index];
    const bool leftNumeric = isAsciiNumeric(leftIdentifier);
    const bool rightNumeric = isAsciiNumeric(rightIdentifier);
    if (leftNumeric && rightNumeric) {
      if (leftIdentifier.size() != rightIdentifier.size()) {
        return leftIdentifier.size() < rightIdentifier.size() ? -1 : 1;
      }
      const int comparison =
          QString::compare(leftIdentifier, rightIdentifier, Qt::CaseSensitive);
      if (comparison != 0) {
        return comparison < 0 ? -1 : 1;
      }
    } else if (leftNumeric != rightNumeric) {
      return leftNumeric ? -1 : 1;
    } else {
      const int comparison =
          QString::compare(leftIdentifier, rightIdentifier, Qt::CaseSensitive);
      if (comparison != 0) {
        return comparison < 0 ? -1 : 1;
      }
    }
  }
  if (left.prerelease.size() == right.prerelease.size()) {
    return 0;
  }
  return left.prerelease.size() < right.prerelease.size() ? -1 : 1;
}

QString normalizeSha256Fingerprint(const QString &fingerprint) {
  QString normalized;
  normalized.reserve(fingerprint.size());
  for (const QChar character : fingerprint) {
    if (character == ':' || character == '-' || character.isSpace()) {
      continue;
    }
    const ushort value = character.unicode();
    const bool hexadecimal = (value >= '0' && value <= '9') ||
                             (value >= 'A' && value <= 'F') ||
                             (value >= 'a' && value <= 'f');
    if (!hexadecimal) {
      return {};
    }
    normalized.append(character.toUpper());
  }
  return normalized.size() == 64 ? normalized : QString{};
}

bool isOfficialWindowsInstallerUrl(const QUrl &url, const QString &version,
                                   const QString &fileName) {
  if (!hasSafeHttpsShape(url) ||
      url.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) !=
          0) {
    return false;
  }
  static const QRegularExpression safeFileName(
      QStringLiteral(R"(^[A-Za-z0-9][A-Za-z0-9._-]{0,199}\.exe$)"),
      QRegularExpression::CaseInsensitiveOption);
  if (!safeFileName.match(fileName).hasMatch()) {
    return false;
  }

  const QString encodedPath = url.path(QUrl::FullyEncoded);
  const QStringList pathParts = encodedPath.split('/', Qt::SkipEmptyParts);
  if (pathParts.size() != 6 || pathParts[0] != QStringLiteral("yonif8") ||
      pathParts[1] != QStringLiteral("LocalFlow") ||
      pathParts[2] != QStringLiteral("releases") ||
      pathParts[3] != QStringLiteral("download") ||
      QUrl::fromPercentEncoding(pathParts[4].toLatin1()) !=
          QStringLiteral("v") + version ||
      QUrl::fromPercentEncoding(pathParts[5].toLatin1()) != fileName) {
    return false;
  }
  return true;
}

std::optional<WindowsUpdateManifest>
parseWindowsUpdateManifest(const QByteArray &json, QString *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (json.isEmpty() || json.size() > kMaximumManifestBytes) {
    assignError(error, QStringLiteral("Update manifest has an invalid size."));
    return std::nullopt;
  }

  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    assignError(error, QStringLiteral("Update manifest is not valid JSON."));
    return std::nullopt;
  }
  const QJsonObject root = document.object();
  if (!hasExactKeys(root,
                    {QStringLiteral("schemaVersion"), QStringLiteral("product"),
                     QStringLiteral("channel"), QStringLiteral("version"),
                     QStringLiteral("publishedAt"), QStringLiteral("platform"),
                     QStringLiteral("installer")})) {
    assignError(
        error,
        QStringLiteral("Update manifest has unexpected or missing fields."));
    return std::nullopt;
  }
  const auto schemaVersion =
      exactInteger(root.value(QStringLiteral("schemaVersion")), 1, 1);
  if (!schemaVersion ||
      root.value(QStringLiteral("product")).toString() !=
          QStringLiteral("LocalFlow") ||
      root.value(QStringLiteral("channel")).toString() !=
          QStringLiteral("stable") ||
      !root.value(QStringLiteral("version")).isString() ||
      !root.value(QStringLiteral("publishedAt")).isString() ||
      !root.value(QStringLiteral("platform")).isObject() ||
      !root.value(QStringLiteral("installer")).isObject()) {
    assignError(error, QStringLiteral("Update manifest identity is invalid."));
    return std::nullopt;
  }
  const QString publishedAt =
      root.value(QStringLiteral("publishedAt")).toString();
  if (publishedAt.size() > 64 ||
      !QDateTime::fromString(publishedAt, Qt::ISODate).isValid()) {
    assignError(error,
                QStringLiteral("Update manifest publication date is invalid."));
    return std::nullopt;
  }

  WindowsUpdateManifest manifest;
  manifest.version = root.value(QStringLiteral("version")).toString();
  QString versionError;
  const auto parsedVersion =
      parseSemanticVersion(manifest.version, &versionError);
  if (!parsedVersion) {
    assignError(error, QStringLiteral("Update manifest version is invalid: %1")
                           .arg(versionError));
    return std::nullopt;
  }
  manifest.parsedVersion = *parsedVersion;

  const QJsonObject platform =
      root.value(QStringLiteral("platform")).toObject();
  if (!hasExactKeys(platform,
                    {QStringLiteral("os"), QStringLiteral("architecture"),
                     QStringLiteral("minimumBuild")}) ||
      platform.value(QStringLiteral("os")).toString() !=
          QStringLiteral("windows") ||
      platform.value(QStringLiteral("architecture")).toString() !=
          QStringLiteral("x86_64")) {
    assignError(
        error, QStringLiteral("Update manifest targets a different platform."));
    return std::nullopt;
  }
  const auto minimumBuild = exactInteger(
      platform.value(QStringLiteral("minimumBuild")), 1, 1'000'000);
  if (!minimumBuild) {
    assignError(
        error,
        QStringLiteral("Update manifest has an invalid Windows build floor."));
    return std::nullopt;
  }
  manifest.minimumWindowsBuild = static_cast<int>(*minimumBuild);

  const QJsonObject installer =
      root.value(QStringLiteral("installer")).toObject();
  if (!hasExactKeys(installer,
                    {QStringLiteral("fileName"), QStringLiteral("url"),
                     QStringLiteral("sizeBytes"), QStringLiteral("sha256"),
                     QStringLiteral("authenticode"),
                     QStringLiteral("silentArguments")}) ||
      !installer.value(QStringLiteral("fileName")).isString() ||
      !installer.value(QStringLiteral("url")).isString() ||
      !installer.value(QStringLiteral("sha256")).isString() ||
      !installer.value(QStringLiteral("authenticode")).isObject() ||
      !installer.value(QStringLiteral("silentArguments")).isString() ||
      installer.value(QStringLiteral("silentArguments")).toString().size() >
          256) {
    assignError(error, QStringLiteral("Update installer metadata is invalid."));
    return std::nullopt;
  }
  manifest.fileName = installer.value(QStringLiteral("fileName")).toString();
  manifest.installerUrl =
      QUrl(installer.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
  const auto installerSize = exactInteger(
      installer.value(QStringLiteral("sizeBytes")), 1, kMaximumInstallerBytes);
  if (!installerSize ||
      !isOfficialWindowsInstallerUrl(manifest.installerUrl, manifest.version,
                                     manifest.fileName)) {
    assignError(
        error, QStringLiteral("Update installer location or size is invalid."));
    return std::nullopt;
  }
  manifest.sizeBytes = *installerSize;

  const QString hash = installer.value(QStringLiteral("sha256")).toString();
  static const QRegularExpression sha256Expression(
      QStringLiteral(R"(^[0-9A-Fa-f]{64}$)"));
  if (!sha256Expression.match(hash).hasMatch()) {
    assignError(error, QStringLiteral("Update installer SHA-256 is invalid."));
    return std::nullopt;
  }
  manifest.sha256 = hash.toLatin1().toLower();

  const QJsonObject authenticode =
      installer.value(QStringLiteral("authenticode")).toObject();
  if (!hasExactKeys(authenticode,
                    {QStringLiteral("required"),
                     QStringLiteral("validAtPublication"),
                     QStringLiteral("signerCertificateSha256")}) ||
      authenticode.value(QStringLiteral("required")) != QJsonValue(true) ||
      authenticode.value(QStringLiteral("validAtPublication")) !=
          QJsonValue(true) ||
      !authenticode.value(QStringLiteral("signerCertificateSha256"))
           .isString()) {
    assignError(
        error,
        QStringLiteral("Update manifest does not require a valid signature."));
    return std::nullopt;
  }
  manifest.signerCertificateSha256 = normalizeSha256Fingerprint(
      authenticode.value(QStringLiteral("signerCertificateSha256")).toString());
  if (manifest.signerCertificateSha256.isEmpty()) {
    assignError(error,
                QStringLiteral("Update manifest signer identity is invalid."));
    return std::nullopt;
  }
  return manifest;
}

} // namespace localflow::updates

struct UpdateManager::Implementation {
  struct VerificationResult {
    bool ok = false;
    QString error;
  };

  explicit Implementation(UpdateManager *ownerValue)
      : owner(ownerValue),
        currentVersionText(QString::fromUtf8(LOCALFLOW_VERSION)) {
    operationTimeout.setSingleShot(true);
    QObject::connect(&operationTimeout, &QTimer::timeout, owner, [this] {
      const bool checking = manifestReply != nullptr;
      fail(checking
               ? QStringLiteral("The update check timed out. Please try again.")
               : QStringLiteral(
                     "The update download timed out. Please try again."));
    });
  }

  ~Implementation() {
    if (manifestReply != nullptr) {
      manifestReply->disconnect(owner);
      manifestReply->abort();
      manifestReply = nullptr;
    }
    if (downloadReply != nullptr) {
      downloadReply->disconnect(owner);
      downloadReply->abort();
      downloadReply = nullptr;
    }
    if (verificationWatcher != nullptr && verificationWatcher->isRunning()) {
      verificationWatcher->waitForFinished();
    }
    cleanupTemporaryDownload();
  }

  void notify() const { emit owner->changed(); }

  void setState(const UpdateManager::State newState, QString status,
                QString detail = {}) {
    state = newState;
    statusText = std::move(status);
    detailText = std::move(detail);
    notify();
  }

  void fail(QString detail) {
    operationTimeout.stop();
    if (manifestReply != nullptr) {
      manifestReply->disconnect(owner);
      manifestReply->abort();
      manifestReply->deleteLater();
      manifestReply = nullptr;
    }
    if (downloadReply != nullptr) {
      downloadReply->disconnect(owner);
      downloadReply->abort();
      downloadReply->deleteLater();
      downloadReply = nullptr;
    }
    downloadFile.close();
    cleanupTemporaryDownload();
    manifest.reset();
    progress = 0.0;
    setState(UpdateManager::State::Error,
             QStringLiteral("Update could not be completed."),
             std::move(detail));
  }

  void cleanupTemporaryDownload() {
    downloadFile.close();
#ifdef Q_OS_WIN
    if (trustedInstallerHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(trustedInstallerHandle);
      trustedInstallerHandle = INVALID_HANDLE_VALUE;
    }
#endif
    downloadedInstallerPath.clear();
    temporaryDirectory.reset();
    downloadHash.reset();
    downloadedBytes = 0;
    downloadFailureReason.clear();
  }

  void checkForWindowsUpdate() {
#ifdef Q_OS_WIN
    const QString expectedSigner =
        localflow::updates::normalizeSha256Fingerprint(
            QString::fromLatin1(LOCALFLOW_WINDOWS_SIGNER_SHA256));
    if (expectedSigner.isEmpty()) {
      fail(QStringLiteral(
          "This build is not configured with a trusted update signer."));
      return;
    }
    QString currentVersionError;
    currentVersion = localflow::updates::parseSemanticVersion(
        currentVersionText, &currentVersionError);
    if (!currentVersion) {
      fail(QStringLiteral("This build has an invalid application version."));
      return;
    }

    manifestBytes.clear();
    setState(UpdateManager::State::Checking,
             QStringLiteral("Checking for updates…"));
    QNetworkRequest request(networkRequest(
        QUrl(QString::fromLatin1(kManifestUrl)), currentVersionText));
    manifestReply = network.get(request);
    QObject::connect(manifestReply, &QNetworkReply::readyRead, owner, [this] {
      if (manifestReply == nullptr) {
        return;
      }
      manifestBytes.append(manifestReply->readAll());
      if (manifestBytes.size() > localflow::updates::kMaximumManifestBytes) {
        manifestReply->abort();
      }
    });
    QObject::connect(manifestReply, &QNetworkReply::finished, owner, [this] {
      operationTimeout.stop();
      QNetworkReply *reply = manifestReply;
      manifestReply = nullptr;
      if (reply == nullptr) {
        return;
      }
      manifestBytes.append(reply->readAll());
      const QString networkError =
          networkFailureMessage(reply, QStringLiteral("Update check"));
      const QUrl finalUrl = reply->url();
      reply->deleteLater();
      if (!networkError.isEmpty()) {
        fail(networkError);
        return;
      }
      if (manifestBytes.size() > localflow::updates::kMaximumManifestBytes ||
          finalUrl.scheme().compare(QStringLiteral("https"),
                                    Qt::CaseInsensitive) != 0 ||
          (finalUrl.host().compare(QStringLiteral("github.com"),
                                   Qt::CaseInsensitive) != 0 &&
           finalUrl.host().compare(
               QStringLiteral("objects.githubusercontent.com"),
               Qt::CaseInsensitive) != 0 &&
           finalUrl.host().compare(
               QStringLiteral("release-assets.githubusercontent.com"),
               Qt::CaseInsensitive) != 0)) {
        fail(QStringLiteral(
            "The update manifest came from an unexpected location."));
        return;
      }

      QString parseError;
      manifest = localflow::updates::parseWindowsUpdateManifest(manifestBytes,
                                                                &parseError);
      manifestBytes.clear();
      if (!manifest) {
        fail(parseError);
        return;
      }
      const QString compiledSigner =
          localflow::updates::normalizeSha256Fingerprint(
              QString::fromLatin1(LOCALFLOW_WINDOWS_SIGNER_SHA256));
      if (manifest->signerCertificateSha256 != compiledSigner) {
        fail(QStringLiteral("The release metadata does not match this build's "
                            "trusted signer."));
        return;
      }
      const int windowsBuild =
          QOperatingSystemVersion::current().microVersion();
      if (windowsBuild > 0 && windowsBuild < manifest->minimumWindowsBuild) {
        fail(QStringLiteral("This update requires Windows build %1 or newer.")
                 .arg(manifest->minimumWindowsBuild));
        return;
      }
      if (localflow::updates::compareSemanticVersions(manifest->parsedVersion,
                                                      *currentVersion) <= 0) {
        const QString checkedVersion = manifest->version;
        manifest.reset();
        setState(UpdateManager::State::UpToDate,
                 QStringLiteral("LocalFlow is up to date."),
                 QStringLiteral("Installed version: %1. Latest release: %2.")
                     .arg(currentVersionText, checkedVersion));
        return;
      }
      availableVersion = manifest->version;
      setState(
          UpdateManager::State::UpdateAvailable,
          QStringLiteral("LocalFlow %1 is available.").arg(availableVersion),
          QStringLiteral(
              "Review the release, then choose Download Update to continue."));
    });
    operationTimeout.start(kManifestTimeoutMs);
#else
    fail(QStringLiteral("Windows updating is unavailable on this platform."));
#endif
  }

  void prepareLinuxUpdate() {
#ifdef Q_OS_LINUX
    linuxAppImagePath.clear();
    linuxUpdaterPath.clear();
    const QString appImageEnvironment = qEnvironmentVariable("APPIMAGE");
    const QFileInfo appImageInfo(appImageEnvironment);
    if (!appImageEnvironment.isEmpty() && appImageInfo.isAbsolute() &&
        appImageInfo.exists() && appImageInfo.isFile()) {
      linuxAppImagePath = appImageInfo.canonicalFilePath();
      const QString applicationDirectory =
          QCoreApplication::applicationDirPath();
      const QStringList updaterCandidates{
          QDir(applicationDirectory)
              .filePath(QStringLiteral("AppImageUpdate-x86_64.AppImage")),
          QDir(applicationDirectory)
              .absoluteFilePath(QStringLiteral(
                  "../libexec/localflow/AppImageUpdate-x86_64.AppImage")),
      };
      for (const QString &candidate : updaterCandidates) {
        const QFileInfo updaterInfo(candidate);
        if (updaterInfo.exists() && updaterInfo.isFile() &&
            updaterInfo.isExecutable()) {
          linuxUpdaterPath = updaterInfo.canonicalFilePath();
          break;
        }
      }
    }
    if (!linuxAppImagePath.isEmpty() && !linuxUpdaterPath.isEmpty()) {
      setState(UpdateManager::State::ReadyToInstall,
               QStringLiteral("AppImage updates are ready."),
               QStringLiteral("Choose Update LocalFlow to open the bundled "
                              "updater. Nothing is uploaded."));
      return;
    }
    setState(
        UpdateManager::State::ExternalUpdateAvailable,
        QStringLiteral("Use the LocalFlow Releases page for updates."),
        linuxAppImagePath.isEmpty()
            ? QStringLiteral(
                  "This installation is not running as an AppImage. Choose "
                  "Open Releases to download the right package.")
            : QStringLiteral("The bundled AppImage updater is unavailable. "
                             "Choose Open Releases to update manually."));
#else
    fail(QStringLiteral("Linux updating is unavailable on this platform."));
#endif
  }

  bool consumeDownloadBytes() {
#ifdef Q_OS_WIN
    if (downloadReply == nullptr || !manifest || !downloadHash) {
      downloadFailureReason =
          QStringLiteral("The update download ended unexpectedly.");
      return false;
    }
    const QByteArray chunk = downloadReply->readAll();
    if (chunk.isEmpty()) {
      return true;
    }
    const qint64 remaining = manifest->sizeBytes - downloadedBytes;
    if (remaining < 0 || chunk.size() > remaining) {
      downloadFailureReason = QStringLiteral(
          "The update download exceeded the signed manifest size.");
      return false;
    }
    if (downloadFile.write(chunk) != chunk.size()) {
      downloadFailureReason =
          QStringLiteral("The downloaded update could not be written safely.");
      return false;
    }
    downloadHash->addData(chunk);
    downloadedBytes += chunk.size();
    progress = static_cast<double>(downloadedBytes) /
               static_cast<double>(manifest->sizeBytes);
    notify();
#endif
    return true;
  }

  bool lockDownloadedInstaller() {
#ifdef Q_OS_WIN
    if (trustedInstallerHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(trustedInstallerHandle);
      trustedInstallerHandle = INVALID_HANDLE_VALUE;
    }
    const std::wstring nativePath =
        QDir::toNativeSeparators(downloadedInstallerPath).toStdWString();
    trustedInstallerHandle =
        CreateFileW(nativePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    return trustedInstallerHandle != INVALID_HANDLE_VALUE;
#else
    return false;
#endif
  }

  void startWindowsDownload() {
#ifdef Q_OS_WIN
    if (!manifest || state != UpdateManager::State::UpdateAvailable) {
      return;
    }
    cleanupTemporaryDownload();
    temporaryDirectory = std::make_unique<QTemporaryDir>(
        QDir(QDir::tempPath())
            .filePath(QStringLiteral("LocalFlow-update-XXXXXX")));
    if (!temporaryDirectory->isValid()) {
      fail(QStringLiteral(
          "A private temporary update folder could not be created."));
      return;
    }
    const QString partialPath = temporaryDirectory->filePath(
        manifest->fileName + QStringLiteral(".part"));
    downloadFile.setFileName(partialPath);
    if (!downloadFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
      fail(QStringLiteral("The temporary update file could not be created."));
      return;
    }
    downloadHash =
        std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    downloadedBytes = 0;
    downloadFailureReason.clear();
    progress = 0.0;
    setState(
        UpdateManager::State::Downloading,
        QStringLiteral("Downloading LocalFlow %1…").arg(manifest->version));

    QNetworkRequest request(
        networkRequest(manifest->installerUrl, currentVersionText));
    downloadReply = network.get(request);
    QObject::connect(
        downloadReply, &QNetworkReply::metaDataChanged, owner, [this] {
          if (downloadReply == nullptr || !manifest) {
            return;
          }
          const QVariant header =
              downloadReply->header(QNetworkRequest::ContentLengthHeader);
          const int status =
              downloadReply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                  .toInt();
          if (status == 200 && header.isValid() &&
              header.toLongLong() != manifest->sizeBytes) {
            downloadFailureReason = QStringLiteral(
                "The server reported an unexpected update size.");
            downloadReply->abort();
          }
        });
    QObject::connect(downloadReply, &QNetworkReply::readyRead, owner, [this] {
      if (!consumeDownloadBytes() && downloadReply != nullptr) {
        downloadReply->abort();
      }
    });
    QObject::connect(downloadReply, &QNetworkReply::finished, owner, [this] {
      operationTimeout.stop();
      QNetworkReply *reply = downloadReply;
      if (reply == nullptr || !manifest) {
        return;
      }
      const bool consumedAll = consumeDownloadBytes();
      downloadReply = nullptr;
      const QString networkError =
          networkFailureMessage(reply, QStringLiteral("Update download"));
      const QUrl finalUrl = reply->url();
      reply->deleteLater();
      downloadFile.close();
      if (!downloadFailureReason.isEmpty() || !consumedAll) {
        fail(downloadFailureReason.isEmpty()
                 ? QStringLiteral(
                       "The update download could not be completed safely.")
                 : downloadFailureReason);
        return;
      }
      if (!networkError.isEmpty()) {
        fail(networkError);
        return;
      }
      if (!isAllowedDownloadedUrl(finalUrl)) {
        fail(QStringLiteral(
            "The update download came from an unexpected location."));
        return;
      }
      if (downloadedBytes != manifest->sizeBytes || !downloadHash ||
          downloadHash->result().toHex().toLower() != manifest->sha256) {
        fail(QStringLiteral(
            "The downloaded update failed its size or SHA-256 check."));
        return;
      }
      const QString finalPath =
          temporaryDirectory->filePath(manifest->fileName);
      if (!QFile::rename(downloadFile.fileName(), finalPath)) {
        fail(QStringLiteral("The verified update could not be finalized."));
        return;
      }
      downloadedInstallerPath = finalPath;
      downloadHash.reset();
      if (!lockDownloadedInstaller()) {
        fail(QStringLiteral(
            "The downloaded update could not be locked for verification."));
        return;
      }
      verifyWindowsInstallerAsync();
    });
    operationTimeout.start(kDownloadTimeoutMs);
#endif
  }

  void verifyWindowsInstallerAsync() {
#ifdef Q_OS_WIN
    setState(
        UpdateManager::State::Verifying,
        QStringLiteral("Verifying the update…"),
        QStringLiteral(
            "Windows is checking the digital signature and trusted signer."));
    const QString installerPath = downloadedInstallerPath;
    const qint64 installerSize = manifest->sizeBytes;
    const QByteArray installerSha256 = manifest->sha256;
    const QString signerFingerprint =
        QString::fromLatin1(LOCALFLOW_WINDOWS_SIGNER_SHA256);
    verificationWatcher = new QFutureWatcher<VerificationResult>(owner);
    QObject::connect(
        verificationWatcher, &QFutureWatcher<VerificationResult>::finished,
        owner, [this] {
          QFutureWatcher<VerificationResult> *watcher = verificationWatcher;
          verificationWatcher = nullptr;
          const VerificationResult result = watcher->result();
          watcher->deleteLater();
          if (!result.ok) {
            fail(result.error);
            return;
          }
          progress = 1.0;
          setState(UpdateManager::State::ReadyToInstall,
                   QStringLiteral("LocalFlow %1 is ready to install.")
                       .arg(availableVersion),
                   QStringLiteral(
                       "Choose Install Update to open the signed installer."));
        });
    verificationWatcher->setFuture(QtConcurrent::run(
        [installerPath, installerSize, installerSha256, signerFingerprint] {
          const NativeVerificationResult native = verifyAuthenticodeAndSigner(
              installerPath, installerSize, installerSha256, signerFingerprint);
          return VerificationResult{native.ok, native.error};
        }));
#endif
  }

  void installWindowsUpdate() {
#ifdef Q_OS_WIN
    if (state != UpdateManager::State::ReadyToInstall ||
        downloadedInstallerPath.isEmpty() || temporaryDirectory == nullptr ||
        trustedInstallerHandle == INVALID_HANDLE_VALUE) {
      return;
    }
    const QFileInfo installer(downloadedInstallerPath);
    if (!installer.exists() || !installer.isFile()) {
      fail(QStringLiteral("The verified installer is no longer available."));
      return;
    }
    // The manifest's silentArguments value is deliberately ignored. This
    // fixed invocation shows the installer's own UI after the explicit user
    // action.
    if (!QProcess::startDetached(downloadedInstallerPath,
                                 {QStringLiteral("/CURRENTUSER")})) {
      fail(QStringLiteral("Windows could not open the verified installer."));
      return;
    }
    CloseHandle(trustedInstallerHandle);
    trustedInstallerHandle = INVALID_HANDLE_VALUE;
    temporaryDirectory->setAutoRemove(false);
    setState(
        UpdateManager::State::Idle,
        QStringLiteral("The LocalFlow installer is open."),
        QStringLiteral("Finish the installation in the Windows installer."));
    emit owner->installerStarted();
#endif
  }

  void installLinuxUpdate() {
#ifdef Q_OS_LINUX
    if (state == UpdateManager::State::ReadyToInstall &&
        !linuxUpdaterPath.isEmpty() && !linuxAppImagePath.isEmpty()) {
      QProcess updater;
      updater.setProgram(linuxUpdaterPath);
      updater.setArguments({linuxAppImagePath});
      QProcessEnvironment environment =
          QProcessEnvironment::systemEnvironment();
      environment.insert(QStringLiteral("APPIMAGE_EXTRACT_AND_RUN"),
                         QStringLiteral("1"));
      updater.setProcessEnvironment(environment);
      updater.setWorkingDirectory(QFileInfo(linuxUpdaterPath).absolutePath());
      if (!updater.startDetached()) {
        fail(QStringLiteral(
            "The bundled AppImage updater could not be opened."));
        return;
      }
      setState(
          UpdateManager::State::Idle,
          QStringLiteral("The AppImage updater is open."),
          QStringLiteral("Follow its prompts to finish updating LocalFlow."));
      emit owner->installerStarted();
      return;
    }
    if (state == UpdateManager::State::ExternalUpdateAvailable) {
      if (!QDesktopServices::openUrl(QUrl(QString::fromLatin1(kReleasesUrl)))) {
        fail(
            QStringLiteral("The LocalFlow Releases page could not be opened."));
        return;
      }
      setState(
          UpdateManager::State::Idle,
          QStringLiteral("The LocalFlow Releases page is open."),
          QStringLiteral("Download the package for your Linux distribution."));
    }
#endif
  }

  UpdateManager *owner = nullptr;
  QNetworkAccessManager network;
  QTimer operationTimeout;
  QNetworkReply *manifestReply = nullptr;
  QNetworkReply *downloadReply = nullptr;
  QFutureWatcher<VerificationResult> *verificationWatcher = nullptr;
  QFile downloadFile;
  std::unique_ptr<QTemporaryDir> temporaryDirectory;
  std::unique_ptr<QCryptographicHash> downloadHash;
  std::optional<localflow::updates::SemanticVersion> currentVersion;
  std::optional<localflow::updates::WindowsUpdateManifest> manifest;
  QByteArray manifestBytes;
  QString downloadedInstallerPath;
  QString currentVersionText;
  QString availableVersion;
  QString statusText;
  QString detailText;
  QString linuxAppImagePath;
  QString linuxUpdaterPath;
  QString downloadFailureReason;
  qint64 downloadedBytes = 0;
  double progress = 0.0;
  UpdateManager::State state = UpdateManager::State::Idle;
#ifdef Q_OS_WIN
  HANDLE trustedInstallerHandle = INVALID_HANDLE_VALUE;
#endif
};

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent), d_(std::make_unique<Implementation>(this)) {}

UpdateManager::~UpdateManager() = default;

UpdateManager::State UpdateManager::state() const { return d_->state; }
QString UpdateManager::statusText() const { return d_->statusText; }
QString UpdateManager::detailText() const { return d_->detailText; }
QString UpdateManager::availableVersion() const { return d_->availableVersion; }
bool UpdateManager::busy() const {
  return d_->state == State::Checking || d_->state == State::Downloading ||
         d_->state == State::Verifying;
}
bool UpdateManager::updateAvailable() const {
  return d_->state == State::UpdateAvailable ||
         d_->state == State::ReadyToInstall ||
         d_->state == State::ExternalUpdateAvailable;
}
bool UpdateManager::downloadAvailable() const {
  return d_->state == State::UpdateAvailable;
}
bool UpdateManager::readyToInstall() const {
  return d_->state == State::ReadyToInstall ||
         d_->state == State::ExternalUpdateAvailable;
}
double UpdateManager::progress() const { return d_->progress; }

void UpdateManager::checkForUpdates() {
  if (busy()) {
    return;
  }
  d_->cleanupTemporaryDownload();
  d_->manifest.reset();
  d_->availableVersion.clear();
  d_->progress = 0.0;
#ifdef Q_OS_WIN
  d_->checkForWindowsUpdate();
#elif defined(Q_OS_LINUX)
  d_->prepareLinuxUpdate();
#else
  d_->fail(QStringLiteral("Updates are not available on this platform."));
#endif
}

void UpdateManager::downloadUpdate() {
  if (busy()) {
    return;
  }
#ifdef Q_OS_WIN
  d_->startWindowsDownload();
#elif defined(Q_OS_LINUX)
  if (d_->state == State::ExternalUpdateAvailable ||
      d_->state == State::ReadyToInstall) {
    d_->detailText = QStringLiteral("Choose Update LocalFlow to continue.");
    emit changed();
  }
#endif
}

void UpdateManager::installUpdate() {
  if (busy()) {
    return;
  }
#ifdef Q_OS_WIN
  d_->installWindowsUpdate();
#elif defined(Q_OS_LINUX)
  d_->installLinuxUpdate();
#endif
}

void UpdateManager::dismissStatus() {
  if (busy()) {
    return;
  }
  d_->statusText.clear();
  d_->detailText.clear();
  emit changed();
}
