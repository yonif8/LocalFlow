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

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>

#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <winver.h>

#include "ed25519.h"

#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "wintrust.lib")
#endif
#endif

#ifndef LOCALFLOW_VERSION
#define LOCALFLOW_VERSION "0.0.0"
#endif

#ifndef LOCALFLOW_WINDOWS_SIGNER_SHA256
#define LOCALFLOW_WINDOWS_SIGNER_SHA256 ""
#endif

#ifndef LOCALFLOW_UPDATE_PUBLIC_ED_KEY
#define LOCALFLOW_UPDATE_PUBLIC_ED_KEY ""
#endif

namespace {

#ifdef Q_OS_WIN
constexpr auto kManifestUrl = "https://github.com/yonif8/LocalFlow/releases/"
                              "latest/download/windows-update.json";
constexpr int kManifestTimeoutMs = 15'000;
constexpr int kDownloadTimeoutMs = 10 * 60 * 1000;
constexpr int kWindowsVerificationTimeoutMs = 2 * 60 * 1000;
#elif defined(Q_OS_LINUX)
constexpr auto kReleasesUrl = "https://github.com/yonif8/LocalFlow/releases";
constexpr int kLinuxUpdateCheckTimeoutMs = 60'000;
constexpr int kLinuxInstallTimeoutMs = 10 * 60 * 1000;
#endif

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

#ifdef Q_OS_WIN
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

void cleanupAbandonedWindowsUpdateDirectories() {
  // The installer must survive the old app being closed by Inno Setup. Clean
  // completed/abandoned handoff directories on a later launch instead of
  // deleting the executable out from under a running installer.
  const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-1);
  QDir temporaryRoot(QDir::tempPath());
  const QFileInfoList candidates = temporaryRoot.entryInfoList(
      {QStringLiteral("LocalFlow-update-*")},
      QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Time);
  for (const QFileInfo &candidate : candidates) {
    if (candidate.isSymLink() || candidate.lastModified().toUTC() > cutoff) {
      continue;
    }
    QDir abandoned(candidate.absoluteFilePath());
    (void)abandoned.removeRecursively();
  }
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
#endif

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
verifyInstallerProductVersion(const std::wstring &nativePath,
                              const QString &expectedVersion) {
  constexpr DWORD kMaximumVersionInfoBytes = 16U * 1024U * 1024U;
  constexpr UINT kMaximumTranslations = 64U;
  struct LanguageAndCodePage {
    WORD language = 0;
    WORD codePage = 0;
  };

  DWORD ignoredHandle = 0;
  const DWORD versionInfoSize =
      GetFileVersionInfoSizeW(nativePath.c_str(), &ignoredHandle);
  if (versionInfoSize == 0 || versionInfoSize > kMaximumVersionInfoBytes) {
    return {false,
            QStringLiteral("The installer has no readable version identity.")};
  }

  std::vector<BYTE> versionInfo(versionInfoSize);
  if (GetFileVersionInfoW(nativePath.c_str(), 0, versionInfoSize,
                          versionInfo.data()) == FALSE) {
    return {false, QStringLiteral(
                       "The installer's version identity could not be read.")};
  }

  void *rawTranslations = nullptr;
  UINT translationBytes = 0;
  constexpr UINT kTranslationSize =
      static_cast<UINT>(sizeof(LanguageAndCodePage));
  if (VerQueryValueW(versionInfo.data(), L"\\VarFileInfo\\Translation",
                     &rawTranslations, &translationBytes) == FALSE ||
      rawTranslations == nullptr || translationBytes < kTranslationSize ||
      translationBytes % kTranslationSize != 0) {
    return {false,
            QStringLiteral("The installer has no valid version translation.")};
  }

  const UINT translationCount = translationBytes / kTranslationSize;
  if (translationCount > kMaximumTranslations) {
    return {false,
            QStringLiteral("The installer has an invalid version identity.")};
  }

  const auto *translations =
      static_cast<const LanguageAndCodePage *>(rawTranslations);
  for (UINT index = 0; index < translationCount; ++index) {
    const QString query =
        QStringLiteral("\\StringFileInfo\\%1%2\\ProductVersion")
            .arg(static_cast<qulonglong>(translations[index].language), 4, 16,
                 QLatin1Char('0'))
            .arg(static_cast<qulonglong>(translations[index].codePage), 4, 16,
                 QLatin1Char('0'));
    const std::wstring nativeQuery = query.toStdWString();
    void *rawProductVersion = nullptr;
    UINT productVersionLength = 0;
    if (VerQueryValueW(versionInfo.data(), nativeQuery.c_str(),
                       &rawProductVersion, &productVersionLength) == FALSE ||
        rawProductVersion == nullptr || productVersionLength == 0 ||
        productVersionLength > 129U) {
      return {false, QStringLiteral(
                         "The installer has no valid ProductVersion text.")};
    }

    const auto *productVersionCharacters =
        static_cast<const wchar_t *>(rawProductVersion);
    qsizetype characterCount = static_cast<qsizetype>(productVersionLength);
    if (productVersionCharacters[characterCount - 1] == L'\0') {
      --characterCount;
    }
    const QString productVersion =
        QString::fromWCharArray(productVersionCharacters, characterCount);
    if (characterCount == 0 || productVersion.contains(QChar(u'\0')) ||
        !localflow::updates::parseSemanticVersion(productVersion)) {
      return {false, QStringLiteral(
                         "The installer has an invalid ProductVersion text.")};
    }
    if (productVersion != expectedVersion) {
      return {false, QStringLiteral("The installer's ProductVersion does not "
                                    "match the update manifest.")};
    }
  }

  return {true, {}};
}

NativeVerificationResult
verifyInstallerAuthenticity(const QString &path, const qint64 expectedSize,
                            const QByteArray &expectedSha256,
                            const QByteArray &expectedEd25519Signature,
                            const QString &expectedFingerprint,
                            const QString &expectedVersion) {
  const QString normalizedExpected =
      localflow::updates::normalizeSha256Fingerprint(expectedFingerprint);
  if (!normalizedExpected.isEmpty() && normalizedExpected.size() != 64) {
    return {false,
            QStringLiteral(
                "This build has an invalid Authenticode signer configured.")};
  }
  const QByteArray encodedPublicKey =
      QByteArrayLiteral(LOCALFLOW_UPDATE_PUBLIC_ED_KEY);
  const auto publicKey = QByteArray::fromBase64Encoding(
      encodedPublicKey, QByteArray::AbortOnBase64DecodingErrors);
  if (!publicKey || publicKey.decoded.size() != 32 ||
      publicKey.decoded.toBase64() != encodedPublicKey ||
      expectedEd25519Signature.size() != 64) {
    return {
        false,
        QStringLiteral(
            "This build has no valid release-verification key configured.")};
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
  uchar *mappedInstaller = installer.map(0, expectedSize);
  if (mappedInstaller == nullptr) {
    return {false, QStringLiteral("The downloaded update could not be mapped "
                                  "for signature verification.")};
  }
  const bool releaseSignatureValid =
      ed25519_verify(reinterpret_cast<const unsigned char *>(
                         expectedEd25519Signature.constData()),
                     mappedInstaller, static_cast<size_t>(expectedSize),
                     reinterpret_cast<const unsigned char *>(
                         publicKey.decoded.constData())) == 1;
  installer.unmap(mappedInstaller);
  if (!releaseSignatureValid) {
    return {false, QStringLiteral("The downloaded update does not have a valid "
                                  "LocalFlow release signature.")};
  }
  installer.close();

  const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
  if (normalizedExpected.isEmpty()) {
    return verifyInstallerProductVersion(nativePath, expectedVersion);
  }
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

  constexpr DWORD kMinimumSignerCertificateInfoBytes =
      static_cast<DWORD>(sizeof(CERT_INFO));
  constexpr DWORD kMaximumSignerCertificateInfoBytes = 1024U * 1024U;
  DWORD signerCertificateInfoSize = 0;
  if (CryptMsgGetParam(cryptographicMessage, CMSG_SIGNER_CERT_INFO_PARAM, 0,
                       nullptr, &signerCertificateInfoSize) == FALSE ||
      signerCertificateInfoSize < kMinimumSignerCertificateInfoBytes ||
      signerCertificateInfoSize > kMaximumSignerCertificateInfoBytes) {
    CryptMsgClose(cryptographicMessage);
    CertCloseStore(certificateStore, 0);
    return {false,
            QStringLiteral("The update contains no readable signer identity.")};
  }
  std::vector<BYTE> signerCertificateInfoBuffer(signerCertificateInfoSize);
  if (CryptMsgGetParam(cryptographicMessage, CMSG_SIGNER_CERT_INFO_PARAM, 0,
                       signerCertificateInfoBuffer.data(),
                       &signerCertificateInfoSize) == FALSE) {
    CryptMsgClose(cryptographicMessage);
    CertCloseStore(certificateStore, 0);
    return {false,
            QStringLiteral("The update contains no readable signer identity.")};
  }
  const auto *signerCertificateInfo =
      reinterpret_cast<const CERT_INFO *>(signerCertificateInfoBuffer.data());
  PCCERT_CONTEXT signerCertificate = CertFindCertificateInStore(
      certificateStore, encoding, 0, CERT_FIND_SUBJECT_CERT,
      signerCertificateInfo, nullptr);

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
  return verifyInstallerProductVersion(nativePath, expectedVersion);
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
                     QStringLiteral("ed25519"), QStringLiteral("authenticode"),
                     QStringLiteral("silentArguments")}) ||
      !installer.value(QStringLiteral("fileName")).isString() ||
      !installer.value(QStringLiteral("url")).isString() ||
      !installer.value(QStringLiteral("sha256")).isString() ||
      !installer.value(QStringLiteral("ed25519")).isObject() ||
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

