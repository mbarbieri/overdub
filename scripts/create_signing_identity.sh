#!/bin/bash
# Creates the self-signed code-signing identity Overdub.app is signed with (ADR 0001, issue #71).
# A stable identity keeps camera and microphone permissions across rebuilds; ad-hoc signing does not.
# Usage: create_signing_identity.sh [identity name]   (default: Overdub Local Development)
# OVERDUB_SIGNING_KEYCHAIN picks the keychain (default: the login keychain);
# OVERDUB_SIGNING_KEYCHAIN_PASSWORD unlocks it when set (tests use a throwaway keychain).
set -euo pipefail

identity="${1:-Overdub Local Development}"
keychain="${OVERDUB_SIGNING_KEYCHAIN:-${HOME}/Library/Keychains/login.keychain-db}"

if security find-identity -p codesigning "${keychain}" | grep -qF "\"${identity}\""; then
    echo "create_signing_identity: '${identity}' already exists in ${keychain}; nothing to do"
    exit 0
fi

if [ -n "${OVERDUB_SIGNING_KEYCHAIN_PASSWORD:-}" ]; then
    security unlock-keychain -p "${OVERDUB_SIGNING_KEYCHAIN_PASSWORD}" "${keychain}"
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

cat > "${work_dir}/cert.cnf" <<CNF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = ${identity}
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
CNF

# /usr/bin/openssl is LibreSSL, whose PKCS#12 output `security import` reads.
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -config "${work_dir}/cert.cnf" -keyout "${work_dir}/key.pem" -out "${work_dir}/cert.pem" 2> /dev/null
/usr/bin/openssl pkcs12 -export -passout pass:overdub \
    -inkey "${work_dir}/key.pem" -in "${work_dir}/cert.pem" -out "${work_dir}/identity.p12"

# -T lets codesign use the private key without a keychain prompt on every build.
security import "${work_dir}/identity.p12" -k "${keychain}" -P overdub -T /usr/bin/codesign > /dev/null

if [ -n "${OVERDUB_SIGNING_KEYCHAIN_PASSWORD:-}" ]; then
    security set-key-partition-list -S apple-tool:,apple: -s -k "${OVERDUB_SIGNING_KEYCHAIN_PASSWORD}" "${keychain}" > /dev/null
fi

echo "create_signing_identity: created '${identity}' in ${keychain}"
echo "Re-run 'cmake --preset debug' (or 'make app') so the build signs with it."
