#!/bin/bash
# Assembles build/package/vip_regen_hp: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/vip_regen_hp"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/vip_regen_hp/libvip_regen_hp.so" "$OUT/bin/"
cp "$ROOT/build/plugins/vip_regen_hp/vip_regen_hp.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
