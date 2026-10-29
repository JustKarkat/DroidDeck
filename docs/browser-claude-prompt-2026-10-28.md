# Prompt for browser Claude — ShieldDeck GPU-process review, 2026-10-28

Copy everything below the line into Claude (web), which has access to the
GitHub repo `JustKarkat/DroidDeck` (branch `shield-tv`) but NOT to the
Shield TV device.

---

You are reviewing work on **ShieldDeck**, a fork/extension of DroidDeck
(a Steam-on-Android-TV launcher) that runs Steam's full Linux client in a
proot glibc guest on an NVIDIA SHIELD Android TV (Tegra X1+, aarch64). The
goal of the current branch is to make Steam's CEF UI (steamwebhelper) render
on the Tegra GPU through this chain:

    Valve's libcef (ANGLE fork, compiled in)
      → ANGLE's EGL display backend (DisplayEGL)
        → dlopen("libddcef.so") = our window-surface shim (`libddcefgl`)
          → forwards to libhybris EGL
            → the bionic Tegra vendor driver

`--use-angle=gles-egl` is forced via argv, and a binary patch
(`droiddeck-cef-patch`) rewrites Valve's backend string
(`swiftshader-webgl` → `gles-egl`) AND the display-type constant
(`mov w10,#14` SwiftShader → `mov w10,#16` ANGLE_OPENGLES_EGL) at libcef
vaddr 0x7872bd0, so Valve's fork should select the real EGL-GLES display
instead of SwiftShader.

**Please start by reading, in this order:**

1. `docs/handoff-shield-tv-2026-10-28.md` — the full investigation log.
   §6 (Valve fork selection analysis), §7 (DT14→DT16 reversal) and
   §8–12 (the newest evidence, from last session) are the important parts.
2. `tools/linuxfs/overlay/usr/local/bin/droiddeck-cef-patch` — the binary
   patcher (string patch + DT16 patch).
3. `tools/hybris/eglshim/egl_shim.c` — the shim (v4: entry logging before
   `load_real`, `DDEGL_DLOPEN_LOCAL=1` RTLD_LOCAL A/B switch, embedded
   crash logger in `crashlog_embed.c`).
4. `docs/evidence/ddmaps-15887-gpu-child-crash.txt` — `/proc/self/maps`
   snapshot of a crashed CEF GPU child, taken ~20 ms before death.
5. `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` — the session
   script (BL_TEGRA branch wires the CEF flags / env).

**The current blocker, precisely:** every CEF GPU child (argv
`--type=gpu-process --use-angle=gles-egl`) exits `exit_code=11` six times,
then a survivor falls back to `--use-gl=disabled`. Newest evidence
(handoff §8–10):

- An instrumented shim proves **no shim EGL entry point is ever called** by
  any child (88 children armed the fault logger, zero entry lines) — the
  death happens before ANGLE's DisplayEGL dlopens `libddcef.so`.
- The dying child's maps show the **DEFAULT display path fully loaded**:
  Mesa `libgallium`, two LLVMs, `libGLX_mesa`, glvnd
  `libGLdispatch/libGLX/libGL`, `libvulkan_lvp` (lavapipe) — plus OUR ANGLE
  front mapped, Valve's runtime ANGLE mapped, and NOTHING from
  `hybris-cef`. No SwiftShader libs at all.
- `/tmp/crashlog.txt` never appears despite armed signal/exit interposers —
  the death bypasses libc `exit`/`abort` and any caught fatal signal
  (raw-syscall exit_group, or a synchronous SIGSEGV on a thread with the
  signal blocked).
- The DT16 patch's application in that session is **unverified** (the
  in-guest verification timed out under Steam load).

**What I want from you (suggestions and fixes, in priority order):**

1. **Code review of `droiddeck-cef-patch`**: correctness of the byte
   patterns and offsets, idempotency edge cases, and whether anything in
   its ordering could let a session run with the string patch applied but
   the DT16 patch missed (or vice versa). Propose a concrete verification
   step (e.g. a checksum command the session script can log).
2. **Hypotheses for why Valve's ANGLE still lands on the DEFAULT display
   type** even after `mov w10,#16`. Read the handoff §6/§7 reversal notes
   (ADRP/ADD xref chain around libcef+0x20dec0e, dispatch table at
   0x20dead8, the DT16 case at 0x77dcc28). What upstream gates in
   Chromium 126's `gl_display_initializer.cc::GetEGLInitDisplays` (e.g.
   `supports_angle_opengl_egl` from the client-extension string, or
   `--use-angle` switch parsing in Valve's fork) could zero out the init
   list despite the switch constant being patched? Concrete, checkable
   hypotheses ranked by likelihood, each with a way to test it WITHOUT the
   device (static reasoning over the described disassembly, or patches we
   could pre-build).
3. **Proposed patches**: if you can identify a plausible upstream gate,
   write the concrete `droiddeck-cef-patch` addition (same-length byte
   patch style) or an alternative angle (e.g. an env var the fork might
   honor, an ANGLE feature override, a wrapper approach that avoids the
   stability regressions documented in §6 bullet 6).
4. **The exit_code=11-with-no-crashlog mystery**: given the interposers in
   `crashlog_embed.c`, what plausible death paths remain (raw
   `syscall(SYS_exit_group, 11)`, blocked-signal delivery, seccomp,
   SIGKILL misreported)? Suggest the cheapest addition to the shim that
   would close this (we can rebuild and redeploy locally).
5. Sanity-check the **architecture** itself: given everything in the
   handoff, is ANGLE-on-EGL → shim → hybris the right hill, or is there a
   materially cheaper path to Tegra-rendered CEF we've overlooked?

**Constraints:** you cannot access the device. The binary libcef.so is not
in the repo (it ships with Steam); work from the disassembly excerpts and
offsets documented in the handoff. CI workflows (`build-hybris`,
`Build ANGLE (GL backend)`) exist in the repo and are runnable, and the
shim can be rebuilt locally — so concrete patch/code suggestions are
directly actionable. Be specific and cite file paths; state uncertainty
explicitly.
