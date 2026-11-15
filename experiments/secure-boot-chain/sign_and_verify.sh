#!/bin/bash
# Sign an HSS boot payload offline and check it two ways. Nothing is flashed and nothing touches a board.
#
#   experiments/secure-boot-chain/sign_and_verify.sh [amp.yaml]
#
# Needs: the HSS payload generator and the files that the YAML names (the default YAML and its
# images come from firmware/build_payload.sh, which writes them to build/payload-bm), OpenSSL, and
# Python with the 'cryptography' package.
#
#   HSS_PAYLOAD_GENERATOR   path to hss-payload-generator
#                           (default: the one built by Yocto, else the one on PATH)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/experiments/secure-boot-chain
YAML=$(realpath "${1:-$ROOT/build/payload-bm/amp-bm.yaml}")
GEN=${HSS_PAYLOAD_GENERATOR:-$ROOT/yocto-dev/build-mpfs/tmp-glibc/sysroots-components/x86_64/hss-payload-generator-native/usr/bin/hss-payload-generator}
[ -x "$GEN" ] || GEN=$(command -v hss-payload-generator) || { echo "hss-payload-generator not found"; exit 1; }
OUT=$ROOT/build/secure-boot
rm -rf "$OUT"; mkdir -p "$OUT"

echo "== 1. Throwaway keys (a real signing key must never be created or kept like this)"
openssl ecparam -name secp384r1 -genkey -noout -out "$OUT/key.ec"
openssl pkcs8 -topk8 -nocrypt -in "$OUT/key.ec" -out "$OUT/signing-private.pem"
openssl ec -in "$OUT/signing-private.pem" -pubout -out "$OUT/signing-public.pem" 2>/dev/null
openssl ec -in "$OUT/signing-private.pem" -pubout -outform DER -out "$OUT/signing-public.der" 2>/dev/null
openssl ecparam -name secp384r1 -genkey -noout -out "$OUT/other.ec"
openssl ec -in "$OUT/other.ec" -pubout -outform DER -out "$OUT/other-public.der" 2>/dev/null

echo "== 2. Build the payload unsigned and signed (the YAML is the same; -p adds the signature)"
( cd "$(dirname "$YAML")" && "$GEN" -c "$YAML" "$OUT/unsigned.bin" >/dev/null && "$GEN" -c "$YAML" "$OUT/signed.bin" -p "$OUT/signing-private.pem" >/dev/null )
ls -l "$OUT/unsigned.bin" "$OUT/signed.bin" | awk '{print "   " $5 " bytes  " $9}' | sed "s|$ROOT/||"

verify_both() {   # $1 image, $2 public key (DER), $3 label
    printf '   %-34s vendor tool: ' "$3"
    { "$GEN" -d "$1" -u "$2" 2>&1 || true; } | grep -o 'verifying signature... [a-z]*' | sed 's/verifying signature... //' | tr '\n' ' '
    printf '| independent verifier: '
    { python3 "$HERE/verify_boot_image.py" "$1" "$2" 2>&1 || true; } | grep -o 'NOT VALID\|VALID\|cannot parse.*' | head -n 1
}

echo "== 3. Verify with the vendor tool and with the independent Python verifier"
verify_both "$OUT/signed.bin" "$OUT/signing-public.der" "signed, right key"
python3 - "$OUT" <<'P'
import sys
d = bytearray(open(sys.argv[1] + '/signed.bin', 'rb').read())
d[len(d) // 2] ^= 1
open(sys.argv[1] + '/tampered.bin', 'wb').write(d)
P
verify_both "$OUT/tampered.bin" "$OUT/signing-public.der" "signed, one bit flipped"
verify_both "$OUT/unsigned.bin" "$OUT/signing-public.der" "never signed"
verify_both "$OUT/signed.bin" "$OUT/other-public.der" "signed, someone else's key"
echo "== done: the keys and images are in build/secure-boot and can be deleted"
