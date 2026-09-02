#!/usr/bin/env bash
# Regenerates the certificate fixtures the TLS tests use.
#
# Everything here is a throwaway: the keys are committed on purpose so the hermetic
# suite needs no openssl at build time, and they are trusted by nothing.
set -euo pipefail

OUT="${1:-$(dirname "$0")/../tests/fixtures}"
mkdir -p "$OUT"
cd "$OUT"

PASSPHRASE="test-passphrase"

# A private certificate authority.
openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -keyout ca.key -out ca.crt \
  -subj "/CN=ImmichKSync Test CA/O=ImmichKSync" \
  -addext "basicConstraints=critical,CA:TRUE" 2>/dev/null

# A server certificate it signed, for the mutual-TLS server.
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr \
  -subj "/CN=localhost" 2>/dev/null
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 397 -sha256 \
  -extfile <(printf "subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n") 2>/dev/null

# A client identity, exported as PKCS#12 — what the user imports in Settings.
openssl req -newkey rsa:2048 -nodes -keyout client.key -out client.csr \
  -subj "/CN=ImmichKSync Test Client" 2>/dev/null
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out client.crt -days 397 -sha256 \
  -extfile <(printf "extendedKeyUsage=clientAuth\n") 2>/dev/null
openssl pkcs12 -export -out client.p12 -inkey client.key -in client.crt \
  -passout "pass:${PASSPHRASE}" 2>/dev/null

# A two-certificate PEM, so the "refuse to anchor a chain" path has a fixture.
cat server.crt ca.crt > chain.pem

rm -f server.csr client.csr ca.srl
echo "Certificate fixtures written to $(pwd)"
echo "Client PKCS#12 passphrase: ${PASSPHRASE}"