  const QJsonObject ed25519 =
      installer.value(QStringLiteral("ed25519")).toObject();
  if (!hasExactKeys(ed25519, {QStringLiteral("algorithm"),
                              QStringLiteral("validAtPublication"),
                              QStringLiteral("signature")}) ||
      ed25519.value(QStringLiteral("algorithm")).toString() !=
          QStringLiteral("ed25519") ||
      ed25519.value(QStringLiteral("validAtPublication")) != QJsonValue(true) ||
      !ed25519.value(QStringLiteral("signature")).isString()) {
    assignError(error, QStringLiteral("Update release signature is invalid."));
    return std::nullopt;
  }
  const QByteArray encodedSignature =
      ed25519.value(QStringLiteral("signature")).toString().toLatin1();
  const auto decodedSignature = QByteArray::fromBase64Encoding(
      encodedSignature, QByteArray::AbortOnBase64DecodingErrors);
  if (!decodedSignature || decodedSignature.decoded.size() != 64 ||
      decodedSignature.decoded.toBase64() != encodedSignature) {
    assignError(error, QStringLiteral("Update release signature is invalid."));
    return std::nullopt;
  }
  manifest.ed25519Signature = decodedSignature.decoded;

  const QJsonObject authenticode =
      installer.value(QStringLiteral("authenticode")).toObject();
  if (!hasExactKeys(authenticode,
                    {QStringLiteral("required"),
                     QStringLiteral("validAtPublication"),
                     QStringLiteral("signerCertificateSha256")}) ||
      !authenticode.value(QStringLiteral("required")).isBool() ||
      !authenticode.value(QStringLiteral("validAtPublication")).isBool()) {
    assignError(error,
                QStringLiteral("Update Authenticode metadata is invalid."));
    return std::nullopt;
  }
  manifest.authenticodeRequired =
      authenticode.value(QStringLiteral("required")).toBool();
  const bool validAuthenticode =
      authenticode.value(QStringLiteral("validAtPublication")).toBool();
  const QJsonValue signerValue =
      authenticode.value(QStringLiteral("signerCertificateSha256"));
  if (validAuthenticode) {
    if (!signerValue.isString()) {
      assignError(error,
                  QStringLiteral("Update Authenticode signer is invalid."));
      return std::nullopt;
    }
    manifest.signerCertificateSha256 =
        normalizeSha256Fingerprint(signerValue.toString());
    if (manifest.signerCertificateSha256.isEmpty()) {
      assignError(error,
                  QStringLiteral("Update Authenticode signer is invalid."));
      return std::nullopt;
    }
  } else if (!signerValue.isNull()) {
    assignError(error,
                QStringLiteral("Update Authenticode signer is invalid."));
    return std::nullopt;
  }
  if (manifest.authenticodeRequired && !validAuthenticode) {
    assignError(
        error,
        QStringLiteral("Update requires a missing Authenticode signature."));
    return std::nullopt;
  }
  return manifest;
}

