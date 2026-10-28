#!/bin/bash
# Build ANGLE (GL/GLES backend) for the CEF GPU process on the Shield TV.
#
# Valve's steamwebhelper ANGLE (steamrtarm64/libGLESv2.so) ships only the
# vulkan/swiftshader implementations — the OpenGL backend is compiled out
# (implementation-name table in the binary: d3d11on12/default/none/
# swiftshader/vulkan). Without it, --use-angle=gles-egl silently falls back
# to SwiftShader even though the shim + Tegra driver path works.
#
# This builds a standalone ANGLE with the OpenGL(ES) backend for aarch64
# glibc; the resulting libEGL.so/libGLESv2.so replace Valve's pair in
# steamrtarm64 (session script applies them with a backup).
set -euxo pipefail

pacman-key --init >/dev/null 2>&1 || true
pacman-key --populate archlinuxarm >/dev/null 2>&1 || true
pacman -Sy archlinuxarm-keyring --noconfirm >/dev/null 2>&1 || true
P=(cmake ninja python3 zlib libx11 libxcb libxext)
pacman -Syu --disable-sandbox --noconfirm --needed "${P[@]}" \
  || pacman -S --disable-sandbox --noconfirm --needed "${P[@]}" \
  || pacman -S --noconfirm --needed "${P[@]}"

cd /work
rm -rf angle-src angle-build angle-out
git clone --quiet --depth 1 --branch chromium/6478 https://github.com/google/angle.git angle-src
cd angle-src
echo "== ANGLE at $(git rev-parse --short HEAD)"

cmake -GNinja -B /work/angle-build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DANGLE_ENABLE_OPENGL=ON \
  -DANGLE_ENABLE_VULKAN=OFF \
  -DANGLE_ENABLE_NULL=ON \
  -DANGLE_BUILD_TESTS=OFF \
  -DANGLE_ENABLE_TESTS=OFF \
  -DANGLE_BUILD_SAMPLES=OFF \
  -DANGLE_ENABLE_DEBUG_ANNOTATIONS=OFF 2>&1 | tail -25

echo "== targets containing GL:"
ninja -C /work/angle-build -t targets all 2>/dev/null | grep -iE "libEGL|libGLESv2" | head -8 || true

ninja -C /work/angle-build EGL GLESv2

mkdir -p /work/angle-out
for f in /work/angle-build/libEGL.so* /work/angle-build/libGLESv2.so*; do
  [ -f "$f" ] && cp -L "$f" /work/angle-out/
done
cd /work/angle-out
for n in libEGL.so libGLESv2.so; do
  [ -f "$n" ] || { real=$(ls ${n}.* 2>/dev/null | head -1); [ -n "$real" ] && cp -L "$real" "$n"; }
done
ls -la
file libEGL.so libGLESv2.so
echo "== backend sanity (expect DisplayEGL/DisplayGLX strings):"
strings -a libGLESv2.so | grep -cE "DisplayEGL|libEGL\.so\.1" || echo "MISSING GL BACKEND"
echo "== sonames:"
readelf -d libEGL.so | grep -E SONAME || true
readelf -d libGLESv2.so | grep -E SONAME || true

cd /work
tar --zstd -cf angle-libs.tzst -C angle-out libEGL.so libGLESv2.so
sha256sum angle-libs.tzst > angle-libs.tzst.sha256
echo "== angle build done"
