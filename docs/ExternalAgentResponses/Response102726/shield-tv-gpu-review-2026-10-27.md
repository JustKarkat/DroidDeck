# ShieldDeck GPU review — CEF on the Tegra X1 (2026-10-27)

Reviewed: `JustKarkat/DroidDeck` @ `shield-tv`, `docs/blockers-shield-tv-gpu-2026-10-27.md`, `tools/hybris/{eglprobe.c,egl_warmup.h,build-in-arch.sh,sysvk/sysvk.c}`, `tools/gamescope/patches/0127-tegra-vk11-no-timeline.patch`, `tools/linuxfs/overlay/usr/local/bin/droiddeck-session`.

**Evidence labels used below**
- **[V]** read this session in upstream source (cited by path and snapshot).
- **[M]** from memory of upstream; not re-read this session. Check it before building on it.
- **[R]** read in your repo.

Caveat on [V] for ANGLE: the `DisplayEGL.cpp` I could read is the copy vendored by Flutter (generated Nov 2023, about Chromium M120). CEF 126 is ANGLE branch `chromium/6478`. The structure has been stable for years, but re-check the exact lines against that branch.

---

## 0. Executive summary

1. **`gl-egl` is almost certainly the wrong switch for a GLES-only driver. `gles-egl` is the right one.** [V]
   - Chromium defines three native-EGL ANGLE names side by side: `gl-egl`, `gles`, and `gles-egl` (`ui/gl/gl_switches.cc`).
   - ANGLE's `DisplayEGL` picks the renderable-type filter from the platform type. `OPENGL_ANGLE` gives `EGL_OPENGL_BIT`. `OPENGLES_ANGLE` gives `EGL_OPENGL_ES3_BIT|EGL_OPENGL_ES2_BIT` (`src/libANGLE/renderer/gl/egl/DisplayEGL.cpp`, `RenderableTypesFromPlatformAttrib`).
   - Your patch turned `swiftshader-webgl` into `gl-egl`, which asks the Tegra EGL for a desktop-GL config. If the Tegra has none, ANGLE fails with `"No available renderable types"` or `"eglChooseConfig failed"`.
   - That failure is a plausible reason for the 40% GPU-process CPU you saw. A failed hardware init makes Chromium fall back to a software GL mode, which is SwiftShader again. [M]
   - Replace the string with `gles-egl`. It is 8 bytes and fits in the 17-byte slot.
2. **ANGLE's GL backend is not desktop-only.** [M]
   - It is one `RendererGL` with several front ends. GLX, EGL and others all feed the same renderer.
   - It detects ES drivers by the `"OpenGL ES"` prefix of `GL_VERSION` (`src/libANGLE/renderer/gl/FunctionsGL.cpp`).
   - The `gl-egl` and `gles-egl` front end is `DisplayEGL`. It `dlopen("libEGL.so.1")` and never touches libGL or GLX. [V]
3. **The libcef strings do not prove what the blocker doc concludes from them.**
   - The `EGL_ANGLE_platform_angle_*` names are looked up by Chromium's own `ui/gl` code, so they appear in libcef whether or not ANGLE is linked in. [M]
   - Stock CEF Linux distributions ship ANGLE as separate files (`libEGL.so`, `libGLESv2.so`, `libvk_swiftshader.so`, `libvulkan.so.1`) next to `libcef.so`. [M]
   - If Valve's `steamrtarm64/` has them, "ANGLE is statically linked, so only the system libEGL is replaceable" is wrong. Then ANGLE itself is replaceable.
   - Check with one `ls`; see §1.5.
4. **There is a second blocker after context creation: presentation. I could not verify this one, but I think it is the real one.**
   - Chromium on ozone-x11 presents through an EGL window surface created on an X11 window (see §1.4).
   - Your libhybris is built with `--with-default-egl-platform=null` and no Wayland/X11 EGL platform [R: `build-in-arch.sh`].
   - A null-platform `eglCreateWindowSurface(<X11 XID>)` is expected to fail with `EGL_BAD_NATIVE_WINDOW`.
   - Fix: a small `libEGL.so.1` shim that renders to a Tegra pbuffer and blits to the X window on `eglSwapBuffers`. This is multi-day, not multi-week (§1.6).
