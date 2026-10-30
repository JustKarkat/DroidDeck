#!/usr/bin/env python3
"""Force ANGLE's GL backend to DisplayEGL on Linux (chromium/6478 Display.cpp).

Why: Chromium's request arrives as DEFAULT (an empty attrib map from
eglGetDisplay), which CreateDisplayFromAttribs fills in as
OPENGL + HARDWARE + X11_EXT; that combination goes to CreateGLXDisplay, i.e.
Mesa/llvmpipe. Only DEVICE_TYPE_EGL reaches DisplayEGL, so even a correct
ANGLE_OPENGLES_EGL request without that device type lands on GLX. On this
device there is no usable GLX path to the Tegra driver — DisplayEGL is the
only one that dlopens our shim — so force it for both GL cases and make
Valve's display-type selection irrelevant.

DDANGLE_NO_FORCE_EGL=1 restores stock selection (A/B without a rebuild).
DDANGLE_LOG=1 traces what was requested and whether an impl was built.
"""
import sys

p = sys.argv[1]
s = open(p).read()
if "DDANGLE_NO_FORCE_EGL" in s:
    print("force-egl-display: already patched")
    sys.exit(0)


def sub(old, new, count):
    global s
    found = s.count(old)
    assert found == count, f"anchor count {found} != {count}: {old[:60]!r}"
    s = s.replace(old, new)


sub("#include <algorithm>\n", "#include <algorithm>\n#include <cstdio>\n", 1)

# OPENGL and OPENGLES cases: DisplayEGL first, escape hatch via env var.
sub(
    "#    elif defined(ANGLE_PLATFORM_LINUX)\n"
    "#        if defined(ANGLE_USE_GBM)\n"
    "            if (platformType == 0)\n",
    "#    elif defined(ANGLE_PLATFORM_LINUX)\n"
    '            if (angle::GetEnvironmentVar("DDANGLE_NO_FORCE_EGL").empty())\n'
    "            {\n"
    "                impl = new rx::DisplayEGL(state);\n"
    "                break;\n"
    "            }\n"
    "#        if defined(ANGLE_USE_GBM)\n"
    "            if (platformType == 0)\n",
    2,
)

# Optional logging (DDANGLE_LOG=1): what was requested, what was built.
sub(
    "    ASSERT(displayType != EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE);\n"
    "    rx::DisplayImpl *impl = nullptr;\n",
    "    ASSERT(displayType != EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE);\n"
    "    rx::DisplayImpl *impl = nullptr;\n"
    '    if (!angle::GetEnvironmentVar("DDANGLE_LOG").empty())\n'
    '        fprintf(stderr, "[ddangle] CreateDisplay type=0x%llx device=0x%llx platform=0x%llx\\n",\n'
    "                (long long)displayType, (long long)deviceType, (long long)platformType);\n",
    1,
)
sub(
    "    return impl;\n}\n\nvoid Display_logError",
    '    if (!angle::GetEnvironmentVar("DDANGLE_LOG").empty())\n'
    '        fprintf(stderr, "[ddangle] CreateDisplay -> impl=%p\\n", (void *)impl);\n'
    "    return impl;\n}\n\nvoid Display_logError",
    1,
)

open(p, "w").write(s)
print("force-egl-display: patched", p)