#ifdef Q_OS_WIN
namespace detail {
namespace {

constexpr int kVerificationRejectedExitCode = 2;
constexpr int kInvalidVerificationRequestExitCode = 64;
constexpr qsizetype kMaximumVerificationErrorCharacters = 1000;

struct ParsedVerificationResponse {
  bool ok = false;
  QString error;
};

QByteArray encodeVerificationResponse(const bool ok, QString error = {}) {
  QJsonObject object{
      {QStringLiteral("schemaVersion"), 1},
      {QStringLiteral("ok"), ok},
  };
  if (!ok) {
    error = error.trimmed().left(kMaximumVerificationErrorCharacters);
    if (error.isEmpty()) {
      error = QStringLiteral("Windows rejected the downloaded update.");
    }
    object.insert(QStringLiteral("error"), error);
  }
  return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<ParsedVerificationResponse>
parseVerificationResponse(const QByteArray &response) {
  if (response.isEmpty() ||
      response.size() > kMaximumWindowsVerificationResponseBytes ||
      response.trimmed() != response || response.contains('\r') ||
      response.contains('\n')) {
    return std::nullopt;
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    return std::nullopt;
  }
  const QJsonObject object = document.object();
  const QJsonValue schema = object.value(QStringLiteral("schemaVersion"));
  const QJsonValue okValue = object.value(QStringLiteral("ok"));
  if (!schema.isDouble() || schema.toDouble() != 1.0 || !okValue.isBool()) {
    return std::nullopt;
  }
  const bool ok = okValue.toBool();
  if (ok) {
    if (!hasExactKeys(
            object, {QStringLiteral("schemaVersion"), QStringLiteral("ok")})) {
      return std::nullopt;
    }
    return ParsedVerificationResponse{true, {}};
  }
  if (!hasExactKeys(object, {QStringLiteral("schemaVersion"),
                             QStringLiteral("ok"), QStringLiteral("error")}) ||
      !object.value(QStringLiteral("error")).isString()) {
    return std::nullopt;
  }
  const QString responseError =
      object.value(QStringLiteral("error")).toString();
  if (responseError.isEmpty() ||
      responseError.size() > kMaximumVerificationErrorCharacters ||
      responseError.trimmed() != responseError ||
      responseError.contains(QChar(u'\0'))) {
    return std::nullopt;
  }
  return ParsedVerificationResponse{false, responseError};
}

} // namespace

int runWindowsVerificationHelper(const QStringList &arguments,
                                 QByteArray *response) {
  const auto rejectRequest = [response](const QString &message) {
    if (response != nullptr) {
      *response = encodeVerificationResponse(false, message);
    }
    return kInvalidVerificationRequestExitCode;
  };
  if (response == nullptr) {
    return kInvalidVerificationRequestExitCode;
  }
  response->clear();
  if (arguments.size() != 5) {
    return rejectRequest(
        QStringLiteral("The private update verification request is invalid."));
  }

  const QString installerPath = arguments[0];
  const QString sizeText = arguments[1];
  const QString hashText = arguments[2];
  const QString versionText = arguments[3];
  const QByteArray encodedEd25519Signature = arguments[4].toLatin1();
  const QFileInfo installerInfo(installerPath);
  if (installerPath.isEmpty() || installerPath.size() > 32'767 ||
      installerPath.contains(QChar(u'\0')) || !installerInfo.isAbsolute()) {
    return rejectRequest(
        QStringLiteral("The private update verification path is invalid."));
  }
  if (sizeText.isEmpty() || sizeText.size() > 19 || !isAsciiNumeric(sizeText) ||
      (sizeText.size() > 1 && sizeText.front() == QLatin1Char('0'))) {
    return rejectRequest(
        QStringLiteral("The private update verification size is invalid."));
  }
  bool sizeConverted = false;
  const qint64 expectedSize = sizeText.toLongLong(&sizeConverted, 10);
  if (!sizeConverted || expectedSize < 1 ||
      expectedSize > kMaximumInstallerBytes) {
    return rejectRequest(
        QStringLiteral("The private update verification size is invalid."));
  }
  static const QRegularExpression canonicalHash(
      QStringLiteral(R"(^[0-9a-f]{64}$)"));
  if (!canonicalHash.match(hashText).hasMatch()) {
    return rejectRequest(
        QStringLiteral("The private update verification hash is invalid."));
  }
  if (!parseSemanticVersion(versionText)) {
    return rejectRequest(
        QStringLiteral("The private update verification version is invalid."));
  }
  const auto ed25519Signature = QByteArray::fromBase64Encoding(
      encodedEd25519Signature, QByteArray::AbortOnBase64DecodingErrors);
  if (!ed25519Signature || ed25519Signature.decoded.size() != 64 ||
      ed25519Signature.decoded.toBase64() != encodedEd25519Signature) {
    return rejectRequest(
        QStringLiteral("The private update release signature is invalid."));
  }
  if (!installerInfo.exists() || !installerInfo.isFile() ||
      installerInfo.isSymLink()) {
    *response = encodeVerificationResponse(
        false,
        QStringLiteral("The downloaded update changed before verification."));
    return kVerificationRejectedExitCode;
  }

  const NativeVerificationResult native = verifyInstallerAuthenticity(
      installerInfo.absoluteFilePath(), expectedSize, hashText.toLatin1(),
      ed25519Signature.decoded,
      QString::fromLatin1(LOCALFLOW_WINDOWS_SIGNER_SHA256), versionText);
  *response = encodeVerificationResponse(native.ok, native.error);
  return native.ok ? 0 : kVerificationRejectedExitCode;
}

bool acceptWindowsVerificationHelperResult(const int exitCode,
                                           const bool normalExit,
                                           const QByteArray &response,
                                           QString *error) {
  if (error != nullptr) {
    error->clear();
  }
  if (!normalExit) {
    assignError(
        error,
        QStringLiteral("The Windows update verifier stopped unexpectedly."));
    return false;
  }
  const auto parsed = parseVerificationResponse(response);
  const bool coherentExit =
      parsed &&
      ((parsed->ok && exitCode == 0) ||
       (!parsed->ok && (exitCode == kVerificationRejectedExitCode ||
                        exitCode == kInvalidVerificationRequestExitCode)));
  if (!coherentExit) {
    assignError(error,
                QStringLiteral(
                    "The Windows update verifier returned an invalid result."));
    return false;
  }
  if (!parsed->ok) {
    assignError(error, parsed->error);
    return false;
  }
  return true;
}

} // namespace detail
#endif

#ifdef Q_OS_LINUX
namespace detail {

bool copyLinuxAppImageForStaging(const QString &sourcePath,
                                 const QString &stagedPath,
                                 const QFileDevice::Permissions permissions,
                                 QString *error) {
  if (sourcePath.isEmpty() || stagedPath.isEmpty() ||
      QFileInfo::exists(stagedPath)) {
    assignError(error,
                QStringLiteral("The private update staging path is invalid."));
    return false;
  }

  const QFileInfo source(sourcePath);
  const QFileInfo stagingParent(QFileInfo(stagedPath).absolutePath());
  if (!source.exists() || !source.isFile() || source.isSymLink() ||
      !stagingParent.exists() || !stagingParent.isDir() ||
      !QFile::copy(sourcePath, stagedPath)) {
    assignError(
        error,
        QStringLiteral("The current AppImage could not be copied into private "
                       "update staging."));
    return false;
  }
  if (!QFile::setPermissions(stagedPath, permissions)) {
    QFile::remove(stagedPath);
    assignError(error,
                QStringLiteral(
                    "The staged AppImage permissions could not be preserved."));
    return false;
  }
  return true;
}

bool commitLinuxStagedAppImage(const QString &stagedPath,
                               const QString &destinationPath,
                               const QFileDevice::Permissions permissions,
                               QString *error) {
  const QFileInfo staged(stagedPath);
  const QFileInfo destination(destinationPath);
  if (stagedPath.isEmpty() || destinationPath.isEmpty() ||
      staged.absoluteFilePath() == destination.absoluteFilePath() ||
      !staged.exists() || !staged.isFile() || staged.isSymLink() ||
      staged.size() <= 0 || !destination.exists() || !destination.isFile() ||
      destination.isSymLink()) {
    assignError(
        error,
        QStringLiteral(
            "The validated staged AppImage is no longer safe to install."));
    return false;
  }
  if (!QFile::setPermissions(stagedPath, permissions)) {
    assignError(
        error,
        QStringLiteral(
            "The validated AppImage permissions could not be preserved."));
    return false;
  }

  const QByteArray stagedNative = QFile::encodeName(stagedPath);
  const QByteArray destinationNative = QFile::encodeName(destinationPath);
  errno = 0;
  if (std::rename(stagedNative.constData(), destinationNative.constData()) !=
      0) {
    const int renameError = errno;
    assignError(
        error,
        QStringLiteral(
            "The validated AppImage could not be installed atomically (%1).")
            .arg(QString::fromLocal8Bit(std::strerror(renameError))));
    return false;
  }
  return true;
}

} // namespace detail
#endif

} // namespace localflow::updates

