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

pacman -Syu --noconfirm --needed base-devel git autoconf automake libtool pkg-config zstd tar libglvnd >/dev/null

WORK=/work/build-hybris
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"
mkdir -p "$WORK/root/usr/bin"

echo "== android headers (Halium 9.0)"
git clone --quiet https://github.com/Halium/android-headers.git headers
git -C headers checkout --quiet 00543b4ff3f56a9f96be336df276365aef46f8a2
make -C headers install PREFIX="$WORK/prefix"

echo "== libhybris"
git clone --quiet https://github.com/libhybris/libhybris.git src
cd src/hybris
./autogen.sh \
  --prefix="$WORK/prefix" \
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
  -L"$WORK/root/usr/lib" -Wl,-rpath,/usr/lib -lEGL -lGLESv2

echo "== package"
cd "$WORK/root"
tar --zstd -cf /work/hybris.tzst .
cd /work
sha256sum hybris.tzst > hybris.tzst.sha256
ls -l hybris.tzst
