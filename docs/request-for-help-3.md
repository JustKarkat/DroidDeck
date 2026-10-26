# Request for research #3 — Shield TV: Steam's gamepad-UI renderer dies every 50–120 s in a prooted aarch64 guest (root cause + how to get a minidump)

Written 2026-10-24 ~22:50 CDT by the working session. **Research only — do not touch the
device.** Prior notes: `docs/request-for-help.md` (#1), `docs/request-for-help-2.md` (#2) and
`docs/ExternalAgentResponses/Shield TV Phase 1 research notes on the gamepadui renderer crash loop.md`
(Phase-1 research reply, 2026-10-23). #2's preferred fix (B: gamescope wl_shm output) is already
implemented and working — this is the next wall, not a regression of #2.

Device: `192.168.0.194:5555`, `adb` works (`run-as com.droiddeck.launcher` too), Android 11
build `RQ1A.210105.003`, 3 GB RAM + 512 MB swap, no root. Guest is a glibc aarch64 Arch-derived
rootfs under **proot** (no namespaces, no systemd).

---

## 0. TL;DR of the question

Steam (native aarch64 `steamwebhelper`, CEF 126.0.6478.183) starts, its **SharedJSContext** page
stays alive, but the **main `https://steamloopback.host/` tab renderer is killed every ~50–120 s**
(`sad_tab.cc(255) Tab Killed`), so the 1280×800 X11 window on `:2` never maps, so gamescope never
gets a first frame. Everything below the Steam UI works (gamescope 0123-shm presents, the app's
compositor copies frames — glxgears E2E verified).

**What we need:** the most likely actual kill mechanism, and the smallest experiment that
distinguishes it; plus a practical way to obtain a minidump/backtrace given
`--disable-crash-reporter`, `--no-zygote`, proot and no root.

**One concrete lead we already have** (details in §3a): the kernel is 4.9.141 (no `statx(2)`), and
the project's proot *fast path* calls `statx` from its untraced trampoline page, so it returns raw
`ENOSYS` instead of falling back to proot's emulation. `stat /tmp` fails in the guest today. Could
that be what kills the renderer / the minidump writer?

---

## 1. What changed / what was ruled out this session

1. **On-screen session-log overlay restored** (pref `loadingLog` had been lost from
   `/data/data/com.droiddeck.launcher/shared_prefs/session.xml`; rewritten with the app stopped,
   verified live). Not related to the bug, but the log now renders again on the TV.
2. **§5.1 "Chromium renderer diet" experiment was run and did NOT fix it.** It was active for a
   full session (`--renderer-process-limit=1 --js-flags=--max-old-space-size=256`), UI got a bit
   further (`SteamAppInit`, config load, locale) but the tab still died on the same cadence.
3. This run (session `2026-10-24-21-steam`) uses a *lighter* override:
   `BL_WEBHELPER_ARGS=--no-zygote --in-process-gpu -AudioServiceOutOfProcess -V4L2VideoDecode`
   (i.e. default args, `--js-flags` and `--renderer-process-limit` removed, V4L2 video decode
   disabled because the guest has no `/dev/video-dec0`). **Same failure**, so the diet flags are
   not the cause (though they may add noise).
4. Not caused by the display path: glxgears under gamescope produces `copy N` / `present … (N)` in
   the app compositor; gamescope heartbeats at ~60 iter/s.
5. Not classic Android LMKD: `oom_score_adj` is `0` for gamescope/steam/steamwebhelper, and logcat
   has no `lowmemorykiller: Kill` lines for those pids (only "Failed to open /proc/PID/oom_score_adj"
   noise for already-dead Android apps). Swap is nonetheless **511/511 MB used** and MemFree is
   ~30 MB, so memory pressure is real even if LMKD isn't the reaper.
6. `strace` exists in the guest, but attaching it from inside the proot session produced no output
   file — expected, proot already ptraces its children (nested ptrace is not available). We have no
   way to see the child exit status/signal that way.

---

## 2. Current state (session `2026-10-24-21-steam`)

- `droiddeckctl state`: session running, phase `PREPARING/STARTING_STEAM`, `steamUi: null`,
  `firstFrame: false`.
- `droiddeckctl focus` (Xwayland servers `:2`, `:3`):
  - `:2` has two windows, `class: Steamwebhelper`, geometry `0,0,1280,800`, **`map: "unmapped"`**,
    pid 2861.
  - `:3` empty. `focusedApp: null`.
- CDP target list sampled every 20–30 s (port 35945 this run):

```
22:45:53   "Steam" (http://error/)      + "SharedJSContext" (…/routes/oobe/1/language)
22:46:22   "Steam" (http://error/)      + "SharedJSContext"
22:46:51   "Steam" (url "")             + "SharedJSContext"
22:47:22   (empty / mid-restart)
```

  `cdp eval` on the `Steam` target always fails `{"code": -32000, "message": "Target crashed"}`.
  `cdp eval` on `SharedJSContext` works and reports
  `viewport 1×1`, `dpr 1.33`, `visibilityState: visible`,
  UA `… Valve Steam Client/Steam Deck/default/0 … Chrome/126.0.6478.183 …`.
- Process list (guest `ps`): `gamescope-wl` ~907 MB RSS (single largest), `steamwebhelper`
  (browser) ~124–220 MB, `steam` ~95 MB, plus 2–4 short-lived renderers.

### Memory snapshot at the same time

```
MemTotal 3016708 kB   MemFree 35088 kB   MemAvailable 440140 kB   Cached 464656 kB
guest free: total 2946 MB, used 2406, free 28, buff/cache 626, available 539
Swap: 511 total, 511 used, 0 free
```

Top swap consumers are Android system processes (`oid.tv.settings` 89 MB, `system_server` 77 MB,
`systemui` 65 MB, `surfaceflinger` 59 MB), not the guest. `echo 3 > /proc/sys/vm/drop_caches`
is not available without root.

### Chromium/CEF evidence

`cef_log.txt` (this session):

```
[2861:2861:1024/224657.902978:WARNING:sad_tab.cc(255)] Tab Killed:
[4558:4558:1024/224659.208670:INFO:crash_reporting.cc(238)] Crash reporting enabled for process: utility
[4560:4560:1024/224659.208989:INFO:crash_reporting.cc(238)] Crash reporting enabled for process: renderer
[4560:4560:1024/224659.209258:WARNING:chrome_main_delegate.cc(324)] process type 'renderer' should be created through the zygote.
[4568:4568:1024/224659.464990:INFO:crash_reporting.cc(238)] Crash reporting enabled for process: renderer
[4645:4645:1024/224701.280527:INFO:crash_reporting.cc(238)] Crash reporting enabled for process: utility
[4646:4646:1024/224701.280689:INFO:crash_reporting.cc(238)] Crash reporting enabled for process: renderer
[4677:4677:1024/224703.495185:INFO:crash_reporting.cc(238)] Crash reporting enabled for process: renderer
[2861:2861:1024/224748.708701:WARNING:sad_tab.cc(255)] Tab Killed: https://steamloopback.host/
[5146:5146:1024/224752.045279:INFO:crash_reporting.cc(238)] Crash reporting enabled for process: renderer
```

Other recurring Chromium lines: `bus.cc Failed to connect to the bus … Unknown address type`
(dbus genuinely absent), `file_path_watcher_inotify.cc Failed to read
/proc/sys/fs/inotify/max_user_watches`, `udev_watcher.cc Failed to begin udev enumeration`,
`cast_crl.cc CRL - Verification failed`, `gpu_memory_buffer_support_x11.cc dri3 extension not
supported`, `ffmpeg_common.cc Unsupported pixel format: -1`,
`v4l2_utils.cc Could not open /dev/video-dec0`.

`steamui.txt` (whole log is only "frame stalled" warnings):

```
[2026-10-24 22:42:49] Warning: SteamUI thread frame stalled for: 8048 ms
[2026-10-24 22:46:26] Client version: 0
[2026-10-24 22:46:26] Warning: SteamUI thread frame stalled for: 7223 ms
```

`webhelper_js.txt` proves the UI JS *does* progress between kills (this is the furthest we have
got): `async CM Connect`, `BasicUIStore`, `SecurityStore`, `LoginStore`, `ControllerStore`,
`SteamAppInit`, then `Loading previous config params … setting locale: en`, then it repeats.

### Minidumps

`files/linuxfs/tmp/dumps/` (guest `/tmp/dumps`) exists, mode 1777, owned by the app uid, with
`attachments/ completed/ new/ pending/ settings.dat` all freshly recreated at 22:44. **`new/` and
`pending/` are empty.** A probe *from inside the guest* just showed:

```
$ df -h /tmp
/dev/root        12G   11G  953M  92% /
$ stat -c "%F %a %U %s" /tmp /tmp/dumps /tmp/dumps/new
stat: cannot statx '/tmp': Function not implemented
stat: cannot statx '/tmp/dumps': Function not implemented
stat: cannot statx '/tmp/dumps/new': Function not implemented
$ echo hi > /tmp/dumps/new/probe && echo write-ok
write-ok
```

So the guest can **write** into `/tmp/dumps/new`, but `statx(2)` on `/tmp` and its children returns
`ENOSYS` through proot (writes and `ls` of the directory work; `stat` does not). That is a very
strong lead for the empty-dump problem and possibly for CEF itself — please treat “what does
`statx`/`fstatat` return inside this proot build, and what does crashpad/Chromium do on `ENOSYS`”
as question 0. In the session log the Steam breakpad handler did run
(`10/24 22:30:35 Init: Installing breakpad exception handler for appid(steam)/version(1.0)`) and
printed `Failed writing minidump, nothing to upload.` Android's audit log shows
`crashhandler.so` being executed by `steamwebhelper` at 22:41:41 (and AVC denials for
`search … cgroup2` from those short-lived crashhandler pids).

Note the browser is launched with Steam's own `--disable-crash-reporter` and
`--crashpad-handler-pid=0`; renderer cmdlines also carry `--crashpad-handler-pid=0
--enable-crash-reporter=,`.

### Launch flags (browser process)

Steam's own: `-nocrashdialog -lang=en_US … -buildid=0 -steamid=0 -uimode=4 -startcount=0
-gamepadui -steamdeck -steamos3 -steamuniverse=Public -realm=Global … --no-sandbox
--password-store=basic --log-file=…/cef_log.txt --disable-crash-reporter
--disable-quick-menu --disable-component-update --gaia-url=http://disabled.invalid`.
Steam passes `--disable-gpu --disable-gpu-compositing --ignore-gpu-blocklist`; steamwebhelper then
picks `--use-angle=swiftshader-webgl --use-gl=angle --enable-features=V4L2VideoDecode`.
Our injected: `--no-zygote --in-process-gpu -AudioServiceOutOfProcess -V4L2VideoDecode`
(rewritten by `tools/linuxfs/preload/webhelper.c`, which can add flags and `+Feature`/`-Feature`
tokens, and cannot remove Steam's flags).

The gamepad UI’s own client is `steam://install/4427310` (ARM64 Proton), and the session script
`droiddeck-steam-compat` polls CDP; its failures this run:

```
droiddeck-steam-compat: waiting for the client (timed out)
droiddeck-steam-compat: waiting for the client (Runtime.evaluate: Target crashed)
droiddeck-steam-compat: waiting for the client (Runtime.addBinding: Target crashed)
droiddeck-steam-compat: waiting for the client (Error: GetCompatTools 0 failed
  (2 no handler for message: CompatManager.GetCompatTools#1) at self.get (<anonymous>:91:22))
```

---

## 3. Facts that feel load-bearing (please challenge them)

- `SharedJSContext` renderer **survives**; only the page renderer for
  `https://steamloopback.host/` dies. Both can exist at once, which is why we doubt a pure
  whole-process OOM kill.
- The kill is silent in Chromium's log (`Tab Killed` only) and produces **no minidump** even though
  crash reporting is "enabled" for the renderer per `crash_reporting.cc`.
- The main page sometimes reaches the **OOBE language route** before dying; `SteamUI thread frame
  stalled` climbs to 7–11 s right before/kills. This could be cause or consequence.
- The guest has **no dbus, no systemd, no /dev/video-dec0, no DRM render node** (gamescope runs on
  lavapipe, `wlserver: Failed to get DRM FD from renderer` is expected).
- The whole guest lives in proot: every guest process has the launcher’s uid and `oom_score_adj 0`;
  pids are host pids; `/proc` inside the guest is the host `/proc`.

---

## 3a. Strong lead found while writing this: proot's fast path returns raw `ENOSYS` for `statx` on this 4.9 kernel

Kernel is `4.9.141-tegra-gb6e5605a` (no `statx(2)`; added in 4.11). Guest processes are ptraced by the
project's patched proot, and the app preloads `libblfastpath.so` with `PROOT_FASTPATH` set
(default pref `prootFastPath=true`, `app/src/main/java/com/droiddeck/launcher/runtime/ProotFastPath.kt`).
In `tools/proot/fastpath/fastpath.c`:

```c
int statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *out) {
  ...
  if (rs == 0 || rs == UNSEEN)
    rs = sc(SYS_statx, AT_FDCWD, (long)r.host, flags | ..., mask, (long)out);   // trampoline page: runs UNTRACED
  ANSWER(rs, dirfd, path, real_statx(dirfd, path, flags, mask, out));
}
```

`sc()` executes from the fast-path trampoline page, which proot's seccomp filter lets run without a
stop (patch `0014`). On a 4.9 kernel the raw `SYS_statx` therefore returns `ENOSYS`, and `ANSWER`
returns it as-is (it only falls back to `real_statx` when the internal resolve returns `FP_SLOW`).
Proot itself *does* emulate `statx` on kernels < 4.11 (`tools/proot/PATCHES.md`, patch `0013` note),
but the fast path bypasses that emulation. Consequence: `stat` (coreutils 9.x calls `statx` directly)
and any library that uses `statx` fail with `ENOSYS` in every guest process, while `ls`/`open`/`write`
work.

This is cheap to test and might be the whole story — please check (a) whether Chromium 126 / CEF /
crashpad / Steam's runtime call `statx` on the renderer's hot path or in profile/cache setup, and
(b) whether failures would present exactly as a silent renderer death. The experiment is one pref:
`prootFastPath=false` (Settings → developer/proot section, or
`SessionPrefs.setProotFastPath`) for the next session; then `stat /tmp` should work again in the
guest.

---

## 4. Questions for the researcher

0. **`statx` returns `ENOSYS` inside this proot guest** (see §2 and §3a; kernel 4.9.141 has no
   `statx`, and the fast path's trampoline bypasses proot's emulation). Confirm the mechanism from
   `tools/proot/fastpath/fastpath.c` / `patches/0014` (we think `ANSWER` returns the raw `ENOSYS`;
   `FP_SLOW` is the only fallback trigger), say whether `prootFastPath=false` is sufficient to
   restore `statx`, and — most important — whether Chromium 126 / crashpad / Steam depend on `statx`
   in a way that would explain “Failed writing minidump, nothing to upload” or a silent renderer
   death. If you know of a smaller fix (e.g. have the fast path return `FP_SLOW` for `statx` when
   the kernel lacks it), outline it.
1. **Kill mechanism.** Which Chromium 126 code paths log exactly `sad_tab.cc(255) Tab Killed`
   (without a reason string), and what conditions lead there? Distinguish: kernel OOM kill,
   renderer `SIGSEGV`/`SIGABRT`, Chromium’s own “kill hung renderer”, and a deliberate
   `RenderProcessHost::Shutdown` from the embedder (Steam). Is there a Chromium switch we can add
   that makes it log the reason / the exit status (e.g. `--enable-logging=stderr --v=1`, or a
   `--log-level`)? Does Steam’s embedder call `Shutdown` on a page when its SharedJSContext
   handshake fails?
2. **Minidumps despite `--disable-crash-reporter`.** Steam passes it; our LD_PRELOAD argv rewriter
   can add flags but not remove Steam’s. Options to evaluate: (a) does appending
   `--enable-crash-reporter=…` override the presence of `--disable-crash-reporter` in Chromium 126
   (I believe `HasSwitch(kDisableCrashReporter)` wins — confirm from source), (b) is there a
   `crashpad_handler` executable we can launch/attach manually with
   `--database=/tmp/dumps`, (c) can we use `chrome://crash` equivalents / `--crash-on-failure`,
   (d) is `--no-zygote` preventing child stderr from reaching the log, and would
   `--enable-logging --log-level=0` in the browser put renderer messages in `cef_log.txt`?
3. **Proot + CEF 126 on aarch64.** Are there known reports of CEF/Chromium renderers dying in
   proot (ptrace-based syscall emulation) — e.g. clone/seccomp/`rseq`/`membarrier`/`PR_SET_NO_NEW_PRIVS`
   or V8 JIT W^X under Android SELinux (`execstack` denials are already in our audit log:
   `avc: denied { execstack }`, `search … cgroup2`)? Does the FEX 2603 “CEF FD handling” issue from
   the Phase-1 reply have an upstream Chromium/CEF bug id we can compare against?
4. **Flag experiments worth running** (rank by expected value, cheapest first) for an aarch64,
   software-rendered, 3 GB, no-GPU Steam UI:
   `--single-process`, `--disable-dev-shm-usage`, `--js-flags=--jitless`,
   `--disable-features=SpareRendererForSitePerProcess,V8CodeRangeAllocation,…`,
   `--in-process-gpu` removal, `--no-zygote` removal, `--disable-gpu-compositing` removal,
   `--use-gl=swiftshader` vs `--use-angle=swiftshader-webgl`,
   `--disable-features=V4L2VideoDecode` (already set),
   `--renderer-process-limit=2/0`, `--disable-site-isolation-trials`
   (`--valve-enable-site-isolation` is passed by Steam), and `--no-sandbox` already on.
   Say what each one changes at the renderer level, not just “try it”.
5. **`/dev/shm`.** Chromium uses memfd rather than `/dev/shm` these days, but if the guest’s
   `/dev/shm` (or `/tmp`) is tiny/unwritable inside proot, which *renderer* allocations fail and
   what would the log look like? Is `--disable-dev-shm-usage` still meaningful in CEF 126?
6. **The `1×1` SharedJSContext viewport** — normal for the hidden shared context, or a sign the
   shared context was created without an Ozone surface (which would explain a page-level crash
   loop when the main page tries to attach)? How does Steam’s CEF create that context on Deck?
7. **If dumps are impossible:** what is the cheapest way to get a stack from a dying renderer on
   an unrooted Android device with a proot guest — e.g. run the *same* `steamwebhelper` outside
   gamescope (`DISPLAY` on a normal Xwayland) to see whether it survives; or launch it under
   `gdbserver`/`catchsegv` (guest has gdbserver, binary is Valve’s, likely stripped but has build
   ids); or set `ulimit -c` + `core_pattern`? Give the concrete command sequence.

## 5. How to answer

Write `docs/request-for-help-3-replies.md` (or append to
`docs/ExternalAgentResponses/`). Plain answers with source references (`sad_tab.cc`, Chromium
switch docs, CEF forum threads) are more useful than speculation. If you propose a flag matrix,
put the expected observable change next to each flag so we can run one restart (~2 min) per row.
If something in §2 contradicts what you find upstream, say so — we will re-verify on device.

## 6. Constraints

- Do not modify the device/repo from the research side; this session owns the device.
- No root, no flashing, no NVIDIA blob redistribution; GPL-3.0 obligations apply to DroidDeck.
- Guest is glibc aarch64; do **not** propose NDK/bionic builds. The guest has python3.14 but no
  compiler; binaries must be cross-built or built in the archlinuxarm CI.
- One restart of the Steam session costs ~1–2 min and ~950 MB RSS for gamescope; keep experiments
  cheap and independent.