struct UpdateManager::Implementation {
  explicit Implementation(UpdateManager *ownerValue)
      : owner(ownerValue),
        currentVersionText(QString::fromUtf8(LOCALFLOW_VERSION)) {
#ifdef Q_OS_WIN
    cleanupAbandonedWindowsUpdateDirectories();
#endif
    operationTimeout.setSingleShot(true);
    QObject::connect(&operationTimeout, &QTimer::timeout, owner, [this] {
      const bool checking = manifestReply != nullptr;
      fail(checking
               ? QStringLiteral("The update check timed out. Please try again.")
               : QStringLiteral(
                     "The update download timed out. Please try again."));
    });
#ifdef Q_OS_WIN
    windowsVerificationTimeout.setSingleShot(true);
    QObject::connect(&windowsVerificationTimeout, &QTimer::timeout, owner,
                     [this] {
                       stopWindowsVerificationProcess(false);
                       fail(QStringLiteral(
                           "Windows did not finish checking the update's "
                           "signature within two minutes. Verification was "
                           "canceled; try again when your network connection "
                           "is stable."));
                     });
#endif
#ifdef Q_OS_LINUX
    linuxProcessTimeout.setSingleShot(true);
    QObject::connect(&linuxProcessTimeout, &QTimer::timeout, owner, [this] {
      auto *process = linuxUpdateProcess;
      if (process == nullptr) {
        return;
      }
      linuxUpdateProcess = nullptr;
      process->disconnect(owner);
      process->kill();
      (void)process->waitForFinished(1000);
      process->deleteLater();
      fail(state == UpdateManager::State::Updating
               ? QStringLiteral("The AppImage updater did not finish within "
                                "ten minutes. The existing app was left in "
                                "place; try again or update from Releases.")
               : QStringLiteral(
                     "The AppImage update check did not respond within one "
                     "minute. Try again or update from Releases."));
    });
#endif
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
#ifdef Q_OS_WIN
    stopWindowsVerificationProcess(true);
#endif
#ifdef Q_OS_LINUX
    linuxProcessTimeout.stop();
    if (linuxUpdateProcess != nullptr) {
      linuxUpdateProcess->disconnect(owner);
      linuxUpdateProcess->kill();
      (void)linuxUpdateProcess->waitForFinished(1000);
      delete linuxUpdateProcess;
      linuxUpdateProcess = nullptr;
    }
    cleanupLinuxStaging();
#endif
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
#ifdef Q_OS_WIN
    stopWindowsVerificationProcess(false);
#endif
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
#ifdef Q_OS_LINUX
    cleanupLinuxStaging();
#endif
    manifest.reset();
    progress = 0.0;
    if (silentCheck) {
      silentCheck = false;
      setState(UpdateManager::State::Idle, {});
      return;
    }
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

#ifdef Q_OS_WIN
  bool disposeWindowsVerificationProcess(QProcess *process,
                                         const bool deleteImmediately) {
    if (process == nullptr) {
      return true;
    }
    QObject::disconnect(process, nullptr, owner, nullptr);
    if (process->state() != QProcess::NotRunning) {
      process->kill();
      (void)process->waitForFinished(1000);
    }
    if (process->state() == QProcess::NotRunning) {
      if (deleteImmediately) {
        delete process;
      } else {
        process->deleteLater();
      }
      return true;
    }

    // TerminateProcess normally completes immediately. If a system component
    // delays teardown anyway, do not transfer that delay back to the GUI or
    // application shutdown. The OS reclaims the detached handles on exit; if
    // the event loop remains alive, the QProcess deletes itself when done.
    process->setParent(nullptr);
    QObject::connect(process,
                     qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     process, [process] { process->deleteLater(); });
    return false;
  }

  void stopWindowsVerificationProcess(const bool deleteImmediately) {
    windowsVerificationTimeout.stop();
    QProcess *process = windowsVerificationProcess;
    windowsVerificationProcess = nullptr;
    const bool stopped =
        disposeWindowsVerificationProcess(process, deleteImmediately);
    if (!stopped && temporaryDirectory != nullptr) {
      // A helper still holding the installer must not race QTemporaryDir's
      // recursive deletion. A later LocalFlow launch removes this stale folder.
      temporaryDirectory->setAutoRemove(false);
    }
    windowsVerificationOutput.clear();
    windowsVerificationOutputOverflow = false;
  }

  void appendWindowsVerificationOutput(QProcess *process) {
    if (windowsVerificationProcess != process) {
      return;
    }
    const QByteArray chunk = process->readAllStandardOutput();
    const qsizetype maximum =
        localflow::updates::detail::kMaximumWindowsVerificationResponseBytes;
    const qsizetype remaining =
        std::max<qsizetype>(0, maximum + 1 - windowsVerificationOutput.size());
    windowsVerificationOutput.append(chunk.left(remaining));
    if (chunk.size() > remaining ||
        windowsVerificationOutput.size() > maximum) {
      windowsVerificationOutputOverflow = true;
      process->kill();
    }
  }
#endif

  void checkForWindowsUpdate() {
#ifdef Q_OS_WIN
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
      if (!compiledSigner.isEmpty() &&
          (!manifest->authenticodeRequired ||
           manifest->signerCertificateSha256 != compiledSigner)) {
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
        if (silentCheck) {
          silentCheck = false;
          setState(UpdateManager::State::Idle, {});
        } else {
          setState(UpdateManager::State::UpToDate,
                   QStringLiteral("LocalFlow is up to date."),
                   QStringLiteral("Installed version: %1. Latest release: %2.")
                       .arg(currentVersionText, checkedVersion));
        }
        return;
      }
      availableVersion = manifest->version;
      silentCheck = false;
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
              .filePath(QStringLiteral("appimageupdatetool-x86_64.AppImage")),
          QDir(applicationDirectory)
              .absoluteFilePath(QStringLiteral(
                  "../libexec/localflow/appimageupdatetool-x86_64.AppImage")),
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
      auto *updater = new QProcess(owner);
      linuxUpdateProcess = updater;
      linuxUpdaterOutput.clear();
      updater->setProgram(linuxUpdaterPath);
      updater->setArguments({QStringLiteral("--check-for-update"),
                             QStringLiteral("--"), linuxAppImagePath});
      updater->setProcessEnvironment(linuxUpdaterEnvironment());
      updater->setWorkingDirectory(QFileInfo(linuxAppImagePath).absolutePath());
      updater->setProcessChannelMode(QProcess::MergedChannels);
      QObject::connect(updater, &QProcess::readyRead, owner,
                       [this, updater] { appendLinuxUpdaterOutput(updater); });
      QObject::connect(
          updater, &QProcess::errorOccurred, owner,
          [this, updater](const QProcess::ProcessError processError) {
            if (linuxUpdateProcess != updater ||
                processError != QProcess::FailedToStart) {
              return;
            }
            linuxUpdateProcess = nullptr;
            linuxProcessTimeout.stop();
            const QString error = updater->errorString();
            updater->deleteLater();
            fail(QStringLiteral("The bundled updater could not start: %1")
                     .arg(error));
          });
      QObject::connect(
          updater, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          owner,
          [this, updater](const int exitCode,
                          const QProcess::ExitStatus exitStatus) {
            if (linuxUpdateProcess != updater) {
              return;
            }
            appendLinuxUpdaterOutput(updater);
            linuxUpdateProcess = nullptr;
            linuxProcessTimeout.stop();
            updater->deleteLater();
            if (exitStatus == QProcess::NormalExit && exitCode == 0) {
              if (silentCheck) {
                silentCheck = false;
                setState(UpdateManager::State::Idle, {});
              } else {
                setState(UpdateManager::State::UpToDate,
                         QStringLiteral("LocalFlow is up to date."));
              }
              return;
            }
            if (exitStatus == QProcess::NormalExit && exitCode == 1) {
              silentCheck = false;
              setState(
                  UpdateManager::State::ReadyToInstall,
                  QStringLiteral("A LocalFlow update is available."),
                  QStringLiteral("Choose Update LocalFlow to install it in "
                                 "place. Nothing is uploaded."));
              return;
            }
            fail(linuxUpdaterFailure(exitCode));
          });
      setState(UpdateManager::State::Checking,
               QStringLiteral("Checking for updates…"));
      updater->start();
      linuxProcessTimeout.start(kLinuxUpdateCheckTimeoutMs);
      return;
    }
    if (silentCheck) {
      silentCheck = false;
      setState(UpdateManager::State::Idle, {});
    } else {
      setState(
          UpdateManager::State::ExternalUpdateAvailable,
          QStringLiteral("Use the LocalFlow Releases page for updates."),
          linuxAppImagePath.isEmpty()
              ? QStringLiteral(
                    "This installation is not running as an AppImage. Choose "
                    "Open Releases to download the right package.")
              : QStringLiteral("The bundled AppImage updater is unavailable. "
                               "Choose Open Releases to update manually."));
    }
#else
    fail(QStringLiteral("Linux updating is unavailable on this platform."));
#endif
  }

#ifdef Q_OS_LINUX
  void cleanupLinuxStaging() {
    linuxStagedAppImagePath.clear();
    linuxOriginalAppImagePermissions = {};
    linuxStagingDirectory.reset();
  }

