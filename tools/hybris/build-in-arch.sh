#!/bin/bash
# Build libhybris + a glibc EGL probe for the Shield TV.
#
# The guest is glibc and the vendor GPU driver is bionic; libhybris is the
# bridge. This only proves the guest can initialise the vendor EGL driver
# (eglprobe should print "GL_RENDERER=NVIDIA Tegra"); wiring it into gamescope
# (zink) is a separate step.
#
# Runs inside menci/archlinuxarm:base-devel on an arm64 runner (see
# .github/workflows/build-hybris.yml). Native aarch64 build, no cross toolchain.
set -euo pipefail

cd /work

grep -q '^DisableSandbox' /etc/pacman.conf || sed -i 's/^\[options\]/[options]\nDisableSandbox/' /etc/pacman.conf
{ for m in https://ca.us.mirror.archlinuxarm.org https://fl.us.mirror.archlinuxarm.org https://de3.mirror.archlinuxarm.org https://nl.mirror.archlinuxarm.org; do echo "Server = $m/\$arch/\$repo"; done; cat /etc/pacman.d/mirrorlist; } > /etc/pacman.d/mirrorlist.new
mv /etc/pacman.d/mirrorlist.new /etc/pacman.d/mirrorlist

pacman -Syu --noconfirm --needed base-devel git autoconf automake libtool pkg-config zstd tar libglvnd curl e2fsprogs vulkan-headers >/dev/null

WORK=/work/build-hybris
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"
mkdir -p "$WORK/root/usr/bin"

echo "== android headers (Halium 11.0)"
git clone --quiet --depth 1 --branch halium-11.0 https://github.com/Halium/android-headers.git headers
make -C headers install PREFIX="$WORK/prefix"

echo "== libhybris"
git clone --quiet https://github.com/libhybris/libhybris.git src
cd src/hybris
# The 2022 vulkan wrapper references VK_NV_cuda_kernel_launch functions that
# current Vulkan headers no longer declare; the driver entrypoints it forwarded
# there are irrelevant for a Tegra.
sed -i '/VULKAN_IDLOAD(vk[A-Za-z]*Cuda[A-Za-z]*)/d' vulkan/vulkan.c
./autogen.sh \
  --prefix=/usr \
  --with-android-headers="$WORK/prefix/include/android" \
  --enable-arch=arm64 \
  --with-default-egl-platform=null

# The bundled tests are irrelevant and one of them forgot <string.h>; stub the
# generated tests Makefile so make skips compiling them.
cat > tests/Makefile <<'EOF'
all install install-exec install-data clean distclean uninstall:
	@true
EOF
make -j"$(nproc)"
make install DESTDIR="$WORK/root"

echo "== glibc EGL probe"
cd "$WORK"
mkdir -p "$WORK/root/usr/bin"
gcc -O1 -o "$WORK/root/usr/bin/eglprobe" /work/tools/hybris/eglprobe.c \
  -I"$WORK/prefix/include/android" -I"$WORK/root/usr/include" \
  -L"$WORK/root/usr/lib" -Wl,-rpath,/usr/lib -Wl,-rpath-link,"$WORK/root/usr/lib" -lEGL -lGLESv2 -lhybris-common -lhardware

echo "== glibc Vulkan probe"
# Requires vulkan/vulkan.h in the container (vulkan-headers): without it libhybris
# skips its whole vulkan subdir (wrapper libvulkan.so + vulkanplatform_null.so),
# which is what lets a glibc app drive the bionic Android loader + tegra ICD.
gcc -O1 -o "$WORK/root/usr/bin/vkprobe" /work/tools/hybris/vkprobe.c \
  -I"$WORK/prefix/include/android" -I"$WORK/root/usr/include" \
  -L"$WORK/root/usr/lib" -Wl,-rpath,/usr/lib -Wl,-rpath-link,"$WORK/root/usr/lib" -lhybris-common
# NB: deliberately NOT -lvulkan — linking the wrapper creates IFUNC dispatch
# entries whose resolvers run in ld.so's relocation phase, before main, and
# that dies under proot. vkprobe dlopens libvulkan.so.1 itself.
ls "$WORK/root/usr/lib/libvulkan"* "$WORK/root/usr/lib/libhybris/vulkanplatform_null.so" \
  || { echo "ERROR: hybris vulkan wrapper not built (vulkan headers missing?)" >&2; exit 1; }

echo "== sysvk (Android Vulkan ICD: hw_get_module -> HAL device -> GetInstanceProcAddr)"
# xMeM/sysvk turns the hwvulkan HAL into a standard Vulkan ICD, so the guest's
# Mesa loader drives the Android driver directly — no Android loader, none of
# its discovery/namespace quirks. hw_get_module and access() are already
# verified on the device (vkprobe).
git clone --quiet https://github.com/xMeM/sysvk.git sysvk
cmake -S sysvk -B sysvk/build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$WORK/root/usr" \
  -DCMAKE_SHARED_LINKER_FLAGS="-L$WORK/root/usr/lib -Wl,-rpath,/usr/local/hybris/usr/lib"
cmake --build sysvk/build -j"$(nproc)"
cmake --install sysvk/build
# Point the ICD json at the in-guest absolute path (relative paths resolve
# against the json's own directory).
sed -i 's|"library_path": "libsysvk.so"|"library_path": "/usr/local/hybris/usr/lib/libsysvk.so"|' \
  "$WORK/root/usr/share/vulkan/icd.d/sysvk_icd.json"
file "$WORK/root/usr/lib/libsysvk.so" | sed 's/^/  /'

echo "== hybris-hal bionic (Droidian android-system-gsi-30, Halium-11 patched)"
# Stock bionic violates libhybris' TLS contract: since Android 10 it reads TLS
# inline (mrs tpidr_el0) and no longer exports __get_tls_hooks, which libhybris
# interposes. Halium patches bionic for exactly this (hybris-patches halium-11.0
# bionic/0004 __get_tls, 0007 emulated TLS). Instead of building AOSP 11 from
# source, lift the patched bionic set from Droidian's Android 11 GSI system
# image (android-system-gsi-30, built by UBports: 11.0.0+r46 ubports.286), the
# same production artifact Droidian/UBports run against stock Android 11
# vendor partitions. Put this dir FIRST in HYBRIS_LD_LIBRARY_PATH.
GSI_DEB=android-system-gsi-30_11.0.0+r46.20230226.ubports.286+git20230329184623.6015077.bookworm.production_arm64.deb
GSI_URL="https://production.repo.droidian.org/pool/main/a/android-system-gsi-30-bin/${GSI_DEB//+/%2B}"
GSI_SHA=8f3cb4b02843c23b96ff0c8c62e0727776f30332811b1bc7aed06ff043f4413f
# In the image, /system/lib64/libc.so is a symlink into the (inactive) runtime
# APEX; the real patched bionic lives in /system/apex/com.android.runtime,
# except ld-android.so and libc++.so which are real files in /system/lib64.
BIONIC_APEX=/system/apex/com.android.runtime/lib64/bionic
declare -A GSI_LIBS=(
  [libc.so]="$BIONIC_APEX/libc.so"
  [libdl.so]="$BIONIC_APEX/libdl.so"
  [libdl_android.so]="$BIONIC_APEX/libdl_android.so"
  [libm.so]="$BIONIC_APEX/libm.so"
  [ld-android.so]="/system/lib64/ld-android.so"
  [libc++.so]="/system/lib64/libc++.so"
)

mkdir -p "$WORK/gsi" "$WORK/root/usr/local/hybris/bionic"
cd "$WORK/gsi"
curl -fsSL -o "$GSI_DEB" "$GSI_URL"
echo "$GSI_SHA  $GSI_DEB" | sha256sum -c -
ar x "$GSI_DEB" data.tar.xz
tar -xf data.tar.xz ./var/lib/lxc/android/android-rootfs.img
IMG=var/lib/lxc/android/android-rootfs.img
for lib in "${!GSI_LIBS[@]}"; do
  debugfs -R "dump ${GSI_LIBS[$lib]} $WORK/root/usr/local/hybris/bionic/$lib" "$IMG" >/dev/null 2>&1
  [ -s "$WORK/root/usr/local/hybris/bionic/$lib" ] || { echo "ERROR: could not extract $lib from GSI image" >&2; exit 1; }
done
# The whole point: the GSI libc must export __get_tls_hooks (Halium patch
# 0004). Fail the build if it doesn't. (No grep -q: under pipefail its early
# exit SIGPIPEs readelf and the match reads as a failure.)
if ! readelf -sW "$WORK/root/usr/local/hybris/bionic/libc.so" | grep '__get_tls_hooks@@LIBC' >/dev/null; then
  echo "ERROR: GSI libc.so does not export __get_tls_hooks (wrong/unpatched artifact?)" >&2
  exit 1
fi
file "$WORK/root/usr/local/hybris/bionic/"*.so | sed 's/^/  /'
du -sh "$WORK/root/usr/local/hybris/bionic"

echo "== package"
cd "$WORK/root"
tar --zstd -cf /work/hybris.tzst .
cd /work
sha256sum hybris.tzst > hybris.tzst.sha256
ls -l hybris.tzst
