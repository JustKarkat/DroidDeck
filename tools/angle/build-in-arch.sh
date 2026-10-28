#!/bin/bash
# Build ANGLE (GL/GLES backend) for the CEF GPU process on the Shield TV.
#
# Valve's steamwebhelper ANGLE (steamrtarm64/libGLESv2.so) ships only the
# vulkan/swiftshader implementations — the OpenGL backend is compiled out
# (implementation-name table in the binary: d3d11on12/default/none/
# swiftshader/vulkan). Without it, --use-angle=gles-egl silently falls back
# to SwiftShader even though the shim + Tegra driver path works.
#
# chromium/6478 has no CMakeLists — the standalone path is GN. The checked-in
# buildtools/ are x64-only and this runner is aarch64, so we build gn itself
# from source first, then use system clang (no sysroot, no hermetic toolchain).
set -euxo pipefail

P=(git cmake ninja python3 clang lld zlib libx11 libxcb libxext)
pacman-key --init >/dev/null 2>&1 || true
pacman-key --populate archlinuxarm >/dev/null 2>&1 || true
pacman -Sy archlinuxarm-keyring --noconfirm >/dev/null 2>&1 || true
pacman -Syu --disable-sandbox --noconfirm --needed "${P[@]}" \
  || pacman -S --disable-sandbox --noconfirm --needed "${P[@]}" \
  || pacman -S --noconfirm --needed "${P[@]}"

cd /work
rm -rf angle-src angle-out gn-src
git clone --quiet --depth 1 --branch chromium/6478 https://github.com/google/angle.git angle-src
# build/ and buildtools/ are submodules (Chromium's build root) — empty in a
# plain clone; gn gen cannot even parse .gn without them. Only fetch those two:
# the rest include chrome-internal remotes that always fail. tools/clang would
# come from a gclient sync we deliberately skip (system clang instead).
git -C angle-src submodule update --init --depth 1 build buildtools || true
# gclient would generate this; a standalone build just needs the flags from
# ANGLE's DEPS gclient_gn_args list.
cat > angle-src/build/config/gclient_args.gni <<'GNARGS'
# Generated-by-gclient replacement for a standalone build.
checkout_angle_internal = false
checkout_angle_mesa = false
checkout_angle_restricted_traces = false
generate_location_tags = false
build_with_chromium = false
GNARGS
git clone --quiet --depth 1 https://gn.googlesource.com/gn gn-src

echo "== build gn from source"
cd /work/gn-src
# CXX set -> gen.py skips its cipd clang download and uses the system toolchain
export CC=clang CXX=clang++ AR=llvm-ar
python3 build/gen.py --no-last-commit-position
ninja -C out gn
export PATH=/work/gn-src/out:$PATH
gn --version

echo "== configure ANGLE"
cd /work/angle-src
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

echo "== build"
ninja -C out/Default EGL GLESv2

mkdir -p /work/angle-out
for f in out/Default/libEGL.so* out/Default/libGLESv2.so*; do
  [ -f "$f" ] && cp -L "$f" /work/angle-out/
done
cd /work/angle-out
for n in libEGL.so libGLESv2.so; do
  [ -f "$n" ] || { real=$(ls ${n}.* 2>/dev/null | head -1); [ -n "$real" ] && cp -L "$real" "$n"; }
done
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