  bool prepareLinuxStaging(QString *error) {
    cleanupLinuxStaging();
    const QFileInfo current(linuxAppImagePath);
    if (!current.exists() || !current.isFile() || current.isSymLink()) {
      assignError(
          error,
          QStringLiteral(
              "The current AppImage is no longer available for updating."));
      return false;
    }

    linuxOriginalAppImagePermissions = current.permissions();
    linuxStagingDirectory = std::make_unique<QTemporaryDir>(
        QDir(current.absolutePath())
            .filePath(QStringLiteral(".localflow-update-XXXXXX")));
    if (!linuxStagingDirectory->isValid() ||
        !QFile::setPermissions(linuxStagingDirectory->path(),
                               QFileDevice::ReadOwner |
                                   QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner)) {
      cleanupLinuxStaging();
      assignError(
          error,
          QStringLiteral("A private update staging folder could not be created "
                         "beside the current AppImage."));
      return false;
    }

    linuxStagedAppImagePath =
        linuxStagingDirectory->filePath(current.fileName());
    if (!localflow::updates::detail::copyLinuxAppImageForStaging(
            linuxAppImagePath, linuxStagedAppImagePath,
            linuxOriginalAppImagePermissions, error)) {
      cleanupLinuxStaging();
      return false;
    }
    return true;
  }

