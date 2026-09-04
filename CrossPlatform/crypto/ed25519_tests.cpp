#include "ed25519.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>

namespace {

template <std::size_t Size>
std::array<unsigned char, Size> decodeHex(const char *hex) {
  const auto nibble = [](const char character) -> unsigned char {
    if (character >= '0' && character <= '9') {
      return static_cast<unsigned char>(character - '0');
    }
    return static_cast<unsigned char>(character - 'a' + 10);
  };
  std::array<unsigned char, Size> bytes{};
  for (std::size_t index = 0; index < Size; ++index) {
    bytes[index] = static_cast<unsigned char>((nibble(hex[index * 2]) << 4) |
                                              nibble(hex[index * 2 + 1]));
  }
  return bytes;
}

} // namespace

int main() {
  // RFC 8032, section 7.1, test vector 1 (empty message).
  const auto seed = decodeHex<32>("9d61b19deffd5a60ba844af492ec2cc4"
                                  "4449c5697b326919703bac031cae7f60");
  const auto expectedPublic = decodeHex<32>("d75a980182b10ab7d54bfed3c964073a"
                                            "0ee172f3daa62325af021a68f707511a");
  const auto expectedSignature =
      decodeHex<64>("e5564300c360ac729086e2cc806e828a"
                    "84877f1eb8e5d974d873e06522490155"
                    "5fb8821590a33bacc61e39701cf9b46b"
                    "d25bf5f0595bbe24655141438e7a100b");

  std::array<unsigned char, 32> publicKey{};
  std::array<unsigned char, 64> privateKey{};
  std::array<unsigned char, 64> signature{};
  const unsigned char emptyMessage = 0;
  ed25519_create_keypair(publicKey.data(), privateKey.data(), seed.data());
  ed25519_sign(signature.data(), &emptyMessage, 0, publicKey.data(),
               privateKey.data());

  if (publicKey != expectedPublic || signature != expectedSignature ||
      ed25519_verify(signature.data(), &emptyMessage, 0, publicKey.data()) !=
          1) {
    std::fputs("Ed25519 RFC 8032 contract failed.\n", stderr);
    return 1;
  }
  signature[0] ^= 1;
  if (ed25519_verify(signature.data(), &emptyMessage, 0, publicKey.data()) !=
      0) {
    std::fputs("Ed25519 tamper rejection failed.\n", stderr);
    return 1;
  }
  std::fill(privateKey.begin(), privateKey.end(), 0);
  return 0;
}