5. **Observability is cheaper than the doc assumes.**
   - Three zero-logging checks (the GPU process's `/proc/<pid>/maps`, its `/dev/nv*` fds, and CDP `SystemInfo.getInfo`) answer "which renderer did ANGLE bind?" in minutes. See §3.
   - Breakpad is already enabled via `/tmp/dumps` [R]. Minidumps give module+offset for the GPU-process crash with no new handler.
6. **Landmines found that you have not hit yet** (§5). The most important:
   - 0127 removed the accidental "Vulkan 1.2 or later" device guard. gamescope may now select the renderless Tegra if the ICD is visible.
   - `sk_realloc` over-reads.
   - The queue-drain completion path may race the submit thread.

**Verdict on B1.** Stock CEF 126 can reach the Tegra GLES driver through **`gles-egl`, not `gl-egl`**, as far as context creation is concerned. End to end it also needs a window-surface bridge, because hybris-null cannot present into an X11 window.

---

## 1. B1 — what ANGLE's native-EGL path actually does

### 1.1 Which front end each switch selects

| `--use-angle=` | Chromium name constant | ANGLE display class | Libraries dlopen'ed | Renderable bit asked of the driver |
|---|---|---|---|---|
| `gl` | `kANGLEImplementationOpenGLName` | `DisplayGLX` (X11, no EGL device type) | `libGL.so.1` via `FunctionsGLX` [M] | n/a (GLX) |
| `gl-egl` | `kANGLEImplementationOpenGLEGLName` [V] | `DisplayEGL` | `libEGL.so.1` | **`EGL_OPENGL_BIT`** (desktop GL) |
| `gles` | `kANGLEImplementationOpenGLESName` [V] | `DisplayEGL` or GLX depending on build [M] | — | — |
| `gles-egl` | `kANGLEImplementationOpenGLESEGLName` [V] | `DisplayEGL` | `libEGL.so.1` | **`EGL_OPENGL_ES3_BIT` + `EGL_OPENGL_ES2_BIT`** |

- Selection on Linux [V]: `src/libANGLE/Display.cpp`, `CreateDisplayFromAttribs`, Linux branch.
  - `deviceType == EGL_PLATFORM_ANGLE_DEVICE_TYPE_EGL_ANGLE` gives `rx::DisplayEGL`.
  - Otherwise `EGL_PLATFORM_X11_EXT` gives `CreateGLXDisplay`.
- Chromium's choice of platform type and device type from the switch is in `ui/gl/gl_display.cc` / `ui/gl/gl_surface_egl.cc`. [M]
  - I have not re-read the exact `kOpenGLESEGL` to `OPENGLES_ANGLE` mapping. The switch name and the ANGLE extension text (`extensions/ANGLE_platform_angle_opengl.txt` [V]: `OPENGLES_ANGLE` is "a native OpenGL ES display") strongly imply it.
  - Confirm it with the log line in §3.

### 1.2 What `DisplayEGL` loads and requests [V: `renderer/gl/egl/DisplayEGL.cpp`]

- **Library:** `getEGLPath()` returns `"libEGL.so.1"` on non-Android. `FunctionsEGLDL` `dlopen`s it and never `dlclose`s it (`FunctionsEGLDL.cpp`). The handle can be overridden with `EGL_PLATFORM_ANGLE_EGL_HANDLE_ANGLE`.
- **Entry points:** EGL via `eglGetProcAddress` from that handle. GL/GLES entry points also come through `eglGetProcAddress` (`FunctionsGLEGL`). [M]
- **Gate 1:** `EGL >= 1.4`, else `"EGL >= 1.4 is required"`.
- **Config selection:** only if `EGL_KHR_no_config_context` is absent.
  - It builds an RGBA8888 + D24S8 query with `EGL_RENDERABLE_TYPE` from the table above.
  - `EGL_SURFACE_TYPE` is `WINDOW|PBUFFER` first, then `EGL_DONT_CARE`.
  - If nothing matches, ANGLE fails with `"eglChooseConfig failed with …"`. If `renderableTypes` is empty it fails with `"No available renderable types."`.
- **Mock pbuffer:** created only if `EGL_KHR_surfaceless_context` is absent.
- **Context creation (`initializeContext`):**
  - If EGL ≥ 1.5 or `EGL_KHR_create_context` is present, it tries ES **3.2, 3.1, 3.0, 2.0** in that order using `EGL_CONTEXT_MAJOR_VERSION` / `EGL_CONTEXT_MINOR_VERSION`. Otherwise it uses `EGL_CONTEXT_CLIENT_VERSION=2`.
  - If `EGL_EXT_create_context_robustness` is advertised, each attempt first adds `EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY=EGL_LOSE_CONTEXT_ON_RESET`. If that fails it retries without.
  - **This listing shows no `eglBindAPI(EGL_OPENGL_API)` call.** Whether `FunctionsEGL::initialize` or `RendererEGL` binds an API is **not verified**. [M] says the default ES binding is used. The probe in §2 covers both outcomes.
- **Gate 2:** after creating a renderer, `getMaxSupportedESVersion() >= 2.0`, else `"OpenGL ES 2.0 is not supportable."`.
- **Version gating beyond that:** `src/libANGLE/renderer/gl/renderergl_utils.cpp` `GetMaxSupportedESVersion` is where ES 3.0, 3.1 and 3.2 are gated on the driver's version and extensions. [M] Native GLES 3.2 on the Tegra should pass these. The remaining risk is missing entry points (see 1.3).

### 1.3 Where a GLES 3.2 driver could still fail the gates

- **Missing ES3.x proc addresses.** ANGLE loads GL entry points through `eglGetProcAddress`. If hybris's wrapper returns NULL for ES3.1/3.2 names, ANGLE's loaders assert or ANGLE caps the ES version. The probe counts missing procs.
- **Robust-context attribute.** The Tegra might accept or reject `EGL_LOSE_CONTEXT_ON_RESET`. ANGLE handles rejection by retrying, but a driver that crashes instead of rejecting is not covered. The probe runs ANGLE's exact two-step sequence.
- **Chromium's own blocklist.** `-cef-force-gpu` presumably maps to `--ignore-gpu-blocklist`. Valve passes it already [R].

### 1.4 Presentation (the part the doc does not mention) [M, unverified]

- On ozone-x11, the GPU process creates the display EGL surface on an X11 window. The relevant code is under `ui/ozone/platform/x11/` and `ui/gl/gl_surface_egl*.cc`.
- ANGLE then forwards `eglCreateWindowSurface(native_config, <XID>)` to the native EGL (`DisplayEGL::createWindowSurface` → `WindowSurfaceEGL` [V]).
- Your hybris build uses the **null** EGL platform. I expect the null platform to reject an X11 XID. Check which `eglplatform_*.so` files the build actually installs.
- With SwiftShader, the equivalent present goes through SwiftShader's own X11 path, which is why that works today.
- The failure signature to look for: `EGL_BAD_NATIVE_WINDOW` or `"Failed to create surface"` → GPU init failure → fallback.

### 1.5 Five-minute device checks that settle most of this

Run these in the guest, in order:

```sh
# (a) Is ANGLE shipped as shared libs beside libcef?
ls -l /root/.local/share/Steam/steamrtarm64/ | grep -Ei 'libEGL|libGLES|swiftshader|libvulkan'

# (b) Does the binary the patch targets know the string we want?  (expect an exact line)
strings -a /root/.local/share/Steam/steamrtarm64/libcef.so | grep -x -E 'gles-egl|gl-egl|swiftshader-webgl|swiftshader'

# (c) Where do the dlopen strings live?  If libGLESv2.so exists, check it too.
for f in libcef.so libGLESv2.so libEGL.so; do
  p=/root/.local/share/Steam/steamrtarm64/$f; [ -f $p ] && { echo "== $f"; strings -a $p | grep -E 'libGL\.so\.1|libGLX_nvidia|DisplayEGL initialized' | head; }
done

# (d) Which driver did the live GPU process actually bind?  (no logging needed)
pid=$(pgrep -f -- '--type=gpu-process' | head -1)
echo "hardware libs mapped:"; grep -Ec 'libglcore|libEGL_tegra|nvgpu' /proc/$pid/maps
echo "swiftshader mapped:";  grep -Ec 'swiftshader' /proc/$pid/maps
echo "nv device fds:";       ls -l /proc/$pid/fd 2>/dev/null | grep -Ec '/dev/(nvmap|nvhost|nvgpu)'
```

Reading the results:
- Hardware libs mapped > 0 and nv fds > 0 mean ANGLE reached the driver.
- SwiftShader mapped and nothing else means a software fallback.
- Nothing from either family means the GPU process is not using GL at all yet.

### 1.6 If the verdict is "needs a bridge": evaluated options

**(a) `libGL.so.1` shim (gl4es style): not recommended.**
- It targets `gl` (GLX), not `gl-egl`. `DisplayGLX` needs `GLX_ARB_create_context` with a desktop version at or above ANGLE's minimum (`src/libANGLE/renderer/gl/glx/DisplayGLX.cpp`, `FunctionsGLX.cpp`). [M]
- gl4es presents GL 2.1 / ES2-era capability. ANGLE needs ES3-class features for Chromium's use (and ES3 on desktop GL needs 3.3-class).
- You would stack GLES → desktop GL → GLES, plus a GLX implementation over EGL. That is a large project with poor odds.
- Skip it.

**(b) Purpose-built ANGLE: only needed if the shared libs are missing or stripped.**
- If `libEGL.so` and `libGLESv2.so` ship beside `libcef.so` (§1.5a), Valve's own ANGLE already includes the GL backend when built with `angle_enable_gl` (Chromium's default on Linux). [M]
- A custom build helps only if that backend is compiled out. Then build branch `chromium/6478` for aarch64 glibc (`angle_enable_gl=true`, `angle_enable_vulkan=false`) and drop the two `.so` files in.
- If ANGLE really is statically linked into libcef, you cannot inject one. Skip this option.

