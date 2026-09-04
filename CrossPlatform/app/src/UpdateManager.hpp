#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <cstdint>
#include <memory>
#include <optional>

namespace localflow::updates {

inline constexpr qsizetype kMaximumManifestBytes = 64 * 1024;
inline constexpr qint64 kMaximumInstallerBytes = 1024LL * 1024LL * 1024LL;

struct SemanticVersion {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
  QStringList prerelease;
  QStringList buildMetadata;
};

struct WindowsUpdateManifest {
  SemanticVersion parsedVersion;
  QString version;
  QString fileName;
  QUrl installerUrl;
  qint64 sizeBytes = 0;
  QByteArray sha256;
  QString signerCertificateSha256;
  int minimumWindowsBuild = 0;
};

// These helpers are intentionally side-effect free so the release-feed contract
// can be tested without networking or a Windows host.
[[nodiscard]] std::optional<SemanticVersion>
parseSemanticVersion(const QString &text, QString *error = nullptr);
[[nodiscard]] int compareSemanticVersions(const SemanticVersion &left,
                                          const SemanticVersion &right);
[[nodiscard]] QString normalizeSha256Fingerprint(const QString &fingerprint);
[[nodiscard]] bool isOfficialWindowsInstallerUrl(const QUrl &url,
                                                 const QString &version,
                                                 const QString &fileName);
[[nodiscard]] std::optional<WindowsUpdateManifest>
parseWindowsUpdateManifest(const QByteArray &json, QString *error = nullptr);

} // namespace localflow::updates

class UpdateManager final : public QObject {
  Q_OBJECT

  Q_PROPERTY(State state READ state NOTIFY changed)
  Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
  Q_PROPERTY(QString detailText READ detailText NOTIFY changed)
  Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY changed)
  Q_PROPERTY(bool downloadAvailable READ downloadAvailable NOTIFY changed)
  Q_PROPERTY(bool readyToInstall READ readyToInstall NOTIFY changed)
  Q_PROPERTY(double progress READ progress NOTIFY changed)

public:
  enum class State {
    Idle,
    Checking,
    UpToDate,
    UpdateAvailable,
    Downloading,
    Verifying,
    ReadyToInstall,
    ExternalUpdateAvailable,
    Error,
  };
  Q_ENUM(State)

  explicit UpdateManager(QObject *parent = nullptr);
  ~UpdateManager() override;

  [[nodiscard]] State state() const;
  [[nodiscard]] QString statusText() const;
  [[nodiscard]] QString detailText() const;
  [[nodiscard]] QString availableVersion() const;
  [[nodiscard]] bool busy() const;
  [[nodiscard]] bool updateAvailable() const;
  [[nodiscard]] bool downloadAvailable() const;
  [[nodiscard]] bool readyToInstall() const;
  [[nodiscard]] double progress() const;

  // None of these actions is automatic. A UI must invoke each successive
  // action after obtaining the user's confirmation.
  Q_INVOKABLE void checkForUpdates();
  Q_INVOKABLE void downloadUpdate();
  Q_INVOKABLE void installUpdate();
  Q_INVOKABLE void dismissStatus();

signals:
  void changed();
  void installerStarted();

private:
  struct Implementation;
  std::unique_ptr<Implementation> d_;
};
