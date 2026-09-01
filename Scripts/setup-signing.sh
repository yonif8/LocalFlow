#!/bin/bash
# setup-signing.sh — create a stable self-signed code-signing identity for
# LocalFlow in a dedicated keychain, so TCC permission grants survive rebuilds.
#
# Ad-hoc signatures change on every build, which makes macOS treat each build
# as a new app and drop its Input Monitoring / Accessibility grants. A stable
# certificate gives the app a stable designated requirement instead.
#
# The keychain is separate from login.keychain and protected by a throwaway
# password: the key inside only ever signs a locally-built app, so the
# password protects nothing sensitive.

set -euo pipefail

KC_NAME="localflow-signing.keychain"
KC_PASS="localflow-signing"
CERT_CN="LocalFlow Signing"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if security find-identity -v -p codesigning 2>/dev/null | grep -q "$CERT_CN"; then
    echo "==> Identity '$CERT_CN' already exists; nothing to do."
    exit 0
fi

echo "==> Generating self-signed code-signing certificate…"
cat > "$TMP/ext.cnf" <<EOF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $CERT_CN
[ext]
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
basicConstraints = critical,CA:false
EOF
openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
    -keyout "$TMP/key.pem" -out "$TMP/cert.pem" -config "$TMP/ext.cnf" 2>/dev/null
openssl pkcs12 -export -out "$TMP/identity.p12" \
    -inkey "$TMP/key.pem" -in "$TMP/cert.pem" -passout pass:temp

echo "==> Creating dedicated keychain '$KC_NAME'…"
security delete-keychain "$KC_NAME" 2>/dev/null || true
security create-keychain -p "$KC_PASS" "$KC_NAME"
security set-keychain-settings "$KC_NAME"           # never auto-lock
security unlock-keychain -p "$KC_PASS" "$KC_NAME"
security import "$TMP/identity.p12" -k "$KC_NAME" -P temp -T /usr/bin/codesign
# Pre-authorize codesign so signing never pops a keychain dialog.
security set-key-partition-list -S apple-tool:,apple:,codesign: -s \
    -k "$KC_PASS" "$KC_NAME" > /dev/null

echo "==> Adding keychain to the user search list…"
# shellcheck disable=SC2046
security list-keychains -d user -s \
    $(security list-keychains -d user | tr -d '" ') "$KC_NAME"

echo "==> Registering trust for the certificate (macOS may ask you to approve)…"
if ! security add-trusted-cert -p codeSign -k "$KC_NAME" "$TMP/cert.pem" 2>/dev/null; then
    echo "    User-level trust failed; trying without policy restriction…"
    security add-trusted-cert -k "$KC_NAME" "$TMP/cert.pem"
fi

echo "==> Verifying the identity is usable…"
security find-identity -v -p codesigning | grep "$CERT_CN" || {
    echo "error: identity not visible to codesign" >&2; exit 1; }

echo "==> Smoke-test signing a scratch binary…"
cp /bin/ls "$TMP/ls-copy"
if codesign --force --sign "$CERT_CN" "$TMP/ls-copy" 2>&1; then
    codesign --verify --verbose=2 "$TMP/ls-copy"
    echo "==> Success: '$CERT_CN' is ready. make-app.sh will pick it up automatically."
else
    echo "error: signing failed — the certificate may need trust approval in Keychain Access" >&2
    exit 1
fi