**(c) Chromium switches in the 126 era.**
- `--use-gl=angle --use-angle=gles-egl` is the supported native-GLES path, and the one to try first.
- `--use-gl=egl` (raw EGL/GLES2, no ANGLE) may be rejected for X11 ozone on 126. It would also skip ANGLE's driver workarounds. [M] Not recommended.
- `--ozone-platform=headless` does not give you the window.

**Artifacts to build and ship (in order):**

| # | Artifact | Where | Effort |
|---|---|---|---|
| 1 | Patch value `swiftshader-webgl` → `gles-egl` (script in §3) | session script | minutes |
| 2 | `libddcefargs.so` exec-argv rewriter (preferred over patching; §3) | `/etc/ld.so.preload` or `LD_PRELOAD` on steam | ~0.5 day |
| 3 | **`libEGL.so.1` window-surface shim** (below) | first in `/usr/local/hybris-egl` | **3–6 days** |
| 4 | Only if §1.5a/§1.5c say the backend is missing: custom ANGLE `.so` pair | next to libcef | 2+ days (build time dominated) |

**Window-surface shim sketch (artifact 3).**
- Ship it as `libEGL.so.1` placed *before* the hybris symlink in `/usr/local/hybris-egl`. ANGLE `dlopen`s that name [V].
- It `dlopen`s the real wrapper by absolute path `/usr/local/hybris/usr/lib/libEGL.so.1`. Absolute-path loading avoids the soname-dedup problem; `sysvk.c` already relies on the same behaviour [R].
- It forwards everything except:
  - `eglGetDisplay` and `eglGetPlatformDisplay[EXT]`: ignore the X11 native display argument and return the default display.
  - `eglCreateWindowSurface` and `eglCreatePlatformWindowSurface[EXT]`: when the native window is an X11 XID, create a Tegra pbuffer the size of the window (`xcb_get_geometry`) and remember the XID.
  - `eglSwapBuffers`: render, then `glReadPixels` (a PBO ring avoids stalls), then `xcb_shm_put_image` into the window. Handle BGRA via `GL_BGRA_EXT` if present, otherwise a NEON swizzle.
  - `eglQuerySurface(WIDTH/HEIGHT)` and resizing on a size change.
  - Possibly `EGL_NATIVE_VISUAL_ID` handling, if Chromium checks it against the X visual.
- Cost estimate: 960×540×4 ≈ 2 MB per frame. Unified memory on the X1 should make that cheap compared with 40% of the A57 cluster. This is an estimate, not a measurement.
- Rule check: it touches libEGL/GLES only and never `libvulkan` [R: env contract], so it obeys the hard rule.

---

## 2. Device probe for `EGL_OPENGL_API` and ANGLE's exact sequence