  QProcessEnvironment linuxUpdaterEnvironment() const {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // AppRun gives the desktop process a private NeMo library path. A nested
    // AppImage must bootstrap with its own libraries instead of inheriting it.
    environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
    environment.insert(QStringLiteral("APPIMAGE_EXTRACT_AND_RUN"),
                       QStringLiteral("1"));
    return environment;
  }

  void appendLinuxUpdaterOutput(QProcess *updater) {
    if (linuxUpdateProcess != updater) {
      return;
    }
    linuxUpdaterOutput += updater->readAll();
    constexpr qsizetype kMaximumUpdaterDiagnostics = 16 * 1024;
    if (linuxUpdaterOutput.size() > kMaximumUpdaterDiagnostics) {
      linuxUpdaterOutput.remove(0, linuxUpdaterOutput.size() -
                                       kMaximumUpdaterDiagnostics);
    }
  }

  QString linuxUpdaterFailure(const int exitCode) const {
    const QString diagnostic =
        QString::fromUtf8(linuxUpdaterOutput).trimmed().right(1000);
    return diagnostic.isEmpty()
               ? QStringLiteral("The bundled updater exited with code %1.")
                     .arg(exitCode)
               : QStringLiteral("The bundled updater failed: %1")
                     .arg(diagnostic);
  }
#endif

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
    if (!manifest || windowsVerificationProcess != nullptr) {
      fail(QStringLiteral(
          "The downloaded update could not be prepared for verification."));
      return;
    }
    setState(
        UpdateManager::State::Verifying,
        QStringLiteral("Verifying the update…"),
        QStringLiteral(
            "Checking the LocalFlow release signature and file identity."));
    windowsVerificationOutput.clear();
    windowsVerificationOutputOverflow = false;
    auto *process = new QProcess(owner);
    windowsVerificationProcess = process;
    process->setProgram(QCoreApplication::applicationFilePath());
    process->setArguments(
        {QString::fromLatin1(
             localflow::updates::detail::kWindowsVerificationHelperMode),
         downloadedInstallerPath, QString::number(manifest->sizeBytes),
         QString::fromLatin1(manifest->sha256), manifest->version,
         QString::fromLatin1(manifest->ed25519Signature.toBase64())});
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setStandardErrorFile(QProcess::nullDevice());
    process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *arguments) {
          arguments->flags |= CREATE_NO_WINDOW;
        });
    QObject::connect(
        process, &QProcess::readyReadStandardOutput, owner,
        [this, process] { appendWindowsVerificationOutput(process); });
    QObject::connect(
        process, &QProcess::errorOccurred, owner,
        [this, process](const QProcess::ProcessError processError) {
          if (windowsVerificationProcess != process ||
              processError != QProcess::FailedToStart) {
            return;
          }
          windowsVerificationProcess = nullptr;
          windowsVerificationTimeout.stop();
          process->deleteLater();
          windowsVerificationOutput.clear();
          windowsVerificationOutputOverflow = false;
          fail(QStringLiteral(
              "Windows could not start the isolated update verifier."));
        });
    QObject::connect(
        process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        owner,
        [this, process](const int exitCode,
                        const QProcess::ExitStatus exitStatus) {
          if (windowsVerificationProcess != process) {
            return;
          }
          appendWindowsVerificationOutput(process);
          windowsVerificationProcess = nullptr;
          windowsVerificationTimeout.stop();
          process->deleteLater();
          const QByteArray response = std::move(windowsVerificationOutput);
          windowsVerificationOutput.clear();
          const bool overflow = windowsVerificationOutputOverflow;
          windowsVerificationOutputOverflow = false;
          QString verificationError;
          if (overflow || !localflow::updates::detail::
                              acceptWindowsVerificationHelperResult(
                                  exitCode, exitStatus == QProcess::NormalExit,
                                  response, &verificationError)) {
            fail(
                overflow
                    ? QStringLiteral(
                          "The Windows update verifier returned too much data.")
                    : verificationError);
            return;
          }
          progress = 1.0;
          setState(UpdateManager::State::ReadyToInstall,
                   QStringLiteral("LocalFlow %1 is ready to install.")
                       .arg(availableVersion),
                   QStringLiteral(
                       "Choose Install Update to open the verified installer. "
                       "Windows may show an Unknown publisher warning."));
        });
    process->start(QIODevice::ReadOnly);
    if (windowsVerificationProcess == process) {
      windowsVerificationTimeout.start(kWindowsVerificationTimeoutMs);
    }
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
      if (linuxUpdateProcess != nullptr) {
        return;
      }
      QString stagingError;
      if (!prepareLinuxStaging(&stagingError)) {
        fail(stagingError +
             QStringLiteral(" The existing AppImage was left unchanged."));
        return;
      }
      auto *updater = new QProcess(owner);
      linuxUpdateProcess = updater;
      linuxUpdaterOutput.clear();
      updater->setProgram(linuxUpdaterPath);
      updater->setArguments({QStringLiteral("--overwrite"),
                             QStringLiteral("--"), linuxStagedAppImagePath});
      updater->setProcessEnvironment(linuxUpdaterEnvironment());
      updater->setWorkingDirectory(linuxStagingDirectory->path());
      updater->setProcessChannelMode(QProcess::MergedChannels);
      QObject::connect(updater, &QProcess::readyRead, owner,
                       [this, updater] { appendLinuxUpdaterOutput(updater); });
      QObject::connect(
          updater, &QProcess::errorOccurred, owner,
          [this, updater](const QProcess::ProcessError processError) {
            if (linuxUpdateProcess != updater ||
                processError != QProcess::FailedToStart) {
              return;
            }
            linuxUpdateProcess = nullptr;
            linuxProcessTimeout.stop();
            const QString error = updater->errorString();
            updater->deleteLater();
            fail(QStringLiteral("The bundled updater could not start: %1")
                     .arg(error) +
                 QStringLiteral(" The existing AppImage was left unchanged."));
          });
      QObject::connect(
          updater, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          owner,
          [this, updater](const int exitCode,
                          const QProcess::ExitStatus exitStatus) {
            if (linuxUpdateProcess != updater) {
              return;
            }
            appendLinuxUpdaterOutput(updater);
            linuxUpdateProcess = nullptr;
            linuxProcessTimeout.stop();
            updater->deleteLater();
            if (exitStatus == QProcess::NormalExit && exitCode == 0) {
              QString commitError;
              const QString stagedPath = linuxStagedAppImagePath;
              if (!localflow::updates::detail::commitLinuxStagedAppImage(
                      stagedPath, linuxAppImagePath,
                      linuxOriginalAppImagePermissions, &commitError)) {
                fail(commitError +
                     QStringLiteral(
                         " The existing AppImage was left unchanged."));
                return;
              }
              // The staged path has moved to the live path. Clearing it before
              // removing the private directory ensures cleanup cannot touch the
              // newly installed AppImage.
              linuxStagedAppImagePath.clear();
              cleanupLinuxStaging();
              progress = 1.0;
              setState(UpdateManager::State::UpToDate,
                       QStringLiteral("LocalFlow was updated."),
                       QStringLiteral("Restart LocalFlow when convenient to "
                                      "use the new version."));
              return;
            }
            fail(linuxUpdaterFailure(exitCode) +
                 QStringLiteral(" The existing AppImage was left unchanged."));
          });
      setState(
          UpdateManager::State::Updating, QStringLiteral("Updating LocalFlow…"),
          QStringLiteral("A private copy is being updated and verified. The "
                         "current AppImage stays in place until validation "
                         "passes."));
      updater->start();
      linuxProcessTimeout.start(kLinuxInstallTimeoutMs);
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
  QString downloadFailureReason;
  qint64 downloadedBytes = 0;
  double progress = 0.0;
  UpdateManager::State state = UpdateManager::State::Idle;
  bool silentCheck = false;
