#!/bin/bash
# Build ANGLE (GL/GLES backend) for the CEF GPU process on the Shield TV.
#
# Valve's steamwebhelper ANGLE (steamrtarm64/libGLESv2.so) ships only the
# vulkan/swiftshader implementations — the OpenGL backend is compiled out
# (implementation-name table in the binary: d3d11on12/default/none/
# swiftshader/vulkan). Without it, --use-angle=gles-egl silently falls back
# to SwiftShader even though the shim + Tegra driver path works.
#
# Standalone ANGLE is a gclient project (build/, testing/, third_party/* all
# come from DEPS; the checked-in buildtools are x64-only), so: depot_tools +
# gclient sync at the chromium/6478 branch, then a GN build with system clang
# (no sysroot — the runner is aarch64 and sysroots are x64).
set -euxo pipefail

P=(git python3 clang lld ninja zlib libx11 libxcb libxext libdrm pkgconf mesa libxi)
pacman-key --init >/dev/null 2>&1 || true
pacman-key --populate archlinuxarm >/dev/null 2>&1 || true
pacman -Sy archlinuxarm-keyring --noconfirm >/dev/null 2>&1 || true
pacman -Syu --disable-sandbox --noconfirm --needed "${P[@]}" \
  || pacman -S --disable-sandbox --noconfirm --needed "${P[@]}" \
  || pacman -S --noconfirm --needed "${P[@]}"

cd /work
rm -rf depot_tools angle angle-out
git clone --quiet --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH=/work/depot_tools:$PATH

mkdir -p /work/angle
git clone --quiet --depth 1 --branch chromium/6478 https://github.com/google/angle.git /work/angle/angle

cat > /work/angle/.gclient <<'GCLIENT'
solutions = [
  {
    "name": "angle",
    "url": None,
    "managed": False,
    "custom_deps": {},
    "custom_vars": {"checkout_angle_internal": False, "checkout_angle_mesa": False,
                    "checkout_angle_restricted_traces": False},
    "safesync_url": "",
  },
]
GCLIENT
cd /work/angle
echo "== gclient sync (DEPS: build, testing, third_party)"
gclient sync --no-history --shallow 2>&1 | tail -15

cd /work/angle/angle
# standalone-build fixes: hermetic-clang headers mask missing includes
grep -q "include <stdint.h>" src/image_util/AstcDecompressor.h ||   sed -i '1i #include <stdint.h>' src/image_util/AstcDecompressor.h
echo "== gn (hermetic depot_tools gn, aarch64 host)"
gn --version || { ls /work/depot_tools | head; exit 1; }

cat > out.gn.args <<'ARGS'
is_debug = false
is_clang = true
clang_base_path = "/usr"
clang_use_chrome_plugins = false
use_sysroot = false
use_lld = true
treat_warnings_as_errors = false
use_custom_libcxx = false
angle_enable_gl = true
angle_enable_vulkan = false
angle_enable_null = true
angle_enable_d3d9 = false
angle_enable_d3d11 = false
angle_standalone = true
angle_build_tests = false
angle_build_samples = false
ARGS
gn gen out/Default --args="$(cat out.gn.args)" 2>&1 | tail -20

echo "== targets:"
ninja -C out/Default -t targets all 2>/dev/null | grep -iE "libEGL|libGLESv2" | head -5 || true
echo "== build"
ninja -C out/Default

mkdir -p /work/angle-out
for f in out/Default/libEGL.so* out/Default/libGLESv2.so*; do
  [ -f "$f" ] && cp -L "$f" /work/angle-out/
done
cd /work/angle-out
for n in libEGL.so libGLESv2.so; do
  [ -f "$n" ] || { real=$(ls ${n}.* 2>/dev/null | head -1); [ -n "$real" ] && cp -L "$real" "$n"; }
done
# Point ANGLE's GL-backend EGL loader at our window-surface shim by a unique
# name: a plain dlopen("libEGL.so.1") can dedup against an already-loaded
# Mesa libEGL (Chromium maps one early), which silently sends ANGLE to
# llvmpipe. Same-length in-place patch; libddcef.so is staged by the session
# script next to the ANGLE libs.
python3 - <<'PATCH'
data = bytearray(open('libGLESv2.so', 'rb').read())
old, new = b'libEGL.so.1\x00', b'libddcef.so\x00'
n = data.count(old)
assert n >= 1, "libEGL.so.1 string not found"
data = data.replace(old, new)
open('libGLESv2.so', 'wb').write(bytes(data))
print('patched', n, 'loader-name site(s)')
PATCH
ls -la
file libEGL.so libGLESv2.so
echo "== backend sanity (expect DisplayEGL / libEGL.so.1 strings):"
strings -a libGLESv2.so | grep -cE "DisplayEGL|libEGL\.so\.1" || echo "MISSING GL BACKEND"
echo "== sonames:"
readelf -d libEGL.so | grep -E SONAME || true
readelf -d libGLESv2.so | grep -E SONAME || true

cd /work
tar --zstd -cf angle-libs.tzst -C angle-out libEGL.so libGLESv2.so
sha256sum angle-libs.tzst > angle-libs.tzst.sha256
echo "== angle build done"