`glprobe.c` compiled cleanly with `gcc -Wall -Wextra` against the Khronos EGL headers in a sandbox. It has not been run against hybris. It does the following:
- prints the EGL client and display extensions that ANGLE gates on;
- counts configs per renderable bit (`EGL_OPENGL_BIT` / ES2 / ES3);
- counts missing ES3.x proc addresses;
- tries desktop contexts (4.5 compat, 3.3 core, default) with `eglBindAPI(EGL_OPENGL_API)`;
- runs the exact two-step ES 3.2 sequence ANGLE `gles-egl` will use, with and without the robustness attribute;
- reads back one pixel from each context that comes up.

It calls GL only through `eglGetProcAddress`, the same way ANGLE does, so it does not link libGLESv2. It is longer than 30 lines because it covers all of that.

Build line (add next to `eglprobe` in `build-in-arch.sh`):

```sh
gcc -O1 -o "$WORK/root/usr/bin/glprobe" /work/tools/hybris/glprobe.c \
  -I"$WORK/prefix/include/android" -I"$WORK/root/usr/include" \
  -L"$WORK/root/usr/lib" -Wl,-rpath,/usr/lib -Wl,-rpath-link,"$WORK/root/usr/lib" \
  -lEGL -lhybris-common -lhardware
```

Run it through the existing `droiddeck-gpu-probe` wrapper (it synthesizes `/linkerconfig/ld.config.txt`).

```c
// glprobe.c — does the Tegra EGL (via hybris) give us desktop-GL contexts, and does
// it accept the exact context-creation sequence ANGLE's DisplayEGL uses (gles-egl)?
// Build like eglprobe.c:  gcc -O1 -o glprobe glprobe.c -lEGL -lhybris-common -lhardware
// (no -lGLESv2: every GL call goes through eglGetProcAddress, as ANGLE does).
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef EGL_CONTEXT_MAJOR_VERSION
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x1
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT 0x2
#endif
typedef const unsigned char *(*PFNGETSTRING)(unsigned);
typedef void (*PFNREAD)(int, int, int, int, unsigned, unsigned, void *);
typedef void (*PFNCLEAR)(float, float, float, float);
typedef void (*PFNCLR)(unsigned);

static void die(int s) { (void)s; dprintf(1, "FAULT (signal) — last stage above is the culprit\n"); _exit(99); }
static EGLDisplay dpy;

static int has(const char *list, const char *w) { return list && strstr(list, w) != NULL; }

// One attempt: bind api, pick config with renderable bit, create ctx with attribs, make current, print GL strings.
static void attempt(const char *tag, EGLenum api, EGLint rbit, const EGLint *ctxattr) {
    printf("--- %s\n", tag);
    if (!eglBindAPI(api)) { printf("  eglBindAPI FAILED 0x%x\n", eglGetError()); return; }
    EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, rbit,
                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1) { printf("  eglChooseConfig n=%d err=0x%x (ANGLE would stop here)\n", n, eglGetError()); return; }
    EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(dpy, cfg, pb);
    EGLContext c = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    printf("  pbuffer=%p ctx=%p err=0x%x\n", s, c, eglGetError());
    if (s == EGL_NO_SURFACE || c == EGL_NO_CONTEXT) return;
    if (!eglMakeCurrent(dpy, s, s, c)) { printf("  MakeCurrent FAILED 0x%x\n", eglGetError()); return; }
    PFNGETSTRING gs = (PFNGETSTRING)eglGetProcAddress("glGetString");
    PFNCLEAR cc = (PFNCLEAR)eglGetProcAddress("glClearColor");
    PFNCLR cl = (PFNCLR)eglGetProcAddress("glClear");
    PFNREAD rp = (PFNREAD)eglGetProcAddress("glReadPixels");
    printf("RESULT[%s] GL_VERSION=%s | GL_RENDERER=%s | GLSL=%s\n", tag, gs ? (const char *)gs(0x1F02) : "(null pfn)",
           gs ? (const char *)gs(0x1F01) : "?", gs ? (const char *)gs(0x8B8C) : "?");
    if (cc && cl && rp) { unsigned char px[4] = {0}; cc(0.2f, 0.4f, 0.8f, 1.f); cl(0x4000); rp(0, 0, 1, 1, 0x1908, 0x1401, px);
        printf("  readback RGBA=%u,%u,%u,%u (expect ~51,102,204,255)\n", px[0], px[1], px[2], px[3]); }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, c); eglDestroySurface(dpy, s);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGSEGV, die); signal(SIGBUS, die); signal(SIGABRT, die);
    const char *cext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("EGL client exts: %s\n", cext ? cext : "(none)");
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint M = 0, m = 0;
    if (!eglInitialize(dpy, &M, &m)) { printf("eglInitialize FAILED 0x%x\n", eglGetError()); return 1; }
    const char *dext = eglQueryString(dpy, EGL_EXTENSIONS);
    printf("EGL %d.%d vendor=%s client_apis=%s\n", M, m, eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_CLIENT_APIS));
    printf("ANGLE gates: KHR_create_context=%d no_config_context=%d surfaceless=%d robustness=%d platform_base(client)=%d\n",
           has(dext, "EGL_KHR_create_context"), has(dext, "EGL_KHR_no_config_context"), has(dext, "EGL_KHR_surfaceless_context"),
           has(dext, "EGL_EXT_create_context_robustness"), has(cext, "EGL_EXT_platform_base"));
    // Config census: ANGLE gl-egl filters on EGL_OPENGL_BIT, gles-egl on ES3/ES2 bits.
    EGLConfig all[512]; EGLint nc = 0, bits[4] = {0}; eglGetConfigs(dpy, all, 512, &nc);
    for (int i = 0; i < nc; i++) { EGLint r = 0; eglGetConfigAttrib(dpy, all[i], EGL_RENDERABLE_TYPE, &r);
        bits[0] += !!(r & EGL_OPENGL_BIT); bits[1] += !!(r & EGL_OPENGL_ES2_BIT); bits[2] += !!(r & EGL_OPENGL_ES3_BIT); }
    printf("configs=%d  OPENGL_BIT=%d ES2_BIT=%d ES3_BIT=%d\n", nc, bits[0], bits[1], bits[2]);
    // ES3.x proc coverage (ANGLE loads these through eglGetProcAddress).
    const char *fn[] = { "glGetStringi", "glTexStorage2D", "glBindVertexArray", "glDispatchCompute", "glBlendBarrier", "glPatchParameteri", "glFenceSync" };
    int nul = 0; for (unsigned i = 0; i < sizeof fn / sizeof *fn; i++) if (!eglGetProcAddress(fn[i])) { printf("  NULL proc: %s\n", fn[i]); nul++; }
    printf("ES3.x procs missing: %d\n", nul);

    // 1) The decisive one: desktop GL.
    EGLint d45[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE };
    EGLint d33[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLint dleg[] = { EGL_NONE };
    attempt("GL desktop 4.5 compat", EGL_OPENGL_API, EGL_OPENGL_BIT, d45);
    attempt("GL desktop 3.3 core", EGL_OPENGL_API, EGL_OPENGL_BIT, d33);
    attempt("GL desktop default (2.1-ish)", EGL_OPENGL_API, EGL_OPENGL_BIT, dleg);
    // 2) Control = what ANGLE gles-egl will do: ES 3.2 via MAJOR/MINOR, with and without the robustness attrib it tries first.
    EGLint es32r[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT, EGL_LOSE_CONTEXT_ON_RESET_EXT, EGL_NONE };
    EGLint es32[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE };
    attempt("ES 3.2 + robustness (ANGLE 1st try)", EGL_OPENGL_ES_API, EGL_OPENGL_ES3_BIT, es32r);
    attempt("ES 3.2 plain (ANGLE 2nd try)", EGL_OPENGL_ES_API, EGL_OPENGL_ES3_BIT, es32);
    return 0;
}
```

