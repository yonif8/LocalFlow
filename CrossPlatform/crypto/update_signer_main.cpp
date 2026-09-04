#include "ed25519.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

#ifndef LOCALFLOW_UPDATE_PUBLIC_ED_KEY
#define LOCALFLOW_UPDATE_PUBLIC_ED_KEY ""
#endif

namespace {

constexpr qint64 kMaximumInstallerBytes = 1024LL * 1024LL * 1024LL;

void wipe(QByteArray &bytes) {
  volatile char *data = bytes.data();
  for (qsizetype index = 0; index < bytes.size(); ++index) {
    data[index] = 0;
  }
  bytes.clear();
}

template <std::size_t Size> void wipe(std::array<unsigned char, Size> &bytes) {
  volatile unsigned char *data = bytes.data();
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    data[index] = 0;
  }
}

int fail(const char *message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  const QStringList arguments = application.arguments();
  if (arguments.size() != 2) {
    return fail("usage: localflow_update_signer <installer>");
  }

  const QByteArray encodedPublicKey =
      QByteArrayLiteral(LOCALFLOW_UPDATE_PUBLIC_ED_KEY);
  const auto publicKeyResult = QByteArray::fromBase64Encoding(
      encodedPublicKey, QByteArray::AbortOnBase64DecodingErrors);
  if (!publicKeyResult || publicKeyResult.decoded.size() != 32 ||
      publicKeyResult.decoded.toBase64() != encodedPublicKey) {
    return fail("The compiled LocalFlow update public key is invalid.");
  }

  QByteArray encodedSeed = qgetenv("LOCALFLOW_UPDATE_ED25519_PRIVATE_KEY");
  qunsetenv("LOCALFLOW_UPDATE_ED25519_PRIVATE_KEY");
  const bool canonicalSeedText = encodedSeed == encodedSeed.trimmed();
  auto seedResult = QByteArray::fromBase64Encoding(
      encodedSeed, QByteArray::AbortOnBase64DecodingErrors);
  if (!canonicalSeedText || !seedResult || seedResult.decoded.size() != 32 ||
      seedResult.decoded.toBase64() != encodedSeed) {
    wipe(encodedSeed);
    if (seedResult) {
      wipe(seedResult.decoded);
    }
    return fail("LOCALFLOW_UPDATE_ED25519_PRIVATE_KEY is invalid.");
  }
  wipe(encodedSeed);
  QByteArray seed = std::move(seedResult.decoded);

  std::array<unsigned char, 32> derivedPublicKey{};
  std::array<unsigned char, 64> privateKey{};
  ed25519_create_keypair(
      derivedPublicKey.data(), privateKey.data(),
      reinterpret_cast<const unsigned char *>(seed.constData()));
  wipe(seed);
  if (!std::equal(derivedPublicKey.begin(), derivedPublicKey.end(),
                  reinterpret_cast<const unsigned char *>(
                      publicKeyResult.decoded.constData()))) {
    wipe(privateKey);
    return fail(
        "The update private key does not match LocalFlow's public key.");
  }

  QFile installer(arguments[1]);
  const QFileInfo installerInfo(installer);
  if (!installerInfo.isAbsolute() || installerInfo.isSymLink() ||
      !installer.open(QIODevice::ReadOnly) || installer.size() < 1 ||
      installer.size() > kMaximumInstallerBytes) {
    wipe(privateKey);
    return fail("The installer cannot be signed safely.");
  }
  uchar *mapped = installer.map(0, installer.size());
  if (mapped == nullptr) {
    wipe(privateKey);
    return fail("The installer could not be mapped for signing.");
  }

  std::array<unsigned char, 64> signature{};
  ed25519_sign(signature.data(), mapped, static_cast<size_t>(installer.size()),
               derivedPublicKey.data(), privateKey.data());
  wipe(privateKey);
  installer.unmap(mapped);
  installer.close();

  const QByteArray encodedSignature(
      reinterpret_cast<const char *>(signature.data()),
      static_cast<qsizetype>(signature.size()));
  const QByteArray output = encodedSignature.toBase64() + '\n';
  if (std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()),
                  stdout) != static_cast<size_t>(output.size())) {
    return fail("The update signature could not be written.");
  }
  return 0;
}
