#!/bin/bash
# Stage the CI hybris-probe artifact into the APK assets so sessions get
# /usr/local/hybris (libhybris + eglprobe + the Halium-patched bionic set).
#
#   gh run download <run-id> -R JustKarkat/DroidDeck -n hybris-probe -D /tmp/hp
#   sha256sum -c /tmp/hp/hybris.tzst.sha256 && tools/hybris/stage-payload.sh /tmp/hp/hybris.tzst
#   python3 tools/release/runtime_inputs.py record && ./gradlew :app:assembleRelease
#
# APK assets cannot carry symlinks, so only real files are staged;
# droiddeck-gpu-probe recreates the soname links in the guest. SessionFiles
# stages the payload as an optional set, so builds without it stay green.
set -euo pipefail

tzst=${1:?usage: stage-payload.sh <hybris.tzst from the hybris-probe artifact>}
if [ ! -f "$tzst.sha256" ]; then
    echo "NOTE: $tzst.sha256 not found next to the artifact; skipping the checksum" >&2
else
    (cd "$(dirname "$tzst")" && sha256sum -c "$(basename "$tzst").sha256")
fi

assets=$(cd "$(dirname "$0")/../.." && pwd)/app/src/main/assets/linuxfs
target=$assets/usr/local/hybris
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

tar --zstd -xf "$tzst" -C "$work" ./usr/lib ./usr/bin ./usr/local/hybris
# bionic lives under usr/local/hybris/bionic in the artifact; flatten the wrapper
if [ -d "$work/usr/local/hybris/bionic" ]; then
    mv "$work/usr/local/hybris/bionic" "$work/bionic"
fi
rmdir "$work/usr/local/hybris" 2>/dev/null || true

rm -rf "$target"
mkdir -p "$target"
cd "$work"
find usr bin bionic -type f ! -name '*.la' ! -path '*pkgconfig*' -print0 |
    while IFS= read -r -d '' f; do
        install -Dm755 "$f" "$target/$f"
    done

# sanity: the patched bionic must be present and interposable
grep -aq '__get_tls_hooks' "$target/bionic/libc.so" ||
    { echo "staged libc.so lacks __get_tls_hooks — wrong artifact?" >&2; exit 1; }
echo "staged $(find "$target" -type f | wc -l) files ($(du -sh "$target" | cut -f1)) into ${target#"$assets"/}"