**How to read the output**

| Output | Meaning | Next step |
|---|---|---|
| `OPENGL_BIT=0` and every desktop attempt prints `eglChooseConfig n=0` | No desktop GL. The `"OpenGL"` in `EGL_CLIENT_APIS` is boilerplate. | `gl-egl` can never work. Use `gles-egl`. |
| `OPENGL_BIT>0` and a desktop context comes up with a `GL_VERSION` | Desktop GL works on this driver. | `gl-egl` may work as patched, and desktop contexts open other paths. Compare against the ES results. |
| `OPENGL_BIT>0` but `eglCreateContext` fails or faults | Config is advertised but the context is not. | Same conclusion as the first row. |
| ES 3.2 + robustness crashes (`FAULT`) but plain 3.2 works | Driver mishandles the robust attribute and ANGLE's fallback cannot catch a crash. | Hide `EGL_EXT_create_context_robustness` in the shim's extension string. |
| ES3.x procs missing > 0 | hybris proc table is incomplete. | Add a name forwarder in the shim (`android_dlsym`). |
| `KHR_create_context=0` | ANGLE falls back to ES 2.0 only. | Shim should advertise it if the driver supports the attributes. |
| `no_config_context=0` | ANGLE uses the config query path in §1.2. | Expected. |

---

## 3. Persistence and observability for the CEF patch

### 3.1 Is persistence even the problem?

- Your session script pins the client to a fake channel (`droiddeck-pinned-v8.0`) whose manifest fetch 404s, so a self-update normally cannot succeed [R]. The `rc=42` restart loop stops after 3 tries.
- A re-validation or a forced repair could still restore `libcef.so`. Treat the patch as something to re-verify on every launch.

### 3.2 Recommended: rewrite the argv, not the binary