#ifdef Q_OS_LINUX
  QString linuxAppImagePath;
  QString linuxUpdaterPath;
  QString linuxStagedAppImagePath;
  QByteArray linuxUpdaterOutput;
  QProcess *linuxUpdateProcess = nullptr;
  QTimer linuxProcessTimeout;
  std::unique_ptr<QTemporaryDir> linuxStagingDirectory;
  QFileDevice::Permissions linuxOriginalAppImagePermissions{};
#endif
#ifdef Q_OS_WIN
  HANDLE trustedInstallerHandle = INVALID_HANDLE_VALUE;
  QProcess *windowsVerificationProcess = nullptr;
  QTimer windowsVerificationTimeout;
  QByteArray windowsVerificationOutput;
  bool windowsVerificationOutputOverflow = false;
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
         d_->state == State::Verifying || d_->state == State::Updating;
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
    if (d_->silentCheck) {
      d_->silentCheck = false;
      d_->statusText = QStringLiteral("Checking for updates…");
      d_->detailText.clear();
      emit changed();
    }
    return;
  }
  d_->cleanupTemporaryDownload();
  d_->manifest.reset();
  d_->availableVersion.clear();
  d_->progress = 0.0;
  d_->silentCheck = false;
#ifdef Q_OS_WIN
  d_->checkForWindowsUpdate();
#elif defined(Q_OS_LINUX)
  d_->prepareLinuxUpdate();
#else
  d_->fail(QStringLiteral("Updates are not available on this platform."));
#endif
}

void UpdateManager::checkForUpdatesSilently() {
  if (busy() || updateAvailable()) {
    return;
  }
  d_->cleanupTemporaryDownload();
  d_->manifest.reset();
  d_->availableVersion.clear();
  d_->progress = 0.0;
  d_->silentCheck = true;
#ifdef Q_OS_WIN
  d_->checkForWindowsUpdate();
#elif defined(Q_OS_LINUX)
  d_->prepareLinuxUpdate();
#else
  d_->silentCheck = false;
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
