#!/bin/bash
# Stage the CI angle-libs artifact into the APK assets so sessions get
# /usr/local/hybris/usr/lib/angle (our GL-backend libEGL/libGLESv2).
#
#   gh run download <run-id> -R JustKarkat/DroidDeck -n angle-libs -D /tmp/angle
#   cd /tmp/angle && sha256sum -c angle-libs.tzst.sha256 && tar --zstd -xf angle-libs.tzst
#   tools/angle/stage-libs.sh /tmp/angle
#   python3 tools/release/runtime_inputs.py record && ./gradlew :app:assembleRelease
#
# NOTE: tools/hybris/stage-payload.sh rm -rf's usr/local/hybris, which wipes
# this directory. Re-run this script after staging a hybris payload.
set -euo pipefail

src=${1:?usage: stage-libs.sh <dir with libEGL.so and libGLESv2.so>}
assets=$(cd "$(dirname "$0")/../.." && pwd)/app/src/main/assets/linuxfs
target=$assets/usr/local/hybris/usr/lib/angle

for n in libEGL.so libGLESv2.so; do
    [ -f "$src/$n" ] || { echo "missing $src/$n" >&2; exit 1; }
done

# The unique shim loader name is the whole point of the v2 build: a generic
# dlopen("libEGL.so.1") dedups against the Mesa libEGL Chromium maps early.
if [ "$(strings -a "$src/libGLESv2.so" | grep -c 'libddcef\.so')" -lt 1 ]; then
    echo "libGLESv2.so lacks the libddcef.so loader string — wrong artifact?" >&2
    exit 1
fi

mkdir -p "$target"
cp -f "$src/libEGL.so" "$src/libGLESv2.so" "$target/"
chmod 755 "$target"/libEGL.so "$target"/libGLESv2.so
echo "staged ANGLE -> ${target#"$assets"/} ($(du -sh "$target" | cut -f1))"
