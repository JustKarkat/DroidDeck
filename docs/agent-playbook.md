# Agent playbook — ShieldDeck on the Shield TV

For any agent working on this machine. Everything here is verified from this repo's sessions; when in
doubt, `docs/handoff-shield-tv-2026-10-25.md` has the evidence.

## 1. Facts

- Repo: `/home/joshuashearussell/Documents/Experiments/ShieldDeck` (fork `fork` =
  `https://github.com/JustKarkat/DroidDeck.git`, branch **`shield-tv`**). Never push to `upstream`.
- App: `com.droiddeck.launcher` (package ID deliberately kept; brand is now **ShieldDeck**).
  `minSdk 26`, `targetSdk 28`, release APK at `app/build/outputs/apk/release/app-release.apk`.
- Device: `192.168.0.194:5555` (NVIDIA Shield TV, Android 11, 3 GB RAM, **no root**).
  `run-as com.droiddeck.launcher` works for app data.
- The guest is a glibc aarch64 Arch rootfs under proot; sessions run gamescope (patched, lavapipe) and
  Steam. Build/deploy of the *app* is local; gamescope is built in GitHub Actions.
- Host tools: `adb`, `python3`, `gh` (authenticated), `./gradlew`, `magick`/`convert`, `docker`
  (arm64 images need qemu; CI is the reliable builder).

## 2. Connect (do this first)

```sh
adb connect 192.168.0.194:5555
adb devices                     # must say "device"
# if "offline" / "No route to host": retry; the TV may need its network-debugging prompt approved
adb disconnect 192.168.0.194:5555; sleep 3; adb connect 192.168.0.194:5555
```

`adb` drops under load. If every command hangs: wait, reconnect, and if the device is wedged
(`adb shell` times out, memory exhausted) do `adb shell am force-stop com.droiddeck.launcher`; a
reboot is the last resort.

## 3. Build and install the app (the normal loop)

```sh
cd ~/Documents/Experiments/ShieldDeck
python3 tools/release/runtime_inputs.py record      # REQUIRED after changing tools/linuxfs or assets
./gradlew :app:assembleRelease -q
ls -l --time-style=+%H:%M app/build/outputs/apk/release/app-release.apk   # check the mtime!
adb -s 192.168.0.194:5555 install -r -t app/build/outputs/apk/release/app-release.apk
adb -s 192.168.0.194:5555 shell am force-stop com.droiddeck.launcher
```

- If install says `INSTALL_FAILED_VERIFICATION_FAILURE`: run `am force-stop` first, then retry with
  `-t` (the `-t` is required). A failed install leaves the **old** APK running — always check mtime.
- Do **not** uninstall (it deletes the ~7 GB Steam rootfs in app data).

## 4. Sessions (`tools/droiddeckctl`)

```sh
dd() { python3 tools/droiddeckctl --serial 192.168.0.194:5555 "$@"; }

dd state                     # phase, firstFrame, sessionId, running
dd start steam --timeout 30  # or: dd start desktop
dd stop                      # may time out; fall back to am force-stop
dd focus                     # gamescope focus + X windows (can time out under load)
dd screenshot /tmp/x.png
dd wake
dd input pad down            # virtual pad: a b c x y lb rb start select guide qam up/down/left/right
dd input key ENTER           # also: text, tap, swipe, stick, trigger
dd cdp targets | dd cdp eval --target SharedJSContext '...'
dd env list | dd env set 'K=V' ... | dd env clear all
dd prefs get session KEY
dd prefs set session KEY VALUE --type string     # bool/int/long/float/string/remove
dd logs                      # pull the newest session artifact dir
```

Session facts:
- `dd start` returns before the guest is ready; wait for `dd state` → `"phase": "READY"`,
  `"firstFrame": true` (usually 60–120 s), or read the session log.
- The debug env (`dd env set`) seeds the **next** session; `dd env clear all` resets it.
- `ddd input pad …` writes the same fake pad the controller uses; it is the reliable way to drive
  Steam's UI from an agent.

## 5. Logs (read these instead of retrying `focus`/`state`)

```sh
adb shell run-as com.droiddeck.launcher ls -t files/logs | head -3
S=<session-id>
adb shell "run-as com.droiddeck.launcher sh -c 'tail -60 files/logs/$S/session.log'"   # guest stdout + gamescope
adb shell "run-as com.droiddeck.launcher sh -c 'tail -5  files/logs/$S/wayland.log'"   # host compositor stats
adb shell "run-as com.droiddeck.launcher sh -c 'tail -40 files/logs/$S/app.log'"       # app side
adb shell "run-as com.droiddeck.launcher sh -c 'cat       files/logs/$S/events.jsonl'" # phase events
# Steam:
adb shell "run-as com.droiddeck.launcher sh -c 'tail -20 files/linuxfs/root/.local/share/Steam/logs/steamui.txt'"
adb shell "run-as com.droiddeck.launcher sh -c 'tail -20 files/linuxfs/root/.local/share/Steam/logs/cef_log.txt'"
```

Gamescope diags in `session.log`: `0123-diag: main loop … repaint=`, `0123-diag: paint_all …`,
`0124-diag: focus pick=…`, `0124-diag: commit surf=…`, `0124-diag: commit fence kind=…`,
`0125` fix, `0126` shm guard.

## 6. When `dd guest`/`state`/`focus` time out (starved app main thread)

Drive the in-guest agent directly through its file protocol. The agent polls
`files/session/agent/req`, executes, and writes `files/session/agent/resp/<id>.json`:

```sh
REQ=$(printf '%s' '{"kind":"exec","argv":["bash","-c","uname -a"],"env":{},"timeout":20}' | base64 -w0)
adb shell "run-as com.droiddeck.launcher sh -c 'echo $REQ | base64 -d > files/session/agent/req/j1.json.tmp && mv files/session/agent/req/j1.json.tmp files/session/agent/req/j1.json'"
sleep 4
adb shell "run-as com.droiddeck.launcher cat files/session/agent/resp/j1.json"
```

Request kinds: `exec` (`argv`, optional `env`, `stdin`, `timeout`), `focus`, `cdp`, `ping`,
`steam-selection`, `quit`. Responses are `{"ok":true, …}` or `{"ok":false,"error":{…}}`.
Useful when `dd` fails: guest commands, `DISPLAY=:0 xprop -root …`, python probes.

## 7. Gamescope changes (patched binary in the APK)

Gamescope is patched from a `patchgen` tree and built in CI; the binary is then staged as an asset:

```sh
# 1. edit /tmp/opencode/patchgen/b/src/... (a/ = pristine)
cd /tmp/opencode/patchgen
{ for f in src/backend.h src/Backends/WaylandBackend.cpp src/rendervulkan.cpp \
          src/rendervulkan.hpp src/steamcompmgr.cpp; do
    diff -u a/$f b/$f --label a/$f --label b/$f || true; done; } \
  > ~/Documents/Experiments/ShieldDeck/tools/gamescope/patches/0123-shm-output-fallback.patch
# 2. commit + push fork shield-tv
cd ~/Documents/Experiments/ShieldDeck && git add tools/gamescope/patches && git commit -m "gamescope …" && git push fork shield-tv
# 3. wait for CI, then:
RID=$(gh run list -R JustKarkat/DroidDeck --limit 1 --json databaseId -q '.[0].databaseId')
gh run watch $RID -R JustKarkat/DroidDeck --exit-status
rm -rf /tmp/opencode/gs && gh run download $RID -R JustKarkat/DroidDeck -n gamescope-patched -D /tmp/opencode/gs
cd /tmp/opencode/gs && sha256sum -c gamescope.tzst.sha256 && tar --zstd -xf gamescope.tzst
cp usr/local/bin/gamescope ~/Documents/Experiments/ShieldDeck/app/src/main/assets/linuxfs/usr/local/bin/gamescope
# 4. rebuild + install (section 3)
```

CI workflows: `build-gamescope.yml` (artifact `gamescope-patched`), `build-hybris.yml` (artifact
`hybris-probe`), `build.yml` (APK). `gh run list -R JustKarkat/DroidDeck`.

`diff -u` exits 1 when files differ — never chain the regen with `&&`.

## 8. Device quirks that cost time

- **Install verification failure** → see §3; always `-t`, check APK mtime.
- **`dd stop`/`focus`/`state` timeouts** are normal under load; read logs. `am force-stop
  com.droiddeck.launcher` is the reliable stop.
- **ADB offline/no route** → reconnect; the user may need to accept network debugging on the TV.
- **proot `statx` returns ENOSYS** on this 4.9 kernel: `stat`/`ls` on some paths fail with
  "Function not implemented". Use `python3 -c 'os.listdir(...)'` or `open()` instead.
- **Nested ptrace is unavailable** (proot already traces); `strace`/`gdbserver` inside the guest do
  not work. Use in-process fault handlers / file logging.
- **The app can die when backgrounded** on this 3 GB device (LMKD). Keep it foreground during tests.
- **`--in-process-gpu` is off by default** now (session script); don't re-add it.
- VM/apps churn: `SessionReclaim` kills the TV launcher/recommendations at session start; don't
  remove it (measured 115 ms → 8 ms `render_scene`).
- Input: only analog-stick gamepads become players (`gamepadOnly` pref); the SHIELD remote is
  intentionally not a player.

## 9. Current state / do not redo

- **Steam gamepad UI renders** (`firstFrame: true`, READY). Fixes in place: gamescope `0125`
  (missing dma-buf fence = commit ready), `0126` (don't overwrite an shm output buffer the host
  still shows), oom guard (CEF children at `oom_score_adj 0`), pad filter, app reclaim, and
  `/system/lib64` + `/apex/com.android.runtime` + `/vendor` binds for the GPU spike.
- Branding is **ShieldDeck**: app name in all locales, adaptive icon, 320×180 TV banner, in-app
  wordmark/loading logo (commit `1d07a4ce`).
- GPU/libhybris: read `docs/hybris-gpu-status.md`. It builds and loads the Android stack, but stock
  Android 11 bionic fails libhybris' TLS contract; the missing artifact is a patched API-30 bionic
  (`hybris-hal`). Do not redo the linker/APS2 work.
- Performance is still the open problem (software CEF + lavapipe, ~2 fps UI, Steam UI thread stalls).
- `0124-diag` logging is temporary and should be removed before release.

## 10. Safety

- Don't uninstall the app, delete `files/linuxfs` or Steam data, or run `pm clear`.
- Don't force-push `shield-tv`; don't push to `upstream`.
- Don't delete the pre-existing untracked dirs (`.CleanupShield/`, `prepared-components/`,
  `stores-cargo-lock/`, `xbox-arm64/`, `shield-probe.txt`).
- The device is shared with the user: check `dd state` before taking it over, and stop sessions you
  started when done.
