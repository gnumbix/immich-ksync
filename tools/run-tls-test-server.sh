#!/usr/bin/env bash
# A throwaway HTTPS server that demands a client certificate.
#
# Nothing hermetic can perform a TLS handshake — the stub transport replaces the whole
# network layer — so this is the only way to prove a certificate reaches the wire
# rather than merely that the app decided to send one.
#
#   ./tools/run-tls-test-server.sh &
#   make test-tls
set -euo pipefail

DIR="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURES="${DIR}/tests/fixtures"
PORT="${IMMICH_TEST_TLS_PORT:-24433}"

if [[ ! -f "${FIXTURES}/server.crt" ]]; then
    echo "Certificate fixtures are missing. Run ./tools/make-test-certificates.sh first." >&2
    exit 1
fi

echo "Serving https://localhost:${PORT} with mutual TLS required."
echo "  server certificate: ${FIXTURES}/server.crt"
echo "  accepted client CA: ${FIXTURES}/ca.crt"

# -Verify 1 *requires* a client certificate rather than merely requesting one, which
# is what makes a missing or wrong client certificate fail the handshake.
exec openssl s_server \
    -accept "${PORT}" \
    -cert "${FIXTURES}/server.crt" \
    -key "${FIXTURES}/server.key" \
    -CAfile "${FIXTURES}/ca.crt" \
    -Verify 1 \
    -www \
    -quiet