- A tiny `LD_PRELOAD` library wraps `execve`, `execvp`, `execvpe`, `posix_spawn` and `posix_spawnp`. [M: Chromium's `base::LaunchProcess` goes through libc for the helper launch]
- If argv contains `--type=gpu-process`, it replaces `--use-angle=swiftshader-webgl` with `--use-angle=${BL_CEF_ANGLE:-gles-egl}` and appends the logging switches in §3.4.
- Advantages: no offsets, no dependence on how many times the string appears, and it survives any libcef update.
- The guest already ships `/etc/ld.so.preload` shims [R], so there is a precedent.
- Both sides of the flag are covered: the GPU process reads its switches from argv, and Valve's override evidently lives in the browser side that builds that argv (you saw it in `/proc/<pid>/cmdline` [R]).

### 3.3 Fallback: idempotent binary patcher (run from `droiddeck-session` before launching Steam)

```python
#!/usr/bin/env python3
# droiddeck-cef-patch: swap Valve's forced ANGLE backend. Safe to run on every launch.
import hashlib, os, shutil, sys
LIB = sys.argv[1]                     # .../steamrtarm64/libcef.so
OLD, NEW = b"swiftshader-webgl\0", (os.environ.get("BL_CEF_ANGLE", "gles-egl").encode() + b"\0")
assert len(NEW) <= len(OLD)
data = open(LIB, "rb").read()
n_old, n_new = data.count(OLD), data.count(NEW.ljust(len(OLD), b"\0"))
if n_old == 0:
    sys.exit(0 if n_new else 3)       # already patched -> 0; neither string present -> 3 (client changed)
if n_old > 4:
    sys.exit(4)                       # unexpected layout; do not guess
bak = LIB + ".droiddeck-bak"
if not os.path.exists(bak):
    shutil.copy2(LIB, bak)
    open(bak + ".sha256", "w").write(hashlib.sha256(data).hexdigest())
open(LIB + ".tmp", "wb").write(data.replace(OLD, NEW.ljust(len(OLD), b"\0")))
shutil.copymode(LIB, LIB + ".tmp"); os.replace(LIB + ".tmp", LIB)
print("patched", n_old, "site(s)")
```

- The caller treats exit 3 or 4 as "client layout changed". The session script should print a loud `== cef-patch: …` line and export `BL_CEF_GLES=0`, so you get the known-good software path instead of a silent wedge.
- Not tested on device. It assumes `/proc`-style read-modify-write of a ~200 MB file is acceptable at startup. A `mmap` patch of the single offset is a drop-in alternative if it is not.

### 3.4 Making GPU-process GL failures visible (Valve's CEF 126)

- Add to `BL_WEBHELPER_ARGS` (currently `--no-zygote -AudioServiceOutOfProcess` [R]):
  ```
  --enable-logging=stderr --log-severity=verbose --v=0 --vmodule=gl_*=1,gpu_init=1,gpu_info_collector=1,gpu_process_host=1,angle_platform_impl=1
  ```
  - `--vmodule` matches Chromium source file base names, so these patterns should catch `gl_display`, `gl_surface_egl`, `gl_initializer_*`, `gl_implementation`, `gpu_init` and ANGLE's platform logger. [M] If Valve's wrapper ignores `BL_WEBHELPER_ARGS` for the GPU child, the exec hook in §3.2 can append them.
  - ANGLE routes its `ERR()`/`WARN()`/`INFO()` through `angle_platform_impl.cc` into Chromium logging. [M] The messages quoted in §1.2 are what you are looking for.
  - Do not use `--enable-gpu-service-logging` at first; it is very verbose.
- **No-flag, no-log checks.** These are the best verification channel because they don't depend on Valve's wrapper:
  - The `/proc/<gpu-pid>/maps` and `/proc/<gpu-pid>/fd` greps from §1.5d.
  - **CDP.** `-cef-enable-debugging -devtools-port $BL_CDP_PORT` is already passed [R] and guest processes can reach it.
    - `SystemInfo.getInfo` returns the GPU devices and `auxAttributes` (renderer/version strings).
    - Alternatively open `chrome://gpu` as a target and read `document.body.innerText`. That page shows the chosen ANGLE display type, `GL_RENDERER`, and a "Problems Detected" list.
    - `droiddeck-steam-compat` already speaks CDP, so reuse its client rather than writing a new one.
- **Stop the silent fallback while diagnosing.** `--disable-software-rasterizer` removes the SwiftShader fallback, so a hardware init failure becomes an immediate, visible GPU process exit instead of a slow UI. Remove it afterwards. [M]
- **Crashes (B3).**
  - Breakpad is on and `/tmp/dumps` is prepared [R]. Pull the `.dmp` files and run a minidump stackwalker on the host. The dump contains the module list, so you get `module+offset` for each frame without a new in-process handler.
  - Map offsets to your `libglcore` cheat-sheet in `docs/handoff-shield-tv-2026-10-26.md`.
  - A preload `SIGSEGV` handler gets overwritten when Chromium installs its own handler. Interpose `sigaction` to chain, or skip this and use the dumps.

---

## 4. B2 — CPU contention triage

### 4.1 What to measure first (all doable without ptrace)

Measure steady state only. `compileAllPipelines` (a gamescope thread, `m_pipelineThread` in the 0127 context) JIT-compiles shaders on lavapipe at startup and will distort an early sample. [R]

1. **Run-queue wait, per thread.** CPU% hides the real cost on 4 cores with more than 4 runnable threads.
   ```sh
   snap(){ for t in /proc/[0-9]*/task/[0-9]*; do
     read c < $t/comm 2>/dev/null; read a b d < $t/schedstat 2>/dev/null; echo "$a $b $d $t $c"; done; }
   snap > /tmp/s0; sleep 10; snap > /tmp/s1   # diff cpu_ns and run_delay_ns per thread
   ```
   Large `run_delay` against CPU time means contention, not slow code. If `schedstat` is absent on this kernel, fall back to `voluntary/nonvoluntary_ctxt_switches` in `/proc/<tid>/status`.
2. **Who are the threads?** In gamescope, split the main/compositor thread, the vblank thread, the X/Wayland event thread, and the lavapipe workers. In the host app, see 4.2.
3. **Frequency and throttling.** Sample `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq` during the session. A TV box that throttles looks exactly like contention.
4. **Whether the GPU process is still software.** If the §1.5d check shows SwiftShader, then ~40% is a fallback and fixing B1 moves the largest block.

### 4.2 The 19% host-app mystery

- **Android side.** Per-thread: `adb shell top -H -p <pid>`. Sampling: `adb shell simpleperf record -p <pid> -g --duration 10`, then `simpleperf report`. System-wide scheduling: `perfetto` with `sched`, `freq` and `gfx` categories; it ships on Android 11.
- **proot tracer cost.** I could not see how proot is spawned. If the `proot` threads run inside that process, ptrace-stop handling shows up there.
  - proot only traps path-style syscalls when its seccomp fast path works. If seccomp acceleration is off, every syscall is trapped.
  - This is worth checking because Chromium and the vendor driver make many syscalls. If simpleperf shows `ptrace`/`wait4`/`waitpid` hot, the fix is in the launcher (seccomp mode), not in Steam.
- Also consider the polling candidate the doc names: `droiddeck-steam-compat` waiting on SharedJSContext. Check its poll interval.

### 4.3 Knobs that probably matter, in order

1. **B1 itself** — moving CEF raster/composite to the GPU removes the largest block.
2. **gamescope frame rate.** `-r`/`BL_FPS` to 30 halves composites if the UI does not need 60. Lavapipe cost is per composited frame.
3. **`BL_MANGOAPP=0`.** `mangoapp` is on by default in steam mode [R] and renders through the same CPU Vulkan; try without it as an A/B.
4. **`LP_NUM_THREADS`.** Currently 2 [R]. Test 1, 2, 3 and judge by gamescope's frame time, not CPU%. Lavapipe synchronises threads per draw, so oversubscribing 4 cores can cost more than it gains.
5. **Core partitioning.** `program_cores()` returns nothing on four identical A57s, so nothing is pinned. Try gamescope on 0–1 and CEF on 2–3 with `taskset` via the existing `BL_CLIENT_CPUS` mechanism [R]. Measure run-delay before and after.
6. **Background priority.** `renice +10` the Steam client main process and the `droiddeck-*` helpers. Raising priority needs privileges you don't have; lowering does not.

Skip: `--disable-gpu-process-crash-limit` (not a performance knob), and `--num-raster-threads` for the GPU process.

---

## 5. Design review: `sysvk.c` and patch 0127

Ordered by how likely each is to bite.

1. **0127 removed an accidental guard.** [R]
   - Before: `apiVersion < VK_API_VERSION_1_2` made gamescope skip the Tegra (api 1.1.178). After 0127 it accepts ≥ 1.1.
   - If `sysvk_icd.json` is visible to gamescope's Vulkan loader alongside lavapipe, gamescope can pick the renderless device. Enumeration order is not something to rely on.
   - Fix: keep the session's `VK_DRIVER_FILES` / `VK_ICD_FILENAMES` pointing only at lavapipe for gamescope, or pass an explicit device selector. Verify with the log line gamescope prints for the chosen device.
2. **`sysvk` is visible to every Vulkan loader in the guest.** [R]
   - It is installed in the standard `icd.d` directory. Steam, Chromium's probing, and Proton games all enumerate it.
   - Games that pick "the first discrete or any GPU" can select the renderless Tegra and fail confusingly.
   - Fix: install the ICD manifest outside the default search path and only reference it from probe tooling.
3. **`sk_realloc` over-reads.** [R]
   - It copies `size` bytes from `orig` when the *new* size can exceed the old one. At a page boundary this faults.
   - Fix: copy `min(size, malloc_usable_size(orig))`. `orig` comes from `posix_memalign`, so glibc's `malloc_usable_size` applies.
4. **Allocator substitution covers `vkCreateInstance` only.** [R, and the code says so]
   - `vkCreateDevice`, `vkAllocateMemory` and every other create call pass the caller's `pAllocator` (NULL from gamescope) straight through.
   - Your own comment says the driver dereferences it unconditionally for instances. If that is true per object, device creation will fault.
   - Fix if it does: wrap the create/destroy pairs you actually use, with a handle-to-instance registry. Since compositing is off the table, only do this if you still need the Tegra device for something.
5. **Queue-drain completion in 0127.** [R]
   - `completedSeqNo()` calls `vk.QueueWaitIdle` and then returns `m_submissionSeqNo`.
   - **If `QueueWaitIdle` is NULL, the code skips the wait and reports everything complete.** That is a use-after-free of upload buffers and command buffers while the GPU may still run.
   - `vkQueueWaitIdle` needs external synchronisation with `vkQueueSubmit`. If `garbageCollect()` and `wait()` run on a different thread than submit, check there is a queue mutex.
   - The 64 `m_aTimelineFences` created in `createScratchResources` are never used, and `m_emulatedCompletedSeqNo` is dead. Harmless, but a sign the design changed.
   - Every descriptor-ring wrap and every GC drains the queue. That serialises CPU and GPU. It is fine for bring-up, bad for any real use.
6. **Stub list gaps in `sk_stub_needed`.** [R]
   - Not stubbed but plausibly called unguarded: `vkImportSemaphoreFdKHR`, `vkImportFenceFdKHR`, `vkGetMemoryFdPropertiesKHR`, `vkGetPhysicalDeviceSurfaceCapabilities2KHR`, `vkGetPhysicalDeviceSurfaceFormats2KHR`, `vkAcquireNextImage2KHR`, and the synchronization2 family (`vkQueueSubmit2`, `vkCmdPipelineBarrier2`, …).
   - Use the `0127-diag` NULL-dispatch log: every NULL entry that has a call site in gamescope is a latent crash. Treat that log as the checklist.
   - `vkCmdBeginRendering` / `vkCmdEndRendering` return `void`, but `sk_stub` returns a `VkResult`. That is harmless on aarch64, because the return register is ignored.
   - Returning `VK_ERROR_FEATURE_NOT_PRESENT` from `vkQueuePresentKHR` is not a legal present result. Mesa WSI treats it as a generic error, but other consumers may not.
7. **`egl_warmup` leaves a context current on whichever thread first hits the ICD.** [R]
   - It never unbinds. If that thread is the loader's or an app thread that later calls EGL itself, you get a surprise context.
   - Also, `egl_warmup` runs inside the Vulkan loader's first ICD call while the loader holds its lock. EGL init dlopens libraries and may start threads. Today it works; it is order-sensitive.
   - Fix: after warm-up, `eglMakeCurrent(dpy, NO_SURFACE, NO_SURFACE, NO_CONTEXT)` and keep the display initialised.
8. **EGL soname collision, order-dependent.** [R: B6 in your doc]
   - `libsysvk.so` has `libEGL.so.1` in `DT_NEEDED`. In a process where glvnd's `libEGL.so.1` is already loaded, the dependency resolves to glvnd, and `egl_warmup()` warms Mesa instead of the Tegra.
   - Fix: have `sysvk` `dlopen` the hybris `libEGL.so.1` by absolute path and `dlsym` the five EGL calls it needs, rather than linking `-lEGL`.
9. **Misc.**
   - `preload_hybris_egl_chain` creates `/usr/lib/libhybris` at runtime from any process. Harmless race (`EEXIST`), but it writes to the rootfs from inside an app.
   - The `HOST_CACHED` relaxation in `findMemoryType` (0127) means CPU readbacks (screenshots, recording) would hit uncached memory. That is very slow on A57. Irrelevant while the Tegra is not used for compositing.
   - EGL platform selection: confirm the process that runs `egl_warmup` has `EGL_PLATFORM=null` (or none) and not a Wayland value inherited from gamescope's nested backend.

---

## 6. Ranked actions — (impact × probability) ÷ effort

Scores are rough 1–5 judgements. Impact is the effect on UI performance on the TV; probability is my chance the step works as described.

| Rank | Action | Impact | Prob. | Effort | Score | Notes |
|---|---|---|---|---|---|---|
| 1 | Run the §1.5 checks (`ls steamrtarm64`, `strings`, GPU-process `/proc/*/maps` and fds) | 4 | 5 | 0.1 d | **200** | Tells you whether ANGLE is shared, whether `gles-egl` exists, and whether you are on SwiftShader right now. |
| 2 | Build and run `glprobe` | 4 | 5 | 0.2 d | **100** | Settles desktop-GL and the ANGLE context sequence. |
| 3 | Switch the patched value to `gles-egl`; read the result with §1.5d and CDP `chrome://gpu` | 5 | 3 | 0.2 d | **75** | Highest-leverage one-word change. Expect it to stop at window-surface creation. |
| 4 | Add the §3.4 logging switches and keep `--disable-software-rasterizer` on while diagnosing | 3 | 4 | 0.2 d | **60** | Makes the next failure legible. |
| 5 | Cheap B2 A/B: `BL_MANGOAPP=0`, `BL_FPS=30`, `LP_NUM_THREADS` 1/2/3, with the run-delay snapshot | 3 | 3 | 0.3 d | **30** | Independent of B1. |
| 6 | Fix `sk_realloc`, isolate the sysvk ICD, pin gamescope's ICD (§5.1–5.3) | 2 | 4 | 0.3 d | **27** | Removes latent crashes. |
| 7 | `libddcefargs.so` exec-argv rewriter plus the §3.3 patcher as fallback | 3 | 4 | 0.5 d | **24** | Persistence; low urgency because the client is pinned. |
| 8 | **Window-surface `libEGL.so.1` shim** (§1.6) | 5 | 3 | 3–6 d | **~4** | Multi-day; the only route to end-to-end hardware CEF if rank 3 stops at presentation. Do it after ranks 1–3 say it is needed. |
| 9 | Custom ANGLE build | 3 | 1 | 2+ d | **~1.5** | Only if rank 1 shows the GL backend is missing. |
| 10 | gl4es / `libGL.so.1` shim | 1 | 1 | weeks | **<0.5** | Not recommended (§1.6a). |

The score formula puts the shim low because its effort is large. It is nonetheless the only item that delivers the end result, so it is where the project ends up if the early steps confirm the presentation blocker.

---

## 7. What I verified and what I did not

**Read this session ([V]):**
- `google/angle` `src/libANGLE/Display.cpp`, Linux display selection by `deviceType`/`platformType`.
- `google/angle` `src/libANGLE/renderer/gl/egl/DisplayEGL.cpp` (Flutter-vendored snapshot, Nov 2023): library path, EGL ≥ 1.4 gate, ES ≥ 2.0 gate, config and context creation, renderable-type table, mock pbuffer.
- `google/angle` `src/libANGLE/renderer/gl/egl/FunctionsEGLDL.cpp` (Cobalt snapshot): `dlopen`, never `dlclose`, handle override.
- `chromium/src` `ui/gl/gl_switches.cc`: `gl-egl`, `gles`, `gles-egl` constants.
- `extensions/ANGLE_platform_angle_opengl.txt`: `OPENGLES_ANGLE` = native ES display.
- Your repo files listed at the top.

**Not verified; check before relying on ([M]):**
- That Chromium maps `gles-egl` to `EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE` with the EGL device type.
- Whether `FunctionsEGL::initialize` or anything else calls `eglBindAPI`, and with which API.
- `FunctionsGL.cpp` ES detection by `GL_VERSION` prefix, and the exact ES-version gates in `renderergl_utils.cpp`.
- Whether Valve ships ANGLE as `libEGL.so` / `libGLESv2.so`.
- The origin of the `libGL.so.1` / `libGLX_nvidia.so.0` strings. My guess is the GPU sandbox preload (`content/gpu/gpu_sandbox_hook_linux.cc`) and the GLX front end, not `DisplayEGL`. They do not change the verdict, because `DisplayEGL` never reads them.
- Chromium's GPU-mode fallback after a failed hardware init (`gpu/ipc/service/gpu_init.cc`, `content/browser/gpu/gpu_data_manager_impl_private.cc`).
- That the X11 ozone path presents via an EGL window surface on an X11 XID, and that hybris-null rejects it.
- That Valve's helper launch goes through libc `exec*`/`posix_spawn` so the argv rewriter sees it.
- Whether the gamescope queue and submit paths share a mutex in 3.16.29.

If any of these turns out differently on the device, the ranking in §6 still holds up to rank 3. Ranks 7–9 should be re-evaluated.
