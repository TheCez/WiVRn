# WiVRn Android-phone-as-server port — status

Goal: run `wivrn-server` (the Monado-based OpenXR runtime + compositor +
network streamer that normally runs on a Linux desktop PC) directly on an
Android phone, so the phone does the rendering and a headset (Quest 1
first) just supplies 6DoF tracking and displays the streamed frames. Fork
of upstream WiVRn, developed toward an eventual upstream PR — **the desktop
build must keep working unchanged**; every Android-specific change is
additive/conditional, never a rewrite of shared code.

- Fork: `TheCez/WiVRn` (upstream: `WiVRn/WiVRn`)
- Local clone: `/home/thecez/VR_Development/projects/ForeverXR/wivrn-android`
- Branch: `android-phone-server`
- Reference device: Pixel 10 Pro XL (PowerVR D-Series DXT-48-1536 / Tensor
  G5), used for every on-device verification below.
- **There is now a real, installable APK** (`server-app` Gradle module) that
  builds, installs, launches, and listens on WiVRn's default port without
  crashing. No real headset has connected to it yet — see Milestone 2.

Read this file before touching anything here again — it has the toolchain
paths, exact commands, and *why* behind non-obvious decisions, so none of
it needs rediscovering.

## Toolchain — entirely self-contained under `ForeverXR/tools/`

**Never touches Unity's own NDK/SDK** (`~/Unity/Hub/Editor/6000.3.23f1/.../PlaybackEngines/AndroidPlayer/...`) —
explicit user requirement, since that install must keep building for Quest 1
via Unity. Everything below was downloaded fresh, read-only reuse only.

| Tool | Path | Why a separate copy was needed |
|---|---|---|
| JDK 21 (Temurin) | `ForeverXR/tools/jdk` | No Java at all on the host; needed for Gradle and the Android SDK's `sdkmanager` |
| Android SDK | `ForeverXR/tools/android-sdk` | No Android SDK existed anywhere on the host. Bootstrapped via the SDK's own `cmdline-tools`/`sdkmanager` (downloaded standalone, licenses auto-accepted with `yes \| sdkmanager --licenses`): `platform-tools`, `platforms;android-34`, `build-tools;34.0.0` (Gradle auto-installed `35.0.0` too, harmless), `ndk;29.0.14206865` (matches the root project's `ndkVersion` — **this is what `server-app` actually builds against**, not the standalone NDK r28b below), `cmake;3.31.6` |
| Portable CMake 3.31.6 | `ForeverXR/tools/cmake/bin/cmake` | Used for the **manual/raw** `cmake`+`ninja` builds only (Milestone 1, and quick iteration outside Gradle) — WiVRn requires CMake ≥3.28 and none existed on the host at all. The Gradle build (`server-app`) uses the SDK-managed CMake 3.31.6 above instead, automatically. |
| Standalone NDK r28b (Clang 19) | `ForeverXR/tools/ndk` | Used for the **manual/raw** builds only, same reason as above. `server-app`'s Gradle build uses the SDK's own `ndk;29.0.14206865` instead (see the "NDK version churn" note below — both needed the same set of fixes, plus one more that only showed up under NDK 29). |
| Vulkan-Headers 1.4.328 | `ForeverXR/tools/vulkan-headers` | The NDK's bundled Vulkan headers report `VK_HEADER_VERSION` too old for WiVRn's `Vulkan_VERSION >= 1.4.304` check, under **both** NDKs above. Passed via `-DVulkan_INCLUDE_DIR=...` in both the manual commands and `server-app/build.gradle`; the NDK's own `libvulkan.so` stub is still used for linking (headers/stub-lib version need not match — the real driver is what's on-device at runtime, confirmed 1.4.317 there) |
| glslangValidator | `ForeverXR/tools/glslang/extracted/usr/bin/glslangValidator` | Needed as a **host** tool (compiles shaders to SPIR-V at build time) by both Monado's `cmake/SPIR-V.cmake` (`GLSLANGVALIDATOR_COMMAND`) and WiVRn's own `cmake/CompileGLSL.cmake` (`Vulkan::glslangValidator` imported target, via `Vulkan_GLSLANG_VALIDATOR_EXECUTABLE`). Obtained via `apt-get download glslang-tools` + `dpkg-deb -x` (no root needed — `apt-get download` doesn't require sudo, unlike `apt-get install`) |
| Eigen3, nlohmann_json, CLI11 | `ForeverXR/tools/prefix` (a real install prefix — `cmake --install`'d there, header-only libs so no cross-compilation needed) | Not present on the host at all, and this codebase has no `FetchContent` fallback for these three (unlike Boost, which it does fetch itself when `WIVRN_USE_SYSTEM_BOOST=OFF`) |

No `sudo` was used anywhere in this toolchain (deliberately — avoids needing
an interactive password mid-session). `apt-get download` (not `install`)
worked without root for glslang-tools.

**More host tools, added during the Milestone 4.5 investigation** (same
`apt-get download` + `dpkg-deb -x` pattern as glslang-tools above, or a
direct static-binary download where that's simpler):

| Tool | Path | Needed for |
|---|---|---|
| spirv-opt | `ForeverXR/tools/spirv-tools/extracted/usr/bin/spirv-opt` | `WIVRN_OPTIMIZE_SHADERS=ON` (`cmake/CompileGLSL.cmake`'s `find_program(SPIRV_OPT spirv-opt)`) — was OFF for `server-app` since Milestone 1 for no real reason (spirv-opt just hadn't been wired up yet); re-enabling it made no correctness difference either way, but it's the correct default so left ON. Also ships inside the NDK itself (`<ndk>/shader-tools/linux-x86_64/spirv-opt`) if this copy ever goes stale. |
| gettext (`msgfmt`/`msgmerge`) | `ForeverXR/tools/gettext/extracted/usr/bin/` | The **client**'s `CMakeLists.txt:199 find_package(Gettext)`, for locale `.mo` generation. `gettext-base` (commonly preinstalled) is NOT enough — only the full `gettext` package has `msgfmt`/`msgmerge`. |
| rsvg-convert | `ForeverXR/tools/rsvg/extracted/usr/bin/rsvg-convert` | The **client**'s icon/image generation (`CMakeLists.txt:203`, `librsvg2-bin` package) |
| ffmpeg (static build) | `ForeverXR/tools/ffmpeg-static/ffmpeg` | Not a build dependency — a **host-side diagnostic tool**, for decoding `WIVRN_DUMP_VIDEO` captures on this PC (see "Diagnosing stream corruption" below). The distro's `ffmpeg` package needs a long chain of shared libs (`libavdevice.so.60` etc.) not present on this host; the self-contained static build from `johnvansickle.com/ffmpeg` sidesteps that entirely — `curl -sL -o ffmpeg.tar.xz https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz && tar -xJf ffmpeg.tar.xz --strip-components=1 -C ForeverXR/tools/ffmpeg-static` |

**Client build: not yet fully working.** Confirmed needed so far (spirv-opt,
gettext, rsvg-convert above), but configuration currently still fails at
`CMakeLists.txt:204 find_program(KTX ktx)` (the KTX texture tool) with more
host tools likely needed after that one too — the client was never built
from this from-scratch toolchain before this session; whatever produced the
currently-installed `org.meumeu.wivrn.local` APK on the test Quest used a
different, fuller toolchain (Android Studio, most likely) predating this
project. Not pursued further once the server-side `WIVRN_DUMP_VIDEO` +
host-ffmpeg approach below turned out to answer the same question without
needing it. Extra `CMAKE_PROGRAM_PATH` entries and `-Pfetchcontent_base_dir=`
for the client build (once resumed) go in the root `build.gradle`'s
`externalNativeBuild.cmake.arguments`, not `server-app/build.gradle` — the
client **is** the root project (see `settings.gradle`'s own comment).

**Reusing an already-populated `FetchContent` cache** (this sandbox has no
outbound network access for most of a session; only the very first
`server-app` configure after a toolchain change can genuinely hit the
network) — pass `-DFETCHCONTENT_BASE_DIR=<an already-populated _deps dir>`
as an extra cmake argument (`server-app/build.gradle` already does this,
hardcoded to a specific `.cxx/Debug/<hash>/arm64-v8a/_deps` that exists from
an earlier successful configure) or, for an ad-hoc `./gradlew assembleDebug`
invocation of the **root** (client) project, pass
`-Pfetchcontent_base_dir=<same path>` (the root `build.gradle` already reads
that Gradle property, see its own `fetchcontentBaseDir` local). **Don't**
point this at a fresh, empty directory expecting it to get copied there —
`FetchContent`'s own subbuild stamp files bake in absolute paths, so copying
an already-populated `_deps` to a new location makes CMake think it needs to
re-populate (learned the hard way earlier this session).

**NDK version churn, briefly**: Milestone 1 discovered Unity's bundled NDK's
libc++ was missing `std::jthread`/`std::stop_token` outright, so a
standalone NDK r28b was downloaded. Milestone 2's Gradle build instead uses
the SDK-managed `ndk;29.0.14206865` (to match the root project's declared
`ndkVersion`, avoiding a second special-cased toolchain in Gradle) — same
`-fexperimental-library`/`enumerate_view`-polyfill fixes from Milestone 1
were still needed under NDK 29, **plus one more** that only surfaced there:
Monado's own `org.freedesktop.monado.auxiliary.hpp` uses `std::vector`
without including `<vector>`, which happened to work by transitive include
under r28b's libc++ but not NDK 29's. Fixed via the project's own existing
`patches/monado/*` mechanism (see `patches/monado/0012-*.patch`) rather than
hand-editing the FetchContent'd source directly, since that gets wiped on
every fresh clone. **If a plain `cmake --build` after pulling a fresh
Monado checkout ever shows this same error again**, it means the patch
didn't apply — check `patches/apply.sh` ran (it does `git am`, which needs
the fetched source to actually be a git checkout).

## Building the real APK — `server-app` Gradle module

`local.properties` (gitignored, not committed — recreate it if missing):
```
sdk.dir=/home/thecez/VR_Development/projects/ForeverXR/tools/android-sdk
```

```bash
export JAVA_HOME=/home/thecez/VR_Development/projects/ForeverXR/tools/jdk
export PATH="$JAVA_HOME/bin:$PATH"
cd /home/thecez/VR_Development/projects/ForeverXR/wivrn-android
./gradlew :server-app:assembleDebug
```

Output: `server-app/build/outputs/apk/debug/server-app-debug.apk`. Both
`libwivrn-server.so` and the self-built `libcrypto.so` are bundled into it
automatically — AGP scans the whole CMake build tree for `.so` files, not
just the `cmake.targets` entry, so no `LD_LIBRARY_PATH` workaround is
needed here (unlike the Milestone-1 manual-executable testing flow, which
still needs it).

```bash
ADB=/home/thecez/VR_Development/projects/ForeverXR/tools/android-sdk/platform-tools/adb
"$ADB" install -r server-app/build/outputs/apk/debug/server-app-debug.apk
"$ADB" shell am start -n org.meumeu.wivrn.server/org.meumeu.wivrn.server.MainActivity
```

`settings.gradle` (new) adds `server-app` alongside the existing root
project (the WiVRn Quest client — untouched, still builds exactly as
before). `server-app/build.gradle` points `externalNativeBuild.cmake.path`
at the same top-level `CMakeLists.txt` the client uses, just with different
`-D` arguments (`WIVRN_ANDROID_JNI=ON WIVRN_BUILD_SERVER=ON
WIVRN_BUILD_CLIENT=OFF`, plus the same toolchain overrides — Vulkan-Headers,
glslangValidator, the Eigen3/nlohmann_json/CLI11 prefix — as the manual
commands, since none of those are resolvable through any Gradle/AGP
mechanism on this host). Java/manifest sources are NOT under
`server-app/src/...` — `sourceSets` in that `build.gradle` points at
`server/android/java` and `server/android/AndroidManifest.xml` directly, so
all server-side Android code stays colocated with the native code under
`server/`, not scattered into a separate module tree.

## Standard test cycle — build, install, launch on both devices

Two physical devices, both reachable over `adb` wifi debugging:
Pixel (server) at `-s 192.168.20.152:33627`, Quest 1 (client) at
`-s 1PASH9BMJA9326` — check `adb devices` if these serials ever change (a
reconnect/reboot can reassign the wifi one). Server package
`org.meumeu.wivrn.server`, client package `org.meumeu.wivrn.local`, test
OpenXR app `com.UnityTechnologies.com.unity.template.urpblank` (installed on
the Pixel, launches against `wivrn-server` as its runtime — see Milestone 4).

```bash
export JAVA_HOME=/home/thecez/VR_Development/projects/ForeverXR/tools/jdk
export PATH="$JAVA_HOME/bin:$PATH"
cd /home/thecez/VR_Development/projects/ForeverXR/wivrn-android
./gradlew :server-app:assembleDebug

ADB=/home/thecez/VR_Development/projects/ForeverXR/tools/android-sdk/platform-tools/adb
APK=server-app/build/outputs/apk/debug/server-app-debug.apk
"$ADB" -s 192.168.20.152:33627 install -r "$APK"

# Full clean restart (avoids stale-connection confusion between runs):
"$ADB" -s 192.168.20.152:33627 shell am force-stop org.meumeu.wivrn.server
"$ADB" -s 192.168.20.152:33627 shell am force-stop com.UnityTechnologies.com.unity.template.urpblank
"$ADB" -s 1PASH9BMJA9326 shell am force-stop org.meumeu.wivrn.local
sleep 2
"$ADB" -s 192.168.20.152:33627 shell monkey -p org.meumeu.wivrn.server -c android.intent.category.LAUNCHER 1
sleep 2
"$ADB" -s 1PASH9BMJA9326 shell monkey -p org.meumeu.wivrn.local -c android.intent.category.LAUNCHER 1
sleep 2
"$ADB" -s 1PASH9BMJA9326 shell am start -a android.intent.action.VIEW -d "wivrn+tcp://192.168.20.152:9757" org.meumeu.wivrn.local
sleep 4
"$ADB" -s 192.168.20.152:33627 shell monkey -p com.UnityTechnologies.com.unity.template.urpblank -c android.intent.category.LAUNCHER 1
```

A `gradlew` build that finishes in ~1s (not the usual ~10-40s) is very
likely a no-op because AGP thinks nothing changed — after any native source
edit, sanity-check the actual shader/`.so` output timestamp is newer than
the edited source file before trusting an install (a real trap encountered
twice this session):
```bash
find server-app -iname "<name>.spv" -newer server/compositor/shaders/<name>.comp
```

**Reading logs**: `"$ADB" -s <serial> logcat -c` before a run to clear the
buffer, then `logcat -d` to dump what's accumulated (not `-c` again — that
clears). Useful greps: `"Failed to find a common frame"` /
`"was not sent because no shard was received"` (client-side, symptom of the
now-fixed IDR livelock, Milestone 4.5), `"IDR frame needed"` (server-side,
should be near-zero in a healthy session), `"compositor debug"` (whatever
ad-hoc `U_LOG_E` a debugging session happens to have added — **remove these
before considering a fix "done"**, several were left in and cleaned up
across this investigation).

## Diagnosing stream corruption — capture what the server actually sent

`server/encoder/video_encoder.cpp`'s `video_encoder::create()` already has a
built-in raw-bitstream dump, gated on the `WIVRN_DUMP_VIDEO` environment
variable (writes `<value>-<stream_idx>.<ext>`, continuously, for the
lifetime of the process) — a real, pre-existing WiVRn feature, not something
added for this investigation. Android apps don't inherit shell environment
variables, so it needs wiring into the JNI entry point to actually fire;
`server/android/wivrn_server_jni.cpp`'s `nativeStart()` currently has a
**temporary** `setenv("WIVRN_DUMP_VIDEO", "/data/data/org.meumeu.wivrn.server/dump_sent", 1)`
for this — remove once no longer needed, or make it conditional (e.g. a
build flag) if it turns out worth keeping permanently.

This lets you decode **exactly what the server sent** — independent of the
network and the Quest's own decoder entirely — using a completely separate
decoder (ffmpeg, on this PC). This is the fastest way to tell whether a
visual bug is upstream (compositor/shader/encoder, before any bytes leave
the phone) or downstream (network loss, or the client's own decode/render
path): if the PC-decoded frame is already wrong, the client and network are
provably not the cause.

```bash
ADB=/home/thecez/VR_Development/projects/ForeverXR/tools/android-sdk/platform-tools/adb
FFMPEG=/home/thecez/VR_Development/projects/ForeverXR/tools/ffmpeg-static/ffmpeg
DEBUGDIR=/home/thecez/VR_Development/projects/ForeverXR/wivrn-android/debugging

# after a test run (see "Standard test cycle" above):
"$ADB" -s 192.168.20.152:33627 exec-out run-as org.meumeu.wivrn.server \
    cat /data/data/org.meumeu.wivrn.server/dump_sent-0.h264 > "$DEBUGDIR/dump_sent-0.h264"
"$ADB" -s 192.168.20.152:33627 exec-out run-as org.meumeu.wivrn.server \
    cat /data/data/org.meumeu.wivrn.server/dump_sent-1.h264 > "$DEBUGDIR/dump_sent-1.h264"

# decode a few frames well past the loading splash (frame ~80+) to PNG:
"$FFMPEG" -y -i "$DEBUGDIR/dump_sent-0.h264" -vf "select='gte(n\,80)'" -frames:v 3 "$DEBUGDIR/sent0_%02d.png" -loglevel error
"$FFMPEG" -y -i "$DEBUGDIR/dump_sent-1.h264" -vf "select='gte(n\,80)'" -frames:v 3 "$DEBUGDIR/sent1_%02d.png" -loglevel error
```

`debugging/` (repo root) is a scratch folder for exactly this kind of
capture — raw `.h264` dumps and decoded `.png` frames from different points
in the investigation, kept around for before/after comparison rather than
thrown in `/tmp`. Not committed to git as a matter of course (it's working
data, not part of the port itself); prune old captures once a bug's closed
out.

The equivalent **receive-side** capture (what the Quest's decoder actually
gets handed, in `client/decoder/android/android_decoder.cpp`'s
`push_data()`) was attempted but not completed this session — the client
build hit a chain of missing host tools (see "Client build: not yet fully
working" above) deep enough that decoding the server's own sent dump turned
out to answer the same question faster. If it's ever needed: `push_data()`
receives `std::span<std::span<const uint8_t>> data` per call, already in
Annex-B order for that frame — write each `sub_data` to a per-`stream_index`
continuous file the same way, pull via `run-as org.meumeu.wivrn.local`.

## Milestone 1 — DONE: `wivrn-server` compiles, links, and runs on-device

Commit `15fb6ae8`. Proved the reusable core (compositor, driver/tracking,
`video_encoder` base + `raw` backend, Monado itself) actually builds under
the Android NDK before investing in app packaging.

| File | What / why |
|---|---|
| `CMakeLists.txt`, `server/CMakeLists.txt`, `common/CMakeLists.txt` | Gate every desktop-only dependency (GDBus codegen, Avahi, glib2, libnotify, librsvg/LibArchive/PNG, native_app_glue) behind `NOT ANDROID`; the "no encoder selected" check now also allows Android (falls back to the always-built `raw` encoder) |
| `server/hostname.cpp` | GDBus hostname lookup wrapped in `#ifndef __ANDROID__`; already had a `gethostname()` fallback for when D-Bus fails, Android just always takes that path |
| `server/driver/wivrn_uinput_android.cpp` (new) | No-op sibling of `wivrn_uinput.cpp` (`/dev/uinput` inaccessible to unprivileged apps) — same class from the shared header, since `wivrn_session.h` holds a real `std::optional<wivrn_uinput>` member |
| `server/android/wivrn_server_android_stub_main.cpp` (new) | Placeholder entry point (default build, `WIVRN_ANDROID_JNI=OFF`) replacing `main.cpp` (D-Bus dashboard IPC) and its exclusive dependents `avahi_publisher.cpp`, `sleep_inhibitor.cpp`, `start_systemd_unit.cpp`, `start_application.cpp`, `active_runtime.cpp` |
| `server/android/wivrn_ipc_socket_monado_stub.cpp` (new) | `wivrn_ipc_socket_monado` (talks to a separate desktop Monado *process* that doesn't exist here) given a real connected loopback socketpair instead of staying `std::nullopt`, since ~20 call sites dereference it unconditionally — leaving it empty would be undefined behavior. Shared by both the stub-main and JNI entry points. |
| `server/accept_connection.cpp` | Its desktop-only side effect (notifying that same separate Monado process) `#ifdef`'d out in place; the rest of the function (accepting the headset's TCP connection) is genuinely needed on Android too |
| `server/driver/wivrn_session.cpp` | `get_application_list` handler (Steam/Flatpak app picker) replies with an empty list on Android instead of calling `wivrn-common-server` (PNG/librsvg/LibArchive) |
| `common/utils/enumerate_polyfill.h` (new) | `std::ranges::enumerate_view` (C++23) isn't implemented by the NDK's libc++ (checked both r28b and 29) — polyfills the destructuring-loop usage via `zip`+`iota`, guarded by `__cpp_lib_ranges_enumerate` so it steps aside once a real implementation ships |
| `server/driver/configuration.cpp` | Unused `to_iso8601(std::chrono::zoned_time<T>)` overload guarded behind the same `__cpp_lib_chrono` feature-test macro the file's *other* overload already used |
| `server/CMakeLists.txt` | `-fexperimental-library` added for the Android target (unlocks `jthread`/`stop_token`/`zoned_time`'s header declarations) |

## Milestone 2 — DONE (first working connect path): JNI entry point + Android Service wrapper + real APK

Native/UI code in commit `2fedb2c7`; Gradle module wiring and the fixes
below not yet committed as of this writing — commit once reviewed.

### What exists and is verified on-device
- `WIVRN_ANDROID_JNI` CMake option (default OFF) builds `wivrn-server` as
  `libwivrn-server.so` instead of an executable, sharing one source list
  with the Milestone-1 executable (`_wivrn_server_sources` in
  `server/CMakeLists.txt`).
- **`server/android/wivrn_server_jni.cpp`** — the real native entry point.
  `main.cpp`'s `start_server()` doesn't reimplement anything itself on
  desktop, it forks and calls Monado's own `ipc_server_main_common()` —
  already fully wired via this target's `XRT_*` CMake config. This file
  calls that directly on a background `std::jthread` instead of forking.
- **`server-app`** Gradle module — a real, installable APK. See the section
  above.
- **`server/android/java/{MainActivity,WivrnServerService}.java`** — a
  foreground `Service` (`connectedDevice` type, confirmed correct by
  observing Monado's own runtime broker use it successfully on this exact
  device in earlier logs) that loads the `.so` and drives start/stop, plus
  a minimal launcher `Activity`. **No app-picker/launcher UI** — the OpenXR
  app (e.g. a Unity build) is launched directly by the user on the phone
  and negotiates with Monado via the OpenXR runtime broker on its own,
  exactly like it already does today; this app's only job is to run the
  server and show connection status.

### Two real crashes found and fixed by actually running it on-device
Both are exactly the kind of thing that reading source alone wouldn't have
caught — found by installing and launching the real APK.

1. **`assertion "server" failed`, SIGABRT**, in
   `wivrn::instance::create_system()` (`target_instance_wivrn.cpp:52`). The
   real `server/ipc_server_cb.cpp` callback's `mainloop_entering` does more
   than log — it calls `static_cast<wivrn::instance*>(xrt_inst)->set_ipc_server(server)`,
   which is what `create_system()`'s assertion actually checks. My own
   parallel callback class only stashed the pointer into its own atomic and
   never called that. Fixed by replicating the exact same call in
   `android_ipc_server_cb::mainloop_entering`/`mainloop_leaving`.

2. **SIGSEGV inside `wivrn_session`'s constructor**, copy-constructing a
   `headset_info_packet` from garbage. Root cause, and the single most
   important fact for continuing this work: `wivrn::instance::create_system()`
   does `std::move(connection)` on an **`extern` global**
   (`std::unique_ptr<wivrn::wivrn_connection> connection;`, declared in
   `wivrn_ipc.h`, defined in `wivrn_ipc.cpp`). On desktop, `main.cpp`'s
   `headset_connected()` populates this global — accepts a TCP connection,
   runs the PIN/encryption handshake, constructs a real `wivrn_connection`
   — **before** `start_server()` is ever called, because
   `ipc_server_main_common()` calls `create_system()` **immediately at
   startup**, unconditionally, not lazily on first client. Calling
   `ipc_server_main_common()` without populating `connection` first isn't
   a missing nice-to-have, it's unsafe to call at all. Fixed: `run_server()`
   in `wivrn_server_jni.cpp` now does its own minimal version of
   `headset_connected()` first — `wivrn::TCPListener` on
   `configuration().port`, blocking `accept()`, construct a
   `wivrn::wivrn_connection` with `encryption_state::enabled` and an empty
   pin (matching desktop's default state before any explicit "pair a new
   device" action) — *then* calls `ipc_server_main_common()`. Wrapped in a
   try/catch retry loop (a failed/garbage connection attempt shouldn't take
   the whole server down) — verified by forwarding the port
   (`adb forward tcp:19757 tcp:9757`) and sending garbage over `nc`: process
   stayed alive, port returned to `LISTEN`, no crash.

### Verified on-device (this exact sequence — repeat it after any change here)
```bash
ADB=/home/thecez/VR_Development/projects/ForeverXR/tools/android-sdk/platform-tools/adb
"$ADB" install -r server-app/build/outputs/apk/debug/server-app-debug.apk
"$ADB" shell am start -n org.meumeu.wivrn.server/org.meumeu.wivrn.server.MainActivity
sleep 4
"$ADB" shell pidof org.meumeu.wivrn.server   # should print a pid, not empty
"$ADB" shell netstat -tln | grep 9757        # should show LISTEN on :::9757
```
Result as of this writing: process stays alive, port listens, UI shows "No
devices connected" (screenshot-verified).

**Update — a real Quest 1 has now connected, end-to-end, streaming real H.264**
(own-built client, same commit, see Milestone 3 below): manually added via
the client's real in-headset "Add server" UI (`lobby_gui.cpp`) pointed at the
phone's LAN IP:9757 — no discovery needed, matches the same code path as a
`wivrn+tcp://ip:port` intent. Full session negotiation completes (encryption
handshake, swapchain format/panel size negotiation, refresh rate, real
hardware H.264 encoder/decoder creation on both ends), connection reaches
`ESTABLISHED` and the OpenXR session is sustained in `XR_SESSION_STATE_FOCUSED`
indefinitely with zero errors. Genuine picture-on-headset hasn't been
eyeballed yet (needs someone wearing it) but every layer of the pipeline is
verified live.

### Live connection status UI — wired to a real native callback, not stubbed
`android_ipc_server_cb::client_connected`/`client_disconnected` in
`wivrn_server_jni.cpp` (Monado's own `ipc_server_callbacks` hooks, fired
when a headset's TCP connection to the compositor opens/closes) call back
into Java via a cached `JavaVM*`/`jobject` (attach/detach per call, since
these run on the server thread, not whatever thread called `nativeStart`).
`WivrnServerService.onClientConnected/onClientDisconnected` update a
`Set<Integer>` of connected client ids and notify `MainActivity` (if it's
alive) via a plain listener interface + `Handler(Looper.getMainLooper())`.

**Honestly scoped, not faked**: only a numeric client id is available right
now, not a device name/model — Monado's `ipc_server_callbacks` don't carry
that, and wiring up the actual `from_headset::headset_info_packet` (which
does have it, handled today only in `main.cpp`/`wivrn_session.cpp` on
desktop) to reach the UI is unstarted work. So the UI shows "No devices
connected" / "Streaming (N connected)" — not a device name, and not a
distinct "paired but not yet streaming frames" state (would need
session/frame-level hooks that don't exist yet either). Per explicit user
direction: this is enough for now, don't invent detail that isn't real.

### Known, deliberate simplifications vs. desktop — worth reconciling before this is upstream-PR-ready
- **No PIN/pairing security flow.** Was `encryption_state::enabled` with an
  empty pin — but with no pairing UI, nothing ever puts the server into
  `pairing` state to learn a client key, so `enabled` rejects every client
  outright ("Pairing is disabled on server", found by actually connecting a
  real headset). Changed to `encryption_state::disabled` unconditionally —
  same as desktop's explicit `--no-encrypt` flag (`main.cpp`). Desktop's
  actual pairing UX (dashboard "add new device" flow, PIN entry,
  `encryption_state::pairing`) isn't represented at all yet; traffic between
  phone and headset is unencrypted until it is.
- **No Avahi-equivalent discovery.** The headset still needs the phone's
  IP entered manually (or however WiVRn's client already supports manual
  connection) — no `NsdManager`-based advertising written yet.
- **One connection at a time, sequentially, forever**, matching desktop's
  actual one-session-at-a-time model, but without desktop's stop/backoff
  refinements (`delay_next_try` exponential backoff on repeated failures
  isn't replicated).
- **`listener.accept()` isn't interruptible by `std::stop_token`** — if
  `nativeStop()`/Service teardown happens while still waiting for a first
  connection, `server_thread->join()` will block until something (even a
  garbage connection) arrives. Not yet hit in testing since the app was
  always force-stopped via `am force-stop`/uninstall rather than a clean
  in-app stop, but worth fixing before relying on this being genuinely
  stoppable, and use `adb shell am force-stop org.meumeu.wivrn.server` for
  now instead of trying to test the Service's own stop path.

## Milestone 3 — IN PROGRESS: real Quest 1 connected, real bugs found by doing it

Own-built client (same commit as the server — required, `protocol_version`
is a compile-time hash) installed on a real Quest 1 via `adb`, own-built
`server-app` on a real Pixel 10 Pro XL, both on the same LAN. Added the phone
manually in the client's real in-headset UI (Address `<phone-ip>`, Port
`9757`, "TCP only" **unchecked** — leaving it unchecked lets the client also
open a UDP socket to the server's advertised `stream_port` for the actual
media stream, not just TCP control; the server does advertise a real one,
reusing desktop's connection code as-is). Four real, previously-latent bugs
found this way — every one of them only surfaces once a real client actually
completes a handshake and a session gets created, which nothing before this
had ever exercised:

1. **SIGABRT, `VmaAllocator_T` assertion**: `vulkanApiVersion >= VK_API_VERSION_1_3
   but required Vulkan version is disabled by preprocessor macros`.
   `common/CMakeLists.txt` hardcoded `VMA_VULKAN_VERSION=1001000` (Vulkan 1.1)
   for **all** Android targets — fine for the Quest client (its GPU only ever
   reports 1.1), but `server/utils/wivrn_vk_bundle.cpp` requests
   `VK_API_VERSION_1_3` unconditionally, same as desktop, and this Pixel's GPU
   genuinely supports it. Fixed: raised to `VMA_VULKAN_VERSION=1003000` —
   costs nothing on Quest, it's a compile-time ceiling, not a runtime
   requirement.
2. **Link failure**: `undefined symbol: vkGetDeviceBufferMemoryRequirements` /
   `vkGetDeviceImageMemoryRequirements`. Fallout from fix #1 — VMA now
   statically imports these Vulkan-1.3-core symbols, but the NDK's
   per-API-level `libvulkan.so` stub only exports them from **API 33**
   onward (confirmed via `nm -D` across every level in the NDK sysroot;
   absent through 32, present from 33). Fixed: `server-app/build.gradle`
   `minSdkVersion` raised from 30 to 33 (no realistic phone-as-VR-server
   target runs anything older).
3. **Every connection failed**: `Failed to find a suitable video encoder`.
   `server/encoder/encoder_settings.cpp`'s `select_encoder()` only ever picks
   the one backend that's actually compiled in for Android
   (`video_encoder_raw.cpp`, uncompressed — see below) when a config
   *explicitly* asks for it by name/codec; it never auto-selects raw the way
   it auto-detects NVENC/VAAPI/Vulkan-video on desktop. With no config file
   (no Android config UI exists), `encoders` stayed default-constructed
   (empty name, no codec) and nothing matched. Fixed: `driver/configuration.h`
   now default-initializes `encoders` to `{codec = raw}` × 3 under
   `#ifdef __ANDROID__` — the only backend that exists right now really is
   the only sane default. A config file's `"encoder"` key, if Android ever
   gets one, still overrides this exactly like desktop.
4. **Every *reconnect* attempt failed in an infinite, uninterruptible retry
   loop**: `Address already in use`, thousands of lines/sec in logcat. Once a
   session is up, if the connection drops, Monado's own
   `wivrn_session::reconnect()` (unmodified, reused from desktop) opens its
   *own* fresh `TCPListener` on the configured port via `accept_connection()`
   to wait for the headset to come back. On desktop this is safe because the
   compositor runs in its own forked child process — no other listener is
   bound in that process. `wivrn_server_jni.cpp`'s `run_server()` has no such
   isolation (one process, no fork) and was keeping its own outer
   `TCPListener` (used for the very first connection) alive for the entire
   function, i.e. the whole life of the session — so Monado's internal
   reconnect listener collided with it on the exact same port, forever.
   Fixed: scoped that outer `TCPListener` to the `try` block around the
   initial `accept()` only, so it's closed (RAII) before
   `ipc_server_main_common()` runs and the port is free whenever Monado's own
   reconnect logic needs it.

**Update — fixed, real H.264 now streams end-to-end.** A `video_encoder_mediacodec.cpp` backend was written (Android's NDK `AMediaCodec` byte-buffer encode API, `server/encoder/`), wired into `select_encoder()`'s auto-probe exactly like NVENC/VAAPI/Vulkan-video are on desktop (`encoder_mediacodec` name, `WIVRN_USE_MEDIACODEC` option/CMake plumbing mirroring the existing four backends' pattern, `common/wivrn_config.h.in` entry). The Android-forced `raw` default in `configuration.h` (added for fix #3 above) was reverted — auto-detect now finds `mediacodec` the same way desktop finds its real backends, so it wasn't needed anymore. Verified on-device: real hardware `c2.google.avc.encoder` components created for all 3 streams (left/right/alpha) on the Pixel, real `OMX.qcom.video.decoder.avc` hardware decoders created on the Quest 1 (the same proven decode path, `android_decoder.cpp`, every real Quest app already uses — `raw_decoder.cpp` is no longer in the path at all), connection `ESTABLISHED`, session sustained in `XR_SESSION_STATE_FOCUSED` with zero errors. This is a copy-based implementation (present_image() copies the compositor's rendered image to a host-visible buffer, same as video_encoder_raw.cpp, then memcpy's into MediaCodec's input buffer) — not yet the genuinely zero-copy path (AMediaCodec_createInputSurface() + rendering directly into it as a VkSurfaceKHR/VkSwapchainKHR target); see video_encoder_mediacodec.h's own `ponytail:` comment for the upgrade path. SPS/PPS (from the encoder's one-time CODEC_CONFIG output buffer) are manually prepended before every IDR, matching the client's in-band expectation (no csd-0/csd-1 passed to the client's decoder configure) and desktop's x264 backend's `b_repeat_headers=1` behavior. Dynamic framerate change is a known, documented gap (bitrate change works via the real `"video-bitrate"` runtime key; there's no equivalently well-supported MediaCodec runtime call for framerate).

**Superseded blocker, client-side (kept for history)**: with all four of the above
fixed, a real session negotiates end-to-end (encryption-disabled handshake,
swapchain format + panel size + refresh rate negotiation all succeed) and
gets as far as `client/decoder/raw_decoder.cpp` creating its per-frame
timeline semaphores — which throws `vkCreateSemaphore: Incomplete` on the
Quest 1's Adreno 540, killing the connection (the lobby scene then retries
automatically, which is what "tries to connect, then fails" looks like from
the headset). The client does correctly probe and enable
`VK_KHR_timeline_semaphore`/`VkPhysicalDeviceTimelineSemaphoreFeaturesKHR`
before this point (`application.cpp`), so this isn't a missing-extension
issue — it's `raw_decoder.cpp`'s actual `vkCreateSemaphore` call (with a
chained `vk::SemaphoreTypeCreateInfo{.semaphoreType = eTimeline}`) itself
failing on this specific old driver. Not yet root-caused. Two ways to look at
it, not mutually exclusive:
  - This is genuinely-uncommon code: `raw` is WiVRn's uncompressed
    debug/dissector codec, essentially never used against real hardware
    (every real deployment uses h264/h265 through a hardware
    encoder/decoder) — plausible nobody has ever run `raw_decoder.cpp`
    end-to-end on a real Quest 1 before, so this could be a latent upstream
    bug, not something this port introduced.
  - It's also a sign that `raw` was always the wrong long-term choice, not
    just an unproven one — the actual fix that matters is Milestone 3's
    still-open real item below (a `video_encoder_mediacodec.cpp` H.264
    backend), which sidesteps `raw_decoder.cpp` entirely and uses the exact
    decode path (`android_decoder.cpp`, `AMediaCodec`) every real Quest
    already exercises daily.

### Still open
- **DONE-ish: real hardware video encoder.** `video_encoder_mediacodec.cpp`
  exists and works (see above) — Vulkan Video was confirmed unusable on this
  Pixel 10 Pro XL (zero `VK_KHR_video_queue`/`VK_KHR_video_encode_queue`
  support, all 161 device extensions checked via an on-device probe), so
  MediaCodec's NDK byte-buffer API is the real backend instead. What's left
  is the zero-copy upgrade (see video_encoder_mediacodec.h's own comment) —
  the device does expose `VK_ANDROID_external_memory_android_hardware_buffer`
  for it — but that's a performance/latency improvement on top of something
  that already works, not a blocker anymore.
- The `raw_decoder.cpp` / `vkCreateSemaphore: Incomplete` bug (see superseded
  blocker above) is no longer in our path at all (mediacodec replaced raw as
  the default), so it's not blocking anything here — but it's still a real,
  unreported upstream bug on Quest 1's Adreno 540 if anyone ever revisits
  `raw` for debugging. Not investigated further, not our problem to fix
  right now.
- PIN/pairing security flow (see Milestone 2's simplifications above).
- Android `NsdManager`-based discovery to replace Avahi.
- An audio backend (`WIVRN_USE_PIPEWIRE=OFF`, no Android audio path written;
  `audio/audio_setup.cpp` already degrades gracefully to "no audio backend"
  so this compiles fine as-is, just doesn't do anything).
- ~~OpenXR runtime broker registration~~ — **DONE, see Milestone 4 below.**

## Milestone 4 — DONE: OpenXR runtime broker registration, a real app streaming end-to-end

The goal: a real VR app on the phone (tested with Unity's own `urpblank`
OpenXR template, same device) discovers this runtime via Android's standard
OpenXR broker mechanism, connects to our *already-running* `server-app`
process over local IPC, and actually streams to the Quest — not just the
phone-to-headset network link (Milestone 3), but a real local OpenXR client
driving it. This turned out to reuse almost all of Monado's own existing
out-of-process machinery (already vendored, mostly unused until now) rather
than requiring anything invented from scratch:

- **`server/utils/wivrn_vk_bundle.cpp` / `server/CMakeLists.txt`**: added
  `VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME` to the
  optional device extensions, and `-DVK_USE_PLATFORM_ANDROID_KHR` to
  `wivrn-server`'s own compile definitions (client/CMakeLists.txt already had
  the latter; wivrn-server never needed it before this).
- **`server-app/build.gradle`**: builds a *second* CMake target from the same
  vendored Monado tree, `openxr_wivrn` (Monado's own `targets/openxr`,
  `RUNTIME_TARGET` name derived from `XRT_OXR_RUNTIME_SUFFIX=wivrn` already
  set in `server/CMakeLists.txt`) — the actual loadable OpenXR runtime `.so`
  (`libopenxr_wivrn.so`), built as a thin `ipc_client` (Monado auto-computes
  this given `XRT_FEATURE_SERVICE=ON`, already set) rather than a second full
  standalone runtime. `-DXRT_ANDROID_PACKAGE=org.meumeu.wivrn.server` makes
  it bind to *this* app's package instead of Monado's own default
  (`org.freedesktop.monado.openxr_runtime.out_of_process`). AIDL support
  (`buildFeatures.aidl`) and `androidx.annotation` (needs
  `android.useAndroidX=true` in `gradle.properties`) added for the pieces
  below.
- **`server/android/aidl/org/freedesktop/monado/ipc/IMonado.aidl`** +
  **`server/android/java/org/freedesktop/monado/{ipc/Client.java,
  auxiliary/{MonadoView,NativeCounterpart}.java}`**: vendored verbatim from
  Monado (kept their own copyright/license headers — not our own
  contribution, needed byte-for-byte for wire/ABI compatibility with the
  loader every OpenXR app on this device already has). `Client.java` is the
  class the OpenXR loader's Java glue loads *from this app's own APK* into
  the calling VR app's process (`loadClassFromApk`) to bootstrap the IPC
  connection — one line adapted (`BuildConfig.SERVICE_ACTION` → its own
  literal value, since that constant is normally generated by upstream's own
  Gradle module, not ours).
- **`server/android/java/org/freedesktop/monado/auxiliary/{ActivityLifecycleListener,SystemUiController}.java`**:
  mechanical Kotlin→Java ports of Monado's own `.kt` originals (same
  behavior) — avoids adding a whole Kotlin toolchain to this module for two
  small files.
- **`server/android/java/MonadoIpcService.java`** (new, ours): a bound
  Service implementing `IMonado.Stub`, `connect()` handing the received fd to
  a new native bridge. `canDrawOverOtherApps()` returns `true` unconditionally
  — tells the calling app's `Client.java` this runtime has its own separate
  display output (the network stream) and doesn't need a local `Surface`,
  which WiVRn's compositor never uses anyway.
- **`server/android/AndroidManifest.xml`**: one `<service>` carrying both the
  real bind target (`org.freedesktop.monado.ipc.CONNECT` action) and the
  broker's own static discovery metadata (`OpenXRRuntimeService` intent-filter
  + `SoFilename`/`MajorVersion` meta-data, mirroring Monado's own reference
  manifest) — no separate broker-only service needed, the broker never binds,
  it only reads meta-data via `queryIntentServices()`.
- **`server/android/wivrn_server_jni.cpp`**: `nativeAddIpcClient(int fd)`
  bridges a connecting client's fd into the *already-running* server's
  mainloop via `ipc_server_mainloop_add_fd(server, &server->ml, fd)` —
  `server->ml` and that function are Monado's own, unmodified.

**Five real, previously-latent bugs found and fixed by testing against a
real app** (every one only surfaces once a real local OpenXR client actually
connects, which nothing before this had ever exercised):
1. A stale, unrelated `org.freedesktop.monado.openxr_runtime.out_of_process`
   package (installed earlier in this project's own testing) was also
   registering an `OpenXRRuntimeService` intent-filter, confusing Unity's
   loader into picking it over ours, and it had its own genuine crash
   (`pthread_mutex_lock called on a destroyed mutex`, unrelated to this
   project). Fixed by uninstalling it — real broker discovery then resolved
   correctly to `org.meumeu.wivrn.server` immediately, confirmed via the
   loader's own log (`getActiveRuntimeCursor: Querying URI:
   content://org.khronos.openxr.runtime_broker/openxr/1/abi/arm64-v8a/runtimes/active`).
2. `XR_ERROR_RUNTIME_UNAVAILABLE` — the loader tried to load
   `org.freedesktop.monado.auxiliary.ActivityLifecycleListener` from our APK's
   dex and failed (`ClassNotFoundException`); we'd only ever vendored the IPC
   AIDL Java module, not the separate `xrt/auxiliary/android` one `Client.java`
   itself depends on. Fixed by vendoring/porting those classes too (see files
   above).
3. Handoff succeeded but the connection reset ~18s later during shared-memory
   setup (`ipc_receive_fds: recvmsg failed: Connection reset by peer`,
   preceded server-side by `epoll_ctl(listen_socket) failed '-1'`): our
   `nativeAddIpcClient` passed the raw fd straight through, but
   `MonadoIpcService.connect()` closes its `ParcelFileDescriptor` right after
   the call returns (correct — matches upstream's own `MonadoImpl.connect()`)
   — without a `dup()` first, that close can race ahead of the new per-client
   thread's own `epoll_ctl` on the same fd number. Fixed by mirroring
   upstream's `service_target.cpp` exactly: `dup(fd)` before handing it to
   `ipc_server_mainloop_add_fd`.
4. SIGSEGV (null pointer) in `comp_multi_compositor.c`'s Android-only
   `multi_compositor_request_display_refresh_rate`, which unconditionally
   queries a globally-registered Android Context/Activity
   (`android_globals_get_context()`) that our headless foreground-Service
   architecture never registered. Fixed: `nativeStart()` now calls
   `android_globals_store_vm_and_context(vm, service)` — a `Service` is
   itself a valid `Context`, just not an `Activity`, which is all this
   particular code path needed.
5. SIGSEGV (null function pointer, `vk_create_image_from_native` calling
   through a null `vkGetAndroidHardwareBufferPropertiesANDROID`) the moment a
   local app tried to import an AHardwareBuffer-backed swapchain image. Two
   layers: (a) `wivrn_vk_bundle.cpp` never requested
   `VK_ANDROID_external_memory_android_hardware_buffer` (nothing before this
   needed AHardwareBuffer import — WiVRn's own encoder pipeline uses plain
   host-visible buffers); (b) even after adding it, the extension-name macro
   itself was silently `#ifdef`'d out, because `wivrn-server` (unlike the
   client target) never defined `VK_USE_PLATFORM_ANDROID_KHR`, without which
   `vulkan_core.h` doesn't declare Android-platform extensions/structs at
   all. Fixed both; verified via `strings` on the built `.so` that the
   extension name literal was actually present before retesting on-device.

**Verified on-device, full chain**: Unity's OpenXR loader discovers and binds
`org.meumeu.wivrn.server` via the real broker, `xrCreateInstance` succeeds,
`System Properties: Name="Oculus Quest on WiVRn"` (the real connected
headset's identity, threaded all the way through), a stereo swapchain is
created at the Quest's actual per-eye resolution (1728×1909), the OpenXR
session reaches `XR_SESSION_STATE_FOCUSED`, and the Quest's own client starts
actively receiving and decoding a real video stream (three decoders running,
matching the left/right/alpha stream setup) — not just "connected", genuine
frame traffic.

**Not yet clean**: the Quest's decoder logs periodic `frame N was not sent
because no shard was received` / `Failed to find a common frame for all
decoders` warnings — real streaming is happening, but not every frame is
making it through cleanly yet. Not investigated further this session; next
thing to look at before calling Milestone 4 fully polished. Possibly related
to the zero-copy encoder work below (video_encoder_mediacodec.cpp's current
CPU-copy path may just not be fast/consistent enough at real frame rates),
possibly a separate pacing issue — worth measuring before assuming which.

## Milestone 4.5 — DONE: root cause of the green-chroma corruption confirmed
## and fixed (a real GPU driver bug), plus several other real bugs found and
## fixed along the way

**Full permanent technical record of this and the related Milestone 4.6
investigation**: `docs/pixel10-pro-xl-gpu-media-investigation.md` —
detailed enough to understand without this chat history, extract
upstream PowerVR bug reports, and avoid accidentally reverting the
workarounds it documents. This section and Milestone 4.6 below remain
the narrative summary; that document is the detailed backing record.

**Symptom** (reported after Milestone 4): the stream showed pixelation and
green artifacts, with what looked like stereo overlap; one eye usually showed
recognizable-but-tinted geometry, the other solid green with static noise.

**Root cause, confirmed rigorously, not guessed**: several visual-only
hypotheses were tried and failed (missing stride/slice-height, an
unconditional chroma copy for the alpha stream -- a real bug, fixed, but not
the cause of the visible artifact, kept -- a U/V byte-swap -- wrong, reverted).
The methodology then shifted to raw evidence:
1. Dumped the raw pre-encode NV12 buffer (`video_encoder_mediacodec.cpp`) to
   a file, pulled via `adb shell run-as <pkg> cat`, converted to PNG with a
   hand-written NV12->RGB Python script (no numpy/ffmpeg on this host) --
   confirmed the corruption (chroma reading as 0 instead of neutral 128 over
   large regions) was already present *before* MediaCodec ever saw the data.
2. Added a temporary Vulkan capture in `server/compositor/compositor.cpp`
   (`debug_dump_app_image`, since removed) that used `vkCmdCopyImageToBuffer`
   to dump the *app's own* swapchain image -- the one Monado handed to WiVRn,
   before WiVRn's foveation shader ever touched it -- straight to a raw RGBA
   file. This image was **perfectly clean for both eyes** (a crisp "Made with
   Unity" splash, correct colours, no artifacts at all). This is the
   decisive piece of evidence: it proves Monado/the app's own rendering was
   never the problem, isolating the bug to WiVRn's own compositor code
   between that point and the encoder.
3. `server/compositor/shaders/foveation.comp` averages each 2x2 luma block
   into one chroma sample using `subgroupShuffleDown(colour, 1/2/3)` -- a raw
   cross-lane GPU op requiring `VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT`.
   Grepping `server/utils/wivrn_vk_bundle.cpp` confirmed WiVRn **never checks
   this feature is actually supported** before using it -- it's simply always
   available on desktop AMD/NVIDIA (where this shader was written and
   tested), so nobody noticed. It is *not* reliably supported on Pixel's
   Tensor GPU (Mali or PowerVR depending on generation -- not Adreno). The
   luma path (no shuffle) stayed correct; only the shuffle-dependent chroma
   path produced garbage -- matching the observed symptom exactly.

**Fix**: replaced the subgroup shuffle with workgroup shared memory +
`barrier()` -- core compute shader functionality with no subgroup
extension/capability dependency at all. `id` (used for both the spatial
remap and the shared-memory index) was already `gl_SubgroupID *
gl_SubgroupSize + gl_SubgroupInvocationID`, which the GLSL subgroup spec
guarantees equals `gl_LocalInvocationIndex` -- so the indexing math is
unchanged, just computed portably, and the actual sum-of-4-neighbours math is
identical to before. Confirmed clean afterwards with a real Quest screenshot
of an actual scene (an aurora/dome environment), both eyes correct.

**This is a genuine upstream WiVRn bug**, present in the current stable
release (v26.9) and every release since `a63aba71` ("Smarter foveation
subsampling", 2026-04-09, first in v26.6) -- not something this Android port
introduced, and not a bleeding-edge regression. It simply never manifested
on the only hardware WiVRn has been tested on (desktop AMD/NVIDIA, which both
support shuffle-relative unconditionally). Worth upstreaming.

**Second, separate, still-open bug found while verifying the fix**: even
after the chroma fix, sustained/real-world testing still showed pixelation,
green wash reappearing over time, and small black block artifacts -- but this
time proven (via the same app-image-dump technique, and via the Quest's own
in-headset statistics overlay, which showed one eye clean and the other
green-washed *including its own 2D UI panel drawn on top*) to be a different
class of problem: the Quest's log shows
`Failed to find a common frame for all decoders` with **completely disjoint**
frame-ID sets between the left/right decoders (e.g. `35889 35899 35897` vs
`35895 35893 35891` -- no overlap at all), and the corruption pattern (small
scattered black macroblocks in the less-affected eye, growing into a solid
colour wash in the other) is consistent with H.264 error propagation from a
stream that isn't self-healing via IDR requests, not a compositor colour bug.
Tested and ruled out: Wi-Fi signal (RSSI -29, 1.1Gbps link -- excellent, not
the cause), and lowering resolution/bitrate/framerate via the client's own
dashboard (no change at all).

**Root cause, found and fixed**: `video_encoder_mediacodec.cpp`'s
constructor unconditionally created, configured, and **started** a real
hardware `AMediaCodec` encoder session for all 3 streams (left, right,
alpha) as soon as the compositor was constructed -- regardless of whether
the running app ever actually submits an alpha-blend layer. For a normal
opaque app (this Unity test included), stream 2's encoder was configured and
running the entire session while never once being fed a frame
(`compositor.cpp` already gates both `present_image()` and `encode()` on
`view_info.alpha`, so it just sat idle). On this SoC that idle-but-started
third hardware video-encode session was enough to starve the two real
streams of throughput, causing them to drift frame-by-frame until no common
frame ID existed between the two decoders at all.

**Fix**: deferred `AMediaCodec_create`/`configure`/`start()` out of the
constructor and into a new `ensure_codec()`, called lazily on the first
actual `present_image()` for that stream -- exactly the same gate
`compositor.cpp` already uses to decide a stream is needed. An app that
never uses alpha now never touches the hardware encoder for that stream at
all. No wire-protocol or client change needed (the client already tolerates
a described-but-never-fed stream, since that was already the steady-state
for streams 0/1 before... just not for a *third* one).

**Verified**: `Failed to find a common frame for all decoders` dropped to
zero occurrences over a sustained 30+ second window (previously constant),
and Quest screenshots show both eyes rendering identically and cleanly with
no tint, no black-block artifacts, no desync.

**Follow-up the same session**: sustained/real testing later showed the
desync symptom (and the right-eye-specific green wash) could still recur,
just less severely and less consistently than before the lazy-alpha fix.
Root-caused two more real, contained issues on top of it:

1. `server/compositor/compositor.cpp`'s `encoder_work()` called
   `encoder->encode(...)` for each stream **sequentially on one thread**.
   Desktop hardware backends (NVENC/VAAPI) don't notice this — their
   per-call CPU overhead is negligible — but Android's `AMediaCodec` has
   real per-call JNI/Binder overhead, so stream 0 always fully finished
   before stream 1's own dequeue/queue calls even started, every frame,
   deterministically favouring whichever stream is processed first. Fixed
   by encoding each stream concurrently (one `std::jthread` per stream per
   frame, joined before the frame is freed). This required adding a
   `std::mutex send_mutex` to `server/driver/wivrn_connection.h`'s
   `send_control`/`send_stream` -- previously completely unsynchronized,
   safe only because nothing ever called them concurrently before.
2. Captured the exact bytes handed to the network socket, per stream,
   right before sending (temporary instrumentation in the shared
   `video_encoder::SendData()`, since removed) to check whether corruption
   was present before or after encoding. Confirmed both streams' data is
   syntactically valid H.264 (correct NAL start codes and slice-type
   bytes) -- ruling out a malformed-bitstream or socket-level bug -- but
   for the same frame index, stream 1 (right eye)'s P-frames were
   consistently **15-20x larger** than stream 0's, and byte-for-byte
   identical to stream 0 for roughly the first 22 bytes (the slice header
   and earliest macroblocks) before diverging into far more entropy. Since
   H.264 encodes macroblocks top-to-bottom, this points at stream 1's
   *input* buffer itself (the host-visible buffer `present_image()` copies
   into and `encode()` reads from in `video_encoder_mediacodec.cpp`) being
   correct for the top of the frame but stale/wrong further down -- a
   buffer-slot-reuse hazard in the `num_slots=2` double-buffering scheme,
   not a network or encoder-output bug. **Not yet fixed** — dispatched to
   further investigation; the parallel-dispatch fix above measurably
   reduced (not eliminated) the frequency, consistent with this being a
   race that gets worse the more stream 1 lags behind stream 0.

**Net result this session**: the original chroma-corruption bug
(`foveation.comp`'s `subgroupShuffleDown`, workgroup-shared-memory fix
above) was the dominant contributor to perceived latency and pixelation --
switching it to portable shared memory measurably fixed the bulk of the
lag/pixelation the user was seeing in practice, even though a smaller,
separate buffer-slot-reuse issue (immediately above) remains open. Next
planned step: a genuine zero-copy encoder input path (see
`video_encoder_mediacodec.h`'s own `ponytail:` comment --
`AMediaCodec_createInputSurface()` + rendering directly into it via an
imported `AHardwareBuffer`, instead of the current
GPU-copy-to-host-buffer-then-memcpy path), which should help both the
remaining buffer-lifetime hazard (no separate host buffer to race over)
and overall latency.

**Follow-up, root-caused with hard evidence from a live device (not
guessed)**: dispatched a fresh, independent review (a more thorough model,
given how long the guessing above had gone in circles) to read
`video_encoder_mediacodec.cpp` against the proven `video_encoder_raw.cpp`/
`video_encoder_vulkan.cpp` backends and the shared base class, and to pull
live evidence rather than theorize further. It refuted the buffer-slot-race
theory above (the "byte-identical prefix" was just the NAL header + a
skip-run encoding of a static top-of-frame -- expected, not evidence of a
half-written buffer; the actual code has no slot-reuse hazard, traced end
to end) and instead found, from a live session:

- **Zero partial-shard frame loss** -- every incomplete frame had exactly
  zero shards received, never some-but-not-all. Rules out network loss,
  MTU, and bitstream corruption entirely.
- Of 2045 frames, 115 were missing from **exactly one** decoder, never
  both -- a per-stream *server-side gating decision*, not resource
  contention (both encoders/decoders showed near-identical throughput
  counters).
- **Root cause**: `server/encoder/idr_handler.cpp`'s `default_idr_handler`
  is a self-sustaining IDR livelock, running independently per stream.
  `compositor::layer_commit()` drops a frame under load before any encoder
  ever sees its `frame_index` (`encode_request >= 0` check). The client
  correctly reports "didn't receive it" for that index, but
  `on_feedback()`'s `running` branch couldn't distinguish "we never even
  tried to send this" from "we sent it and it was genuinely lost" --
  wrongly treating the former as loss and demanding an IDR, then
  `should_skip()` skips *every* frame until that IDR is acknowledged (a
  7-8 frame silent run, observed live). Each stream's IDR acks land at
  different times, desyncing the two streams' frame timelines from each
  other; once desynced by more than the client's 3-deep matching window,
  `common_frame()` can never find a shared frame index between the two
  decoders again, and whichever eye is left holding a stale/blank frame
  renders as solid green (an all-zero YUV surface converts to
  RGB(0,135,0)). Stream 1 is consistently constructed/presented/sent
  second throughout the whole pipeline, so it's systematically the one
  left stale -- explaining why it was always specifically the right eye.
- **Fix applied** (`server/encoder/idr_handler.h`/`.cpp`): track which
  frame indices this encoder actually encoded and sent (`sent_frames`, a
  512-entry ring, mirroring the existing `non_ref_frames` ring), and only
  let `on_feedback()`'s "was this genuinely lost" check treat a
  `sent_to_decoder=false` report as real loss if that frame index is
  actually in `sent_frames` -- a frame the compositor silently dropped
  before this encoder ever touched it no longer triggers a false IDR
  demand. Also fixed a real (if currently harmless) bug found along the
  way: `non_ref_frames{512, uint64_t(-1)}`'s brace-init picks the
  `initializer_list` constructor over the fill constructor, making it a
  2-element vector instead of 512 -- changed to parens. Also fixed
  `video_encoder_mediacodec.cpp` telling `AMediaCodec_queueInputBuffer` the
  full `payload_size` when only `copy_size = min(in_size, payload_size)`
  was actually copied (latent truncation risk, currently harmless since
  they happen to be equal on this device).
- **Verified live**: `Failed to find a common frame for all decoders` and
  `IDR frame needed` both dropped to **zero** occurrences across multiple
  fresh sessions (previously dozens within seconds). `Timeout on stream`
  and partial-shard-loss messages also stayed at zero, confirming this
  wasn't just moving the symptom around.
- **Not fully resolved**: a visually different corruption still appears on
  the right eye in sustained testing -- structured block-level noise now,
  rather than the earlier uniform green wash or a frozen stale frame,
  and it occurs with zero IDR requests, zero desync warnings, and zero
  partial-shard loss logged, i.e. invisible to every diagnostic used so
  far. The compositor is still genuinely dropping a meaningful fraction of
  frames under load (a real, separate throughput/pacing issue, not a
  correctness bug) -- whether the remaining corruption is downstream of
  that, or a distinct issue, is not yet established. Next step (user's
  call, already planned): a genuine zero-copy encoder input path (see
  `video_encoder_mediacodec.h`'s own `ponytail:` comment), which removes
  the CPU-copy pipeline this whole investigation has been circling, one
  way or another worth doing regardless of whether it turns out to affect
  this remaining issue.

### Follow-up: the green corruption survives everything above — MAX_INPUT_SIZE
### fix confirmed real but NOT the cause; structural trace found nothing;
### MediaCodec's negotiated I/O format matches exactly what we asked for

Picking up the "not fully resolved" note above. In order:

1. **Independent-decode proof the corruption is server-side, not
   network/client.** `WIVRN_DUMP_VIDEO` (a real, pre-existing WiVRn feature
   in `video_encoder.cpp`'s `video_encoder::create()`) was wired up in
   `wivrn_server_jni.cpp`'s `nativeStart()` — Android apps don't inherit
   shell env vars, so this needed an explicit `setenv()` call (still
   present, gated to nothing, always on — cheap, writes only to
   app-private storage). Pulled the dump with `adb ... run-as ... cat`,
   decoded independently on the PC with a static ffmpeg build (the distro
   package's shared libs aren't on this host) — completely bypassing the
   Quest's own decoder and the network. **The corruption is present in
   this PC-side decode**, proving neither the network nor the client
   decoder is the cause.
2. **The installed Quest client is provably not at fault either.** Its
   `lastUpdateTime` was confirmed byte-identical to a fresh install of the
   official `WiVRn-release.apk` — so nothing this session's (incomplete,
   still stuck on a missing `ktx` host tool) client-build attempt could
   have broken is responsible.
3. **Monado's own output, before WiVRn's compositor touches it, is clean.**
   A temporary Vulkan capture (`debug_dump_app_image` in
   `compositor.cpp`, since removed again — see below) dumped the app's
   swapchain image straight from Monado's handoff, both eyes, well past
   the loading splash. Both eyes render correctly. (The user separately
   flagged the two eyes' UI panel not being at the same horizontal
   position as a possible bug; the working theory is normal binocular
   parallax for geometry at finite depth, not a bug — but this has **not**
   been checked against WiVRn's actual per-eye `src_rect`/`src_fov`
   handling, so treat it as plausible, not confirmed.)
4. **`AMEDIAFORMAT_KEY_MAX_INPUT_SIZE` fix — real bug, confirmed fixed, but
   not this bug.** A dispatched review found that without this key,
   Codec2 sizes its input `ByteBuffer` from an internal default that can
   be smaller than one actual NV12 frame (observed: 1MiB/512KiB on this
   device, both under the ~1.23MiB a real 896×960 NV12 frame needs) —
   silently, no error. `encode()`'s existing size guard means such a
   frame would get truncated, and everything past the real buffer size
   would stay zeroed, encoding as solid green. This looked like a
   plausible full explanation. Applied `AMEDIAFORMAT_KEY_MAX_INPUT_SIZE`
   in `ensure_codec()`; verified genuinely active (`grep -c "too small"`
   → 0 across a full session log, where it would previously have fired).
   **Re-tested with a fresh `WIVRN_DUMP_VIDEO` capture immediately after
   confirming zero truncation warnings — the corruption is still there,
   byte-identical in pattern.** So this was a real, independent latent
   bug, worth keeping, but not the (or not the only) cause of the visible
   symptom. Correcting the earlier, premature "this is what every green
   chroma symptom turned out to actually be" claim in this doc's own
   history above — that was wrong; it's now known to be wrong from a
   direct re-test, not just untested optimism.
5. **Manual structural trace, Monado handoff → encoder input, nothing
   found.** Walked `image_formats()` (multi-planar
   `eG8B8R82Plane420Unorm` for 8-bit), `make_images()` (one
   `image_allocation` per compositor image, `arrayLayers=3` for
   left/right/alpha, `usage=eStorage|eTransferSrc`), the queue-family/
   layout-transition barrier at the end of `layer_commit()` (`eGeneral` →
   `encoder->target_layout`, which for mediacodec is also `eGeneral`), and
   `video_encoder_mediacodec.cpp`'s `present_image()` copy-region setup
   (plane0/luma at buffer offset 0 full-res, plane1/chroma at
   `width*height` half-res, both indexed by `stream_idx` as the array
   layer). All structurally correct at the Vulkan-API-call level — no
   coding bug found by reading it.
6. **Tested the "MediaCodec doesn't actually honor the format we
   requested" theory directly (prompted by the classic "greenish video
   encoded with MediaCodec" failure mode, where hardware/Codec2 backends
   silently keep their own padded/tiled layout despite `KEY_STRIDE`/
   `KEY_SLICE_HEIGHT` configure-time hints) — result: negative for this
   device.** Logged `AMediaCodec_getInputFormat()` right after
   `AMediaCodec_start()` (temporary, since removed) and compared against
   what was requested. Live result on-device:
   `requested 896x960 stride=896 slice=960 color=21 -- got w=896 h=960
   stride=896 slice=960 color=21` — an exact match, no silent padding, no
   silently-substituted color format. This specific, well-known MediaCodec
   footgun is **ruled out** on this hardware/codec combination — but it
   only checked what the codec *reports*, not necessarily what the
   underlying Codec2 component *actually does internally* with a
   ByteBuffer input; if this gets re-opened, the next step would be a
   byte-level A/B (feed a synthetic, known-correct NV12 buffer — e.g. a
   flat mid-grey frame — directly into the same encoder call path,
   independent of Monado/the compositor entirely, and decode what comes
   out) rather than trusting the format-negotiation API further.

**Cleanup done this pass**: removed `debug_dump_app_image` and its two call
sites from `compositor.cpp` (it did an unconditional `vk.device.waitIdle()`
per dumped frame for the first 60 frames of every run — a real stall, not
worth leaving in now that the question it was answering is settled), the
`layer_count`/`type0` one-shot debug log next to it, and the
`AMediaCodec_getInputFormat()` diagnostic in `video_encoder_mediacodec.cpp`
(one-shot, its result is now written down above). Left in place, because
they're cheap and still useful for the next round of investigation: the
`WIVRN_DUMP_VIDEO` `setenv()` in `wivrn_server_jni.cpp`, and a
symmetrical receive-side dump in `client/decoder/android/android_decoder.cpp`'s
`push_data()` (writes `dump_recv_<stream_index>.h264`, gated to 600 frames
per stream) — added for an eventual receive-side capture but not yet pulled
from a device, since it requires the client to actually be running code
built by us, which the incomplete client-toolchain build still blocks (see
"Toolchain" section for the `ktx` gap).

**Also kept, unrelated to the corruption investigation but genuinely
fixed**: `foveation.comp` is back to upstream's `subgroupShuffleDown` (the
workgroup-shared-memory replacement documented earlier in this section was
proven to make no difference to this bug — identical corruption either way
— so per explicit instruction it was reverted to the original; it remains
true that WiVRn never checks `VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT`
before relying on it, a real portability gap worth fixing upstream someday,
just not this bug). `video_encoder.h`/`.cpp` gained `push_async(data&&)`
and `video_encoder_mediacodec.cpp` uses it (plus `async_send=true`) so CSD
(SPS/PPS) and the main frame payload both go through the shared background
sender thread instead of blocking the encode-dispatch path — matches
`video_encoder_raw.cpp`/`video_encoder_vulkan.cpp`'s existing pattern, no
regression observed since re-applying it after the IDR-livelock fix made it
safe.

### Root cause found and fixed: a real GPU driver bug, confirmed by direct
### on-device reproduction outside this codebase

Continuing from "where this stands" above. In order:

1. **A full 10-point re-verification against the actual code**, prompted by
   a detailed external checklist (AHardwareBuffer/external-format usage,
   MediaCodec stride/slice-height, direct mapping of an optimal-tiled
   image, image-view/array-layer/aspect-mask correctness, copy-region
   dimensions, barrier aspect masks, synchronization) — every one of these
   was checked directly against the real code (not re-asserted from
   memory) and came back clean. A live `vkQueueWaitIdle()` right after the
   compute dispatch, before any copy, made **zero difference** — same
   corruption, same shape, ruling out synchronization directly rather than
   by inspection.
2. **A minimal, standalone Vulkan program** (`ForeverXR/tools/foveation-pc-test/`,
   disposable, not part of the port) that loads the *exact* unmodified
   `foveation.spv` from the Android build and replays the *exact* real
   captured UBO parameters and source image pulled mid-investigation from
   the Pixel, reproducing the real on-device object shape precisely
   (`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`, `MUTABLE_FORMAT|EXTENDED_USAGE`,
   plane-0/plane-1 views as `R8_UNORM`/`R8G8_UNORM`, matching copy
   regions) — with a `mode` switch to run three configurations:

   | Configuration | NVIDIA (desktop) | Mesa llvmpipe (software) | **Real Pixel** |
   |---|---|---|---|
   | Separate R8 + R8G8 images | clean | clean | **clean** |
   | Multi-planar, 1 array layer | clean | clean | **clean** |
   | Multi-planar, 3 array layers (exact real config) | clean | clean | **corrupted, same signature as production** |

   Cross-compiled with the NDK and run directly on the Pixel via
   `adb push` (no APK needed — a standalone executable, same pattern as
   Milestone 1's early Vulkan-extension probes). This is airtight: same
   shader binary, same real data, on the *same device*, differing only in
   which Vulkan object shape it targets. **Neither multi-planar alone nor
   array layers alone triggers it — only the combination does, and only
   on this GPU.** Two independent desktop implementations (one
   proprietary, one open-source/spec-compliant) stay clean throughout,
   ruling out the shader's math entirely.
3. **Web research** (dispatched in parallel) confirmed the device is a
   **PowerVR D-Series DXT-48-1536** (Tensor G5 — Google switched away from
   ARM Mali starting with this generation) and surfaced independently
   confirmed, currently-unfixed Vulkan compute-shader correctness bugs on
   this exact chip from unrelated projects (llama.cpp's quantization
   shaders producing wrong numbers, PyTorch ExecuTorch's Vulkan backend
   outputting all-zero/NaN textures) — a young, still-buggy driver, and
   this is a new bug in the same family, not a fluke. A structurally
   similar bug class (writing multiple array layers from one compute
   dispatch corrupting all-but-one layer) has precedent elsewhere too:
   [SDL3 GPU issue #12906](https://github.com/libsdl-org/SDL/issues/12906).
   Tested that issue's exact workaround (splitting the single
   `dispatch(...,...,2)` into two `dispatchBase(...,1)` calls against the
   *same* 3-layer image) first, since it's cheaper than restructuring
   images — **result: worse, not better** (the broken eye went from
   partially-correct to solid green with zero detail), confirming the
   array layer itself is the problem, not just how many dispatches touch
   it in one command buffer.
4. **Fix implemented**: `compositor.h`'s `struct image` now holds a
   `stream_image` (own `image_allocation` + Y/CbCr views) per stream —
   `content[2]` (left, right) plus a shared `alpha` — instead of one
   `image_allocation` with `arrayLayers=3`. `make_images()` builds three
   separate single-array-layer multi-planar images per compositor slot
   instead of one three-layer image. `foveation.comp` now receives which
   eye to process via a push constant (`pc.eye`) instead of
   `gl_GlobalInvocationID.z`, and always writes to array layer 0 of
   whichever image is bound (each destination image only has one layer
   now); alpha keeps its existing x-offset packing (both eyes share one
   alpha image, unchanged), just via its own dedicated image with 2 new
   descriptor bindings (4, 5) instead of array layer 2 of the shared
   image. `foveation.cpp`'s `foveate()` now dispatches once per eye
   (`dispatch(gx, gy, 1)` × 2) against its own permanently-allocated
   descriptor set (2 sets total — descriptor set *contents* aren't
   snapshotted at bind-record time, so two dispatches recorded into the
   same command buffer can't safely share one set rewritten in between).
   `video_encoder_mediacodec.cpp`'s `present_image()` copy regions lost
   their `baseArrayLayer = stream_idx` (always 0 now, since each stream's
   image only has one layer).
5. **A real crash found and fixed while first testing this live**:
   `compositor.cpp`'s `beman::inplace_vector<vk::ImageMemoryBarrier2, 3>`
   (a fixed-capacity, non-growing vector) overflowed once the single
   shared pre-dispatch barrier became three (one per stream image) —
   `std::bad_alloc`, `SIGABRT`, confirmed via a full tombstone backtrace
   pointing straight at `inplace_vector::emplace_back` inside
   `layer_commit()`. Fixed by bumping the capacity to 5 (worst case: 1
   squasher-path barrier + 3 per-stream barriers, with margin) — a exact,
   understood fix, not a guess, once the backtrace named the exact
   overflow.
6. **Verified live, repeatedly, on the real headset**: after the crash fix,
   installed and run through the full test cycle — clean on both eyes,
   confirmed by the user directly in-headset ("the tint and artifacts
   problem is solved"), and independently confirmed via a fresh
   `raw_nv12` capture of the previously-broken right eye showing a
   correct, detailed, complex scene (sky gradient, terrain, ground plane)
   with zero green tint, zero corruption — captured from the exact same
   diagnostic path used throughout this investigation, now clean.
7. **A real, expected latency regression from the fix, found and mostly
   addressed the same session**: 2 dispatches instead of 1 means 2
   descriptor sets instead of 1, and `foveate()` was rewriting all 6
   bindings on both sets every single frame — real added per-frame CPU
   cost that showed up as reported pixelation/latency/lag right after the
   fix went in. Root cause: `y`/`cbcr`/`alpha_y`/`alpha_cbcr` (and the ubo
   buffer) only ever take on 2 distinct values for the session's lifetime
   (the compositor's 2 fixed double-buffer slots), alternating every
   *other* frame — only `src` (Monado's own swapchain view) genuinely
   changes every frame. Split the descriptor write into two
   `vkUpdateDescriptorSets` calls per eye: one for `src` (binding 0,
   always), one for the rest (bindings 1-5, only when the
   `(y,cbcr,alpha_y,alpha_cbcr)` tuple actually changed since last time,
   tracked via a small `last_bound` cache in `foveation.h`) — cutting the
   redundant half of the per-frame descriptor-write cost.
8. **Still open, explicitly requested next**: a genuine zero-copy encoder
   input path. The compositor still copies the GPU image to a host buffer
   (`vkCmdCopyImageToBuffer`) every frame, then `memcpy`s that into
   MediaCodec's own input buffer — real, avoidable per-frame cost on top
   of everything above. `video_encoder_mediacodec.h`'s own `ponytail:`
   comment already names the upgrade path:
   `AMediaCodec_createInputSurface()` + rendering directly into it via an
   imported `AHardwareBuffer`, instead of the current GPU-copy-to-host-
   buffer-then-memcpy path. Not started this session — real architecture
   work, not a quick fix.

**Diagnostic tooling kept from this investigation** (outside the port
itself, not shipped): `ForeverXR/tools/foveation-pc-test/foveation_test.c`
— the standalone multi-planar-image reproduction harness described above,
runs on desktop (NVIDIA/lavapipe) or cross-compiled for Android via the
NDK; useful again if a similar array-layer/multi-planar driver bug needs
isolating on different hardware. All the temporary on-device debug dumps
used to chase this (raw NV12 buffer captures, UBO buffer captures, the app
source-image capture, the subgroup-properties log, `WIVRN_DUMP_VIDEO`
wiring, the client's receive-side dump) have been removed now that the
root cause is confirmed and fixed — they were real per-frame overhead
(continuous disk I/O) not worth leaving active in normal operation.
`ForeverXR/tools/vulkan-headers/` (vendored Vulkan-Headers, used to build
the PC test harness without needing `libvulkan-dev`) is kept for reuse.
`ForeverXR/tools/vulkan-validation/android-binaries.tar.gz` (prebuilt
Android Vulkan validation layer binaries from KhronosGroup's GitHub
releases) was downloaded but never actually needed or extracted — the PC
harness's own desktop-side validation-layer runs (`vulkan-validationlayers`,
already installed via apt) caught the real bug (a stale text-format `.spv`
artifact, not the shader) before Android-side validation was ever required.

## Milestone 4.6 — EXHAUSTIVELY INVESTIGATED: no usable public zero-copy
## MediaCodec input path found on this device/firmware. Tried: both
## Surface-input mechanisms (Vulkan WSI, EGL); Block Model + direct
## HardwareBuffer (MediaCodec side genuinely works -- Checkpoint 1); and
## every public GPU-write mechanism into that HardwareBuffer, including
## the two extensions purpose-built for this exact scenario
## (VK_ANDROID_external_format_resolve, GL_EXT_YUV_target) -- both
## present, enabled, correctly used, and both fail at the driver level
## (Checkpoint 2)

`video_encoder_mediacodec.h`'s own `ponytail:` comment named this as the
real upgrade path over the current GPU-copy-to-host-buffer-then-`memcpy`
pipeline: render directly into MediaCodec's own input `Surface` instead.
The server runs on a stock, unrooted Pixel 10 Pro XL — the Quest 1 is only
the remote streaming client — so this investigation deliberately stayed
within **public NDK APIs only**: no `Codec2Client`, no private
`GraphicBufferSource` access, no OMX private/native-buffer extensions, no
vendor encoder libraries, no VNDK-only APIs, no root, no custom ROM.

Investigated both public Surface-input mechanisms via three small
standalone probes (`ForeverXR/tools/foveation-pc-test/`:
`mediacodec_surface_test.c` for Vulkan WSI, `mediacodec_egl_test.c` for
EGL/GLES, `mediacodec_persistent_test.c` for the persistent-surface
variant — all disposable, not part of the port, cross-compiled with the
NDK and run directly on the Pixel):

- **`AMediaCodec_createInputSurface()`**: `configure` → `createInputSurface`
  → `start`, the standard pattern (`ANativeWindow` → a swapchain/EGL
  surface targeting it → render → present → the codec's own `BufferQueue`
  consumer handles the rest).
- **`AMediaCodec_createPersistentInputSurface()` + `AMediaCodec_setInputSurface()`**:
  the other public path (`createPersistentInputSurface` before `configure`,
  independent of any codec instance, then `setInputSurface` after
  `configure`, then `start`).

**Result: no usable public zero-copy MediaCodec input path found on this
device/firmware. Both mechanisms fail identically, precisely
characterized, not a mystery.** In order:

1. Vulkan WSI mechanically works perfectly: `vkCreateAndroidSurfaceKHR`
   succeeds, the queue can present, the surface reports 3 usable RGBA
   formats (device negotiates its own RGB→encoder-native conversion —
   confirmed no manual NV12/YUV handling is needed for this approach),
   `supportedUsageFlags` even includes `STORAGE_BIT`, a swapchain creates
   with 19 images, and up to 40 real frames acquire/render/present
   cleanly with zero stalling (buffers genuinely cycle through the
   `BufferQueue`) — but **zero encoded output ever arrives**, not even
   the usually-immediate `INFO_OUTPUT_FORMAT_CHANGED` event.
2. The EGL/GLES control test (the actually-standard, actually-tested
   Android pattern for this) shows the **identical symptom**: zero output
   events through 10 real presented frames, before eventually blocking on
   a later `eglSwapBuffers` call waiting for a buffer that's never
   released. Since EGL fails identically to Vulkan, this is **not** a
   Vulkan WSI/vendor-driver interoperability gap.
3. Ruled out directly, not assumed: explicit monotonically-increasing
   presentation timestamps via `VK_GOOGLE_display_timing` /
   `eglPresentationTimeANDROID` (both extensions confirmed present) — no
   change. An explicit `request-sync` (force a keyframe) call, matching
   what the real working ByteBuffer backend's `idr_handler` always does
   for frame 0 — no change. `debug.stagefright.c2inputsurface` is already
   `-1` (the framework-side `GraphicBufferSource` path Google's own
   device trees recommend) — nothing to toggle. Only two AVC encoder
   components exist on this device (`c2.google.avc.encoder`,
   `c2.android.avc.encoder`; no distinct vendor hardware component, no
   legacy OMX path at all) — **both show the exact same failure**,
   ruling out a single-component bug.
4. `GraphicBufferSource` (Android's own framework-level Codec2 Surface
   consumer — confirmed by (3) to be the one in use, not a vendor
   implementation) logs `got buffer with new dataSpace ...` **exactly
   once**, ever, then goes completely silent at that log level — no
   further buffer acquisition, no errors. The `BufferQueue` itself has 64
   slots and happily accepts dozens of queued frames from the producer
   side; it's specifically the consumer that stops pulling after buffer #1.
5. **`AMediaCodec_createPersistentInputSurface()` + `AMediaCodec_setInputSurface()`
   fails identically** — same setup, same symptom (frames present cleanly
   through frame 10+, zero output events, eventual producer stall). A
   longer capture on this variant caught the most precise evidence yet,
   one level deeper than `GraphicBufferSource`, straight from the vendor's
   own hardware encoder component's self-reported diagnostics:
   ```
   GC2_EncComp: [0][Id=202] VPU_EncOpen codec AVC succeeded
   GC2_EncComp: wait cmd queue for 0 times, instanceQueueCount(1) interrupted(0) flushing(0)
   GC2_EncComp: wait cmd queue for 5 times, instanceQueueCount(1) interrupted(0) flushing(0)
   GC2_EncComp: wait cmd queue for 10 times, instanceQueueCount(1) interrupted(0) flushing(0)
   GC2_EncComp: wait cmd queue for 15 times, instanceQueueCount(1) interrupted(0) flushing(0)
   ```
   The hardware VPU opens successfully, receives exactly one work item
   into its internal command queue (`instanceQueueCount(1)`), and that one
   item never gets processed — the component periodically self-reports
   this exact stall for the entire run. This is the vendor's own component
   naming its own stuck state; not an inference from absence of output.

**Conclusion for the two Surface-input mechanisms, stated precisely**: on
the tested stock Pixel 10 Pro XL firmware, both fail to encode submitted
frames:
1. `createInputSurface()`
2. `createPersistentInputSurface()` + `setInputSurface()`

Both fail downstream of every public API call succeeding, in the vendor's
own hardware encoder component (`GC2_EncComp`), stuck at
`instanceQueueCount(1)` forever — see the evidence above. This is
deliberately not phrased as "the hardware cannot zero-copy encode", since
that alone doesn't prove it — which is exactly why a third, architecturally
distinct public API was tried next instead of stopping here.

### Update: MediaCodec Block Model + `QueueRequest.setHardwareBuffer()` —
### WORKS for direct HardwareBuffer input (Checkpoint 1 confirmed)

A third public API family exists that bypasses `Surface`/`ANativeWindow`/
`BufferQueue`/`GraphicBufferSource`/EGL/Vulkan-WSI entirely:
`MediaCodec.CONFIGURE_FLAG_USE_BLOCK_MODEL` (requires async
`setCallback()`, confirmed live via
`IllegalStateException: Block model is only valid with callback set`) +
per-buffer `MediaCodec.QueueRequest.setHardwareBuffer(HardwareBuffer)`.
This is Java-only (not exposed via NDK `AMediaCodec`), so the probe
(`ForeverXR/tools/foveation-pc-test/BlockModelTest.java` +
`hwbfill.c`, a small JNI helper using the public
`AHardwareBuffer_fromHardwareBuffer()`/`AHardwareBuffer_lockPlanes()`
bridge to CPU-fill a 2-plane YCbCr `HardwareBuffer`) runs standalone via
`app_process` (no APK needed) rather than as native C.

**Checkpoint 1 (CPU-filled `HardwareBuffer` → `QueueRequest.setHardwareBuffer()`
→ hardware AVC encoder, deliberately not touching Vulkan or the working
ByteBuffer backend): CASE A — WORKS.**

- `HardwareBuffer.isSupported(896x960, YCBCR_420_888, layers=1,
  USAGE_VIDEO_ENCODE|USAGE_CPU_WRITE_OFTEN)` = `true`; allocation succeeds.
- `configure()` with `CONFIGURE_FLAG_USE_BLOCK_MODEL` accepted by
  `c2.google.avc.encoder`.
- All 60 submitted frames queued and **all 60 encoded** — continuous real
  AVC output the entire run (not just the first item, unlike both Surface
  mechanisms), including full-size IDR frames periodically (~210-249
  bytes vs. ~38 bytes for P-frames of this flat-color test content).
- `onOutputFormatChanged` fires immediately with real `csd-0`/`csd-1`
  (SPS/PPS) — something neither Surface mechanism ever produced.
- Clean EOS (`BUFFER_FLAG_END_OF_STREAM` observed, `done` latch released).
- **Logcat confirms no stall**: `GC2_EncComp: [0][Id=206] VPU_EncOpen
  codec AVC succeeded` followed by continuous encoding and a clean
  `VPU_EncClose succeeded` at teardown — **the
  `wait cmd queue for N times, instanceQueueCount(1)` self-diagnostic
  that characterized both Surface-mode failures never appears.** This is
  the vendor's own component confirming it received and processed many
  work items, not one.
- One implementation gotcha worth recording: block-model output is
  **not** exposed via `getOutputBuffer()` — that throws
  `IncompatibleWithBlockModelException`; must use
  `getOutputFrame(index).getLinearBlock().map()` instead. Also,
  `app_process`'s main thread has no `Looper` prepared by default and
  `MediaCodec.setCallback()` needs one on the *calling* thread internally
  regardless of the explicit `Handler` argument passed — fixed with an
  explicit `Looper.prepare()` before any MediaCodec work (an artifact of
  the `app_process` test harness, not of the API itself; a real Android
  Service/Activity thread already has one).

**This was not yet zero-copy for the real pipeline** — the compositor's
frames live in a Vulkan `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` image, not a
CPU-filled buffer.

### Checkpoint 2: AHardwareBuffer ↔ Vulkan bridging — FAILS on this
### device/driver (not a MediaCodec-side problem this time)

Three standalone native probes (no Java, no MediaCodec —
`ForeverXR/tools/foveation-pc-test/vk_ahb_probe.c`,
`vk_ahb_export_test.c`, `vk_ahb_import_test.c`), run directly on-device:

1. **`vk_ahb_probe.c`** — allocated the exact `YCBCR_420_888`
   `AHardwareBuffer` shape Checkpoint 1 used (896x960, layers=1,
   `USAGE_VIDEO_ENCODE`), plus a GPU usage bit
   (`GPU_DATA_BUFFER`/`GPU_SAMPLED_IMAGE`/`GPU_COLOR_OUTPUT` each tried
   individually — `AHardwareBuffer_isSupported()` reports **YES** for
   all four), then imported it into Vulkan and called
   `vkGetAndroidHardwareBufferPropertiesANDROID()`. **Result: CASE B —
   `format=VK_FORMAT_UNDEFINED`, `externalFormat=0x301`.** This Pixel's
   PowerVR gralloc allocator never exposes `YCBCR_420_888` to Vulkan as
   the concrete `G8_B8R8_2PLANE_420_UNORM` format the compositor uses
   internally — it's always an opaque external format.
   `formatFeatures=0xbad081`: `STORAGE=0`, `TRANSFER_SRC=1`,
   `TRANSFER_DST=1`, `SAMPLED=1`. Independently, a plain (non-AHB)
   capability query (`vkGetPhysicalDeviceImageFormatProperties2` with the
   AHB-external chain) for the concrete format with `STORAGE` usage also
   comes back degenerate (`VK_SUCCESS` but `maxExtent=0x0`) — confirms
   `STORAGE` is unsupported for AHB interop on this format in both
   directions, not just an import-side quirk.
2. **`vk_ahb_export_test.c`** (Step 15's "Vulkan-first" alternative —
   create the concrete-format image ourselves, export its memory as an
   `AHardwareBuffer` via `vkGetMemoryAndroidHardwareBufferANDROID()`,
   sidestepping the opaque-format problem entirely): `vkCreateImage`
   succeeds, `vkGetImageMemoryRequirements2` succeeds — but the actual
   `vkAllocateMemory(export, dedicated)` call **fails with
   `VK_ERROR_OUT_OF_DEVICE_MEMORY`**, despite the prior capability query
   (`vkGetPhysicalDeviceImageFormatProperties2` for `TRANSFER_DST`-only
   usage) reporting the combination supported with real, non-degenerate
   limits. A genuine capability-query-vs-real-driver-behavior mismatch —
   same character as the array-layer bug found in Milestone 4.5 (the
   driver's own queries aren't trustworthy in isolation on this
   hardware).
3. **`vk_ahb_import_test.c`** — re-confirmed the CASE B external format
   from probe 1, then actually created a `VkImage` for it
   (`VK_FORMAT_UNDEFINED` + `VkExternalFormatANDROID`, usage
   `TRANSFER_DST_BIT`) and imported the same AHB as dedicated memory.
   **`vkCreateImage`/`vkAllocateMemory`/`vkBindImageMemory` all fully
   succeed** — so the AHB genuinely is a valid Vulkan import target,
   just an opaque one.

**Why no write path exists, even with `TRANSFER_DST` "supported"** (this
part is spec reasoning, not a live-tested result — see below): an image
with a non-zero `externalFormat` has no Vulkan-visible texel size or
layout, so a `vkCmdCopyImage` region's extent is only meaningful when
*both* sides of the copy share the identical external format (this is
why the extension's compatibility rules require it — the driver blits
between two buffers of its own private layout without either side's
texel geometry being expressible to Vulkan). Our compositor's real
source image is a normal concrete `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`
image, not `externalFormat=0x301`, so this copy has no legal encoding.
And a same-external-format source is unobtainable any other way: the
only way to get `externalFormat=0x301` memory is another
`YCBCR_420_888` AHB allocation — which is subject to the exact same
`STORAGE=0` constraint probe 1 found, so *nothing* can populate it via
Vulkan. **This reasoning was not verified with a live illegal copy
attempt** — deliberately, since it would need to run without validation
layers (not installed on this stock, unrooted device) and risks an
unrecoverable device-side hang for close to zero additional information,
given two independent *empirical* failures (probe 1's `STORAGE=0`, probe
2's real `vkAllocateMemory` failure) already point to the same
conclusion through different mechanisms. Flagged here explicitly as
reasoned-not-tested in case it's worth the risk later.

Also checked (Step 16): the public NDK header
(`android/hardware_buffer.h`) has no `AHARDWAREBUFFER_FORMAT_PRIVATE`
constant — only named formats (`Y8Cb8Cr8_420`, `BLOB`, RGBA variants,
`YCbCr_P010`/`P210`, etc.) and `BLOB` (a linear byte buffer, not a 2D
image — not usable for `QueueRequest.setHardwareBuffer()`'s image input
regardless). Not actionable within this project's public-API-only
constraint; not pursued further.

### Update: PATH A (`VK_ANDROID_external_format_resolve`) and PATH B
### (`GL_EXT_YUV_target`) — the two extensions purpose-built for exactly
### this scenario — both tried, both fail at the driver level

The `STORAGE=0`/export-OOM findings above don't end the investigation on
their own: Android and Khronos ship two extensions specifically designed
to render into an opaque external-format YUV `AHardwareBuffer` without
`STORAGE`/plain `TRANSFER` — `VK_ANDROID_external_format_resolve`
(Vulkan) and `GL_EXT_YUV_target` (GLES). Both were tried in full,
end-to-end, before concluding anything.

**PATH A — `VK_ANDROID_external_format_resolve`** (probes
`vk_ext_format_resolve_probe.c`, `vkyuvresolve.c` + `YuvResolveTest.java`):
extension present, `externalFormatResolve` feature `VK_TRUE`,
`colorAttachmentFormat=37` (`VK_FORMAT_R8G8B8A8_UNORM`, renderable) for
the exact `externalFormat=0x301` Checkpoint 1's buffer uses. Built the
full render+resolve pipeline (tried both the `nullColorAttachmentWithExternalFormatResolve`
implicit-attachment form and an explicit real RGBA attachment +
`resolveImageView` form). **Every Vulkan call succeeds**
(`vkCreateImage`/`vkAllocateMemory`/`vkBindImageMemory`/`vkCreateImageView`/
`vkQueueSubmit`/`vkWaitForFences`, all 60 frames, `VK_SUCCESS`
throughout), and MediaCodec produces 60 real encoded AVC frames — but
**the write is a silent no-op**, proven three independent ways: (1)
swapping the fragment shader three times (quadrants → gradient →
magenta) produces byte-for-byte identical (same MD5) `.h264` output
regardless of shader content; (2) the shader itself is independently
proven correct — the identical shader rendering into a real,
CPU-readable RGBA image (no AHB, no resolve) gives exactly the right
per-quadrant RGB values via `vkCmdCopyImageToBuffer` readback; (3) a
diagnostic buffer with added CPU-read usage, rendered with solid
magenta, then `AHardwareBuffer_lockPlanes()`'d directly: **Y=0 at every
sample point**. A genuine driver-side silent failure — this extension is
`SPEC_VERSION=1`, brand new — not an application bug.

**PATH B — `GL_EXT_YUV_target`** (probes `egl_gles_ext_probe.c`,
`glyuvtarget.c` + `GlYuvTargetTest.java`): all required extensions
present (`GL_EXT_YUV_target`, `GL_OES_EGL_image_external[_essl3]`,
`GL_OES_EGL_image`, `EGL_ANDROID_get_native_client_buffer`,
`EGL_ANDROID_image_native_buffer`, `EGL_KHR_image_base`). The AHB
bridges to an `EGLImage` cleanly (`eglGetNativeClientBufferANDROID` +
`eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)`, both `EGL_SUCCESS`). But
**it can never be attached to an FBO as a write target**: tried both
valid OES binding mechanisms — `GL_TEXTURE_EXTERNAL_OES` via
`glEGLImageTargetTexture2DOES` (+ `glFramebufferTexture`, since
`glFramebufferTexture2D`'s `textarget` param doesn't accept
`EXTERNAL_OES`), and `GL_RENDERBUFFER` via
`glEGLImageTargetRenderbufferStorageOES` (+ `glFramebufferRenderbuffer`,
the mechanism `GL_OES_EGL_image` actually defines for write targets) —
also tried binding the `layout(yuv)` program before the completeness
check in case of ordering sensitivity. **Every variant gives the
identical result**: zero GL errors at any step, but
`glCheckFramebufferStatus` returns exactly `0` — not
`GL_FRAMEBUFFER_COMPLETE` (`0x8CD5`) nor any other spec-defined status
enum (all are non-zero). A genuine driver contract violation, not a
binding-mechanism mistake (two independently-correct mechanisms fail
identically).

**Secondary (Step 16, PRIVATE/implementation-defined format)**:
confirmed on both surfaces — no `PRIVATE` constant exists in
`android/hardware_buffer.h` (native) nor in `android.hardware.HardwareBuffer`'s
public constants (`javap`-verified: only named formats). Not obtainable
through any public API.

**Conclusion for Checkpoint 2, precisely**: on this Pixel 10 Pro XL,
MediaCodec's block-model `HardwareBuffer` input (Checkpoint 1) is **not**
the blocker — it works perfectly. Every public GPU-write mechanism into
a MediaCodec-accepted YUV `HardwareBuffer` has now been tried: Vulkan
`STORAGE` (unsupported), Vulkan `TRANSFER`-copy from a concrete source
(spec-illegal, reasoned not live-tested), Vulkan-first export
(`vkAllocateMemory` fails despite passing its capability query),
`VK_ANDROID_external_format_resolve` (every call succeeds, write
silently discarded), `GL_EXT_YUV_target` (framebuffer never becomes
usable, no error given), and PRIVATE format (not reachable via public
API). The two extensions *specifically built* for this exact scenario
are both present, enabled, and used correctly by multiple
independently-verified implementation variants — and both fail at the
driver level. The only way found to legally put pixel content into any
AHardwareBuffer MediaCodec's block model accepts, on this device, is a
CPU lock (`AHardwareBuffer_lockPlanes()` — exactly what Checkpoint 1's
`hwbfill.c` already does). Combined with the Surface-input findings:
**no usable public zero-copy MediaCodec input path has been found on
this device/firmware** — still deliberately not phrased as "impossible
on this hardware," since every failure has a specific, distinct, named
cause (mostly real empirical driver failures, one spec-reasoned) rather
than a single proven hardware ceiling. If revisited on different
hardware/firmware, all probes in `ForeverXR/tools/foveation-pc-test/`
are directly reusable — re-run them first.

Returning to optimizing the working ByteBuffer/NV12 path is the
concrete next step (the pixelation reported after Milestone 4.5's
latency fix is still open and likely a separate, more tractable issue —
bitrate or frame-pacing related).

## Milestone 5 (in progress, branch `perf/pipelined-mediacodec`) — readback
## pipelining + the remaining pixelation/quality investigation

Branched from `f2a46ada` (the Pixel PowerVR investigation doc commit).
Goal: (1) pipeline the GPU-copy → CPU-memcpy → MediaCodec readback path to
reduce stalls, (2) root-cause the visible pixelation reported after
Milestone 4.5. Do NOT reopen the zero-copy investigation for this work —
it's closed (see `docs/pixel10-pro-xl-gpu-media-investigation.md`).

**Instrumentation added** (`d654c757`): `server/utils/frame_timing_stats.h`/`.cpp`,
a lightweight opt-in (`WIVRN_TIMING_LOG` env var) rolling avg/p50/p95/max
logcat logger, wired into every real wait/copy boundary in
`video_encoder_mediacodec.cpp`'s `present_image()`/`encode()`, plus the
compositor's existing GPU query-pool compute timing. Baseline/after
numeric comparison not yet collected — the investigation below took
priority once it surfaced a real, decisive finding.

**Real bug found and fixed as a direct result of that instrumentation**
(`bd666aa6`): the mediacodec staging buffer is `HOST_VISIBLE` but **not
`HOST_COHERENT`** on this driver (confirmed live via a new one-time log at
encoder construction: `mediacodec[N] staging buffer is NOT HOST_COHERENT`)
— meaning the GPU's `vkCmdCopyImageToBuffer` write was not guaranteed
visible to the CPU's `memcpy` read in `encode()` without an explicit
invalidate, which was missing. Added `basic_allocation::invalidate()`
(`common/vk/allocation.h`/`.cpp`, wrapping `vmaInvalidateAllocation`,
shared by desktop and Android) and call it before the read. **This did
NOT eliminate the visible pixelation** — confirmed live, before and after
this fix, so it is a real, independent correctness gap, not the (or not
the only) cause of the reported quality issue.

### The pixelation is confirmed encoder-side, not network — a major, decisive finding

Per the investigation's own priority order (prove local-stream cleanliness
*before* touching bitrate/network), `WIVRN_DUMP_VIDEO` (pre-existing
WiVRn feature, temporarily wired into `wivrn_server_jni.cpp`'s
`nativeStart()` again for this one capture, same pattern as commit
`89655cd4`, reverted once done) captured the exact H.264 bytes the server
sent, for both eyes, during a real live session (Quest 1 connected, real
Unity OpenXR app driving it, `XR_SESSION_STATE_FOCUSED` reached). Decoded
**independently on the PC** (a separate static `ffmpeg` build — no
network, no Quest, no client decoder involved at all):

- The vast majority of sampled frames, both streams, are completely
  clean.
- **Scattered, transient block-level corruption (near-black macroblocks)
  does appear in the raw, pre-network, server-side encoded bitstream
  itself**, confirmed by direct visual inspection of independently-decoded
  frames. Re-sampling the identical frame-index range minutes apart
  showed it present at one sampling and absent at another — genuinely
  intermittent/transient, not deterministic per frame index.
- The user separately confirmed, live in-headset, seeing the same kind of
  artifact on **both** eyes (this investigation's own sampling had, by
  chance, only directly caught it on stream 1/right eye at first — the
  user's live observation is the more reliable signal on which streams
  are affected).
- `GC2_EncComp` (the vendor hardware encoder component) logged zero
  stall/error/warning messages correlating with the corrupted frames in
  the captured window — opened cleanly (`VPU_EncOpen ... succeeded`) for
  both streams' hardware channels, no `wait cmd queue`-style distress
  signal like Milestone 4.6's Surface-input investigation found. One
  minor asymmetry noted but not yet explored: the second stream's
  `ECOServiceStatsProvider` registration logged `Failed to add stats
  provider` at encoder-open time (immediately after the first stream's
  identical registration succeeded) — possibly benign (a duplicate-key
  collision on a telemetry hook, not necessarily related to the video
  path at all), flagged for follow-up, not yet investigated further.

**Conclusion, precisely**: this is **not** a network, packetization, or
Quest-side decoder problem — the corruption is provably already present
in the bytes the server itself produced, before anything is sent.
Bitrate tuning, rate-control-mode changes, and packet/network analysis
(Parts K/L/M/R of the original investigation plan) are **not** the right
next step until this is root-caused; per the plan's own explicit
instruction, they were deliberately not pursued yet. This also is **not**
the already-fixed Milestone 4.5 array-layer driver bug (fixed and
confirmed clean separately) and **not** eliminated by the HOST_COHERENT
fix above (both already ruled out by direct live testing).

**This matches a previously-noted, never-root-caused issue from earlier
in this project** — commit `33895ed6`'s own message: *"A separate,
differently-shaped corruption (structured block-level noise, not the
same visual signature as before) still appears on the right eye under
sustained load, with zero IDR/desync/partial-loss events logged — not
yet root-caused."* That note predates even the Milestone 4.5 array-layer
investigation and its fix. This strongly suggests it's the **same
long-standing, still-unresolved issue**, not something newly introduced
by any change made this session.

**Not yet determined at that point**: exact root mechanism, whether both
streams are equally affected, whether MediaCodec/hardware encode itself
was responsible or was just faithfully encoding already-bad input.

### Decisive follow-up: the NV12 input is already corrupt before MediaCodec ever sees it

Per the plan's own priority order — *"For one visibly corrupted encoded
frame, prove whether its immediately preceding NV12 input frame was
clean or corrupt. That one answer determines which half of the remaining
pipeline to spend time on"* — a proper, permanent, committed diagnostic
was added (commit `b7dfca02`): `WIVRN_DUMP_NV12` (mirroring the existing
`WIVRN_DUMP_VIDEO`) continuously captures the exact bytes handed to
`AMediaCodec_queueInputBuffer()`, each frame prefixed with its 8-byte
`frame_index`, toggled at runtime via the `debug.wivrn.dump` Android
system property (`adb shell setprop debug.wivrn.dump 1`) — no
rebuild/reinstall needed, unset by default.

Method: a fresh live session (Quest 1, real Unity OpenXR app,
`XR_SESSION_STATE_FOCUSED`) ran with the flag set. The encoded-output
capture (stream 1/right eye, ~183 MB, 18350 frames) was decoded
independently on the PC — ffmpeg itself logged a hard bitstream syntax
error partway through (`cabac_init_idc 32 overflow` — a value with only
3 legal values, 0-2 — `decode_slice_header error`, `Invalid data found
when processing input`), confirming this is genuinely malformed H.264
syntax, not merely an ugly-but-valid encode. A per-frame near-black-pixel
scan flagged **1496 of 18350 frames (~8.2%)**, clustered in dense bursts
(consistent with P-frame error propagation from one bad frame forward
until the next IDR resets it) — including one **isolated** single-frame
glitch at output position 1215 (0-indexed), chosen specifically because
it isn't part of a multi-hundred-frame inherited-corruption cascade.

The exact corresponding record was pulled from the frozen, byte-tagged
`dump_nv12-1.nv12raw` (23.8 GB total; only the ~27 MB, 21-frame window
around that position was pulled, via an on-device `dd` producing a
frame-exact byte range — no need to transfer the full capture) and
decoded directly as raw Y-plane bytes (no H.264 involved at all for this
side). **Result: `frame_index=2995`'s raw NV12 Y-plane already shows
8.77% near-black pixels — matching almost exactly the 8.77% black
fraction of the corresponding corrupted encoded output frame — in the
identical scattered/streaked spatial pattern.**

**Conclusion, precisely: the corruption is already present in the NV12
buffer at the moment it is copied into MediaCodec's input buffer.
MediaCodec and the hardware encoder are exonerated as the source** — they
are faithfully encoding already-corrupt input, not introducing the
corruption themselves. This redirects the investigation to the GPU
render/readback/synchronization path specifically:
`compositor.cpp`/`foveation.comp`'s RGB→NV12 compute, the
`vkCmdCopyImageToBuffer` readback in `present_image()`, and the fence/
memory-visibility handling around it (the `HOST_COHERENT` invalidate fix
above was real and necessary, but demonstrably not sufficient — the
corruption persists with it in place).

### Isolation result: concurrent operation of both streams is a major amplifying factor

Per the plan's "run one eye / one encoder only" step, a committed
diagnostic toggle was added (commit `6531aec2`): `WIVRN_ONLY_STREAM`
(`-1` default = both streams run normally; `0`/`1` = only that
`stream_idx` gets `present_image()`/`encode()` calls at all — the other
encoder object exists but is never fed a frame or given a worker
thread). **On Android, this specific mechanism (Monado's
`DEBUG_GET_ONCE_NUM_OPTION`) reads an Android system property directly
— `debug.xrt.WIVRN_ONLY_STREAM` — bypassing `getenv()`/env vars
entirely**, confirmed by reading `u_debug.c`'s Android-specific
`get_option_raw()`; this is different from `WIVRN_DUMP_VIDEO`/
`WIVRN_DUMP_NV12`/`WIVRN_TIMING_LOG` (plain `getenv()`-based, genuinely
need the env-var forwarding `apply_debug_dump_property()` provides). A
`debug.wivrn.only_stream`-to-`WIVRN_ONLY_STREAM`-env-var forwarding path
was added first, based on the incorrect assumption both mechanisms
worked the same way, and confirmed inert (stream 0 kept producing output
despite being "disabled") before this was traced down — worth knowing if
touching this code again: **set `debug.xrt.WIVRN_ONLY_STREAM` directly
on Android**, don't rely on the env-var forwarding for options read via
Monado's own `DEBUG_GET_ONCE_*` macros.

Three ~45-second live sessions, same method as above (WIVRN_DUMP_VIDEO
capture, independent PC decode, per-frame near-black-pixel scan
excluding the initial loading-splash frames):

| Configuration | Frames | Flagged corrupted | Rate |
|---|---|---|---|
| Left eye (stream 0) alone | 3108 | 15 | **0.48%** |
| Right eye (stream 1) alone | 3276 | ~53 | **~1.6%** |
| Both eyes concurrent (earlier capture) | 18339 | 1496 | **~8.2%** |

**Conclusion**: running both streams concurrently produces roughly
**4-15× more corruption** than either stream running alone — decisive
evidence that concurrent per-stream operation (GPU copy submission
and/or `encode()`'s hardware-encoder interaction) is a major amplifying
factor, not merely a random transient VPU hiccup independent of load.
A smaller baseline of corruption exists even with a single stream
running alone, and right eye alone is consistently ~3× worse than left
eye alone (matching the historical note's specific call-out of the
right eye) — suggesting **two separate, stackable contributing causes**:
some inherent stream-0-vs-1 asymmetry, and a much larger
concurrency-related contribution.

### Follow-up: serializing encode() makes it WORSE, not better — points away from CPU-thread concurrency

Per the plan's step 4, a second toggle (`WIVRN_SERIALIZE_ENCODE`, commit
`19c3b6d7`) leaves GPU work for both streams completely unchanged (still
submitted together every frame, same as normal) and only serializes
`encode()`'s `std::jthread` workers back to sequential calls on
`encoder_work()`'s own thread — isolating the CPU-thread-concurrency
variable specifically from the GPU-submission-cadence variable that
`WIVRN_ONLY_STREAM` couldn't separate.

| Configuration | Left | Right |
|---|---|---|
| Left alone | 0.48% | — |
| Right alone | — | ~1.6% |
| Both, normal concurrent `encode()` | ~8.2%* | ~8.2%* |
| Both, **serialized** `encode()` | 2.98% | **19.21%** |

(*both streams pooled in the earlier concurrent-baseline sample, not
measured per-stream separately at the time)

**Result: serializing made the right stream's corruption more than 2×
worse, not better.** This argues *against* the concurrent per-stream
`encode()` worker threads (`89655cd4`) being the direct cause — if a
CPU-side data race between the two worker threads were responsible,
removing that concurrency should have helped, not hurt. Instead, this
points toward a **GPU-submission-cadence / buffer-reuse-timing race**:
the render thread submits both streams' GPU compute+copy work
unconditionally every single frame regardless of how fast the CPU side
is actually consuming it (`present_image()`'s own per-slot fence wait
only guards against reusing a slot that's still *in flight*, not against
how much *slower* than the GPU's cadence the CPU has fallen). Serializing
`encode()` makes each stream's CPU consumption strictly slower (stream 0
must fully finish, including its own hardware-encoder turnaround, before
stream 1's call even starts) — apparently *widening*, not closing, the
window for a stale-buffer/fence race against the next frame's GPU copy.

**Working hypothesis going forward**: this is likely tied to
`num_slots=2`'s limited headroom (the base `video_encoder` class's
present/encode ping-pong, shared by every backend — see "Part A" earlier
in this milestone) combined with this device's real per-frame CPU+GPU
turnaround sometimes exceeding what 2 slots of headroom can absorb,
rather than a straightforward thread-safety bug in the concurrent
`encode()` design itself.

### Decisive negative result: maximal forced synchronization does NOT fix it either

A third toggle (`WIVRN_FORCE_GPU_WAIT`, commit `04b79152`) makes the
render thread fully block in `present_image()` until that exact frame's
GPU copy is 100% complete before returning at all — the most aggressive
synchronization possible short of a global `vkDeviceWaitIdle()`,
eliminating any possibility of the render thread moving on to other GPU
work (the next frame's compute dispatch, the other stream's copy) while
a copy is still in flight.

| Configuration | Left | Right |
|---|---|---|
| Left alone | 0.48% | — |
| Right alone | — | ~1.6% |
| Both, normal concurrent | ~8.2% (pooled) | ~8.2% (pooled) |
| Both, serialized `encode()` | 2.98% | 19.21% |
| Both, **forced full GPU wait** | 1.40% | **11.36%** |

**Result: forced maximal synchronization did not eliminate or clearly
reduce the corruption.** This is the decisive negative result that rules
out "just needs more/better waiting" as the fix — if this were a plain
timing race closeable with sufficient synchronization, the most
conservative possible wait should have fixed it or driven it down near
the single-stream baseline. It didn't.

**Conclusion, updated and more precise**: across every configuration
tested, the one thing that consistently and dramatically correlates with
low corruption is simply *whether only one stream is doing real GPU+encode
work at all* (0.5-1.6% alone vs. 8-19% with both active, regardless of
how the two streams' work is ordered/synchronized/serialized relative to
each other). More synchronization didn't help; less overlap (serializing)
made the worse stream's corruption *increase*, not decrease. This pattern
fits **genuine resource contention between two concurrent real-time
GPU-compute + hardware-video-encode pipelines on this SoC** far better
than an application-level synchronization bug fixable with a wait or a
mutex — it looks like a platform/driver-level limitation when both
streams are actively rendering+encoding around the same time, not a
closeable race window in this project's own code. Not yet proven beyond
this pattern-matching, but this is now the leading hypothesis, and it
would put this bug back in the same general territory as the earlier
array-layer driver bug: a real hardware/driver characteristic to work
around, not a logic bug to fix outright.

**Next steps** (not yet started): if resource contention under
concurrent load is correct, a *pacing/scheduling* fix (spreading the two
streams' GPU work and hardware-encoder submissions further apart in time
within each frame period, rather than back-to-back on the render thread)
is a more promising direction than any additional synchronization
primitive — this is a different kind of change from anything tried so
far. Also worth doing before any code change: correlate corrupted frames
against the existing `frame_timing_stats` GPU fence-wait data and the one
`ECOServiceStatsProvider` asymmetry noted earlier, to see whether
corruption clusters around moments of measurably higher GPU/VPU load.
Bitrate/rate-control tuning and network/packet analysis remain
explicitly out of scope until this is resolved — confirmed multiple
times now (network ruled out by the encoder-side capture; MediaCodec/
encoder ruled out by the NV12 capture; CPU-thread concurrency argued
against; plain synchronization gaps now argued against too) that none of
those are the cause.

The full pipelined-readback-ring rewrite (Parts C-H of the original
readback-pipelining plan) remains paused — correctly, per the decision
to avoid changing pipeline timing before the root cause here is found,
since a concurrency change could mask or alter this exact bug.

**Follow-up: a fixed stagger between the two streams also does NOT fix
it** (`f2576aeb`, `WIVRN_STAGGER_US=3000` — the render thread sleeps 3ms
between the first and second stream's `present_image()`/encode-worker
start each frame):

| Configuration | Left | Right |
|---|---|---|
| Both, normal concurrent (baseline) | ~0.5-3% (varies by run) | ~8-11% |
| Both, 3ms stagger | **7.97%** | 11.60% |

Left got *worse* under staggering (7.97% vs. its usual low single digits),
right stayed about the same. This rules out simple temporal staggering
(at this value) as a fix too, and is consistent with the resource-
contention hypothesis above rather than a fixable ordering/timing issue:
spreading the two streams' work out in time didn't reduce contention, it
just shifted which stream's work landed in the more/less loaded window.

**`VK_EXT_host_image_copy` experiment (`7eb2c715`) — inconclusive, not
recommended to pursue further without a dedicated session.** Rationale:
this replaces the queue-submitted `vkCmdCopyImageToBuffer()` readback
with `vkCopyImageToMemory()` (core in Vulkan 1.4, confirmed present and
correctly reporting non-degenerate limits for this exact image shape via
a standalone probe — see the toolchain table above), which runs
synchronously on the host and never touches GPU queue scheduling at all —
mechanistically different from every synchronization-based experiment
above. Behind `WIVRN_HOST_IMAGE_COPY` (default 0, unchanged behavior).

What actually happened testing it, in order:

1. First live attempt: the server process SIGSEGV'd on the very first
   frame inside `video_encoder_mediacodec::present_image()` (fault addr
   `0x70`, null-pointer-shaped), killing `MonadoIpcService` and dropping
   the Quest connection ("connection refused" is just what a dead
   listener looks like from the client side).
2. Investigating the crash, the code appeared to reference Vulkan struct
   names (`vk::CopyImageToMemoryInfo`, `vk::ImageToMemoryCopy`,
   `Device::copyImageToMemory` — the core, non-`EXT`-suffixed names) that
   don't exist in `common/CMakeLists.txt`'s own separately-fetched
   `Vulkan-Headers-vulkan-sdk-1.3.268.0` (pre-dates Vulkan 1.4). This led
   to an incorrect diagnosis that a stray Vulkan-Headers install was
   accidentally shadowing the pinned SDK, and — **mistakenly** —
   `ForeverXR/tools/vulkan-headers/` (Vulkan-Headers 1.4.328) got deleted
   to "fix" it.
3. That directory is not stray: it's documented, load-bearing toolchain
   infrastructure (see the toolchain table above and the entry right
   after Milestone 4's array-layer investigation) — upstream WiVRn's own
   `CMakeLists.txt` requires `Vulkan_VERSION >= 1.4.304` to build
   `wivrn-server` at all, and `server-app/build.gradle` passes
   `-DVulkan_INCLUDE_DIR=.../tools/vulkan-headers/include` specifically
   because the NDK's own bundled headers report too old a version. The
   core Vulkan 1.4 names in the diagnostic code were correct all along.
   Deleting it broke the *entire* server build (`CMake Error: Vulkan
   version must be at least 1.4.304, found`); re-downloaded
   Vulkan-Headers v1.4.328 from upstream to restore it, and reverted the
   EXT-suffixed rewrite back to the original core-name code once the real
   toolchain was back in place.
4. With the real toolchain restored (no code logic change from the
   version that crashed), a second live attempt ran for the tested window
   (~15s+) **without the SIGSEGV recurring** — but the Unity/Quest session
   never reached `XR_SESSION_STATE_FOCUSED` on this run (a separate
   connectivity issue, not a crash — server process stayed alive
   throughout), so **no corruption-rate data was actually collected**
   before the session ended for the day.

**Status: unresolved.** The one confirmed crash is not safely attributable
to `vkCopyImageToMemory()` itself — it happened while the build was
(unknowingly, at the time) still using the correct headers, so the
SIGSEGV's real cause is still unknown; it could be a genuine driver bug
in this PowerVR driver's host-image-copy implementation (this device's
driver has already shown multiple other Vulkan-capability-reporting bugs
in this investigation — see `docs/pixel10-pro-xl-gpu-media-investigation.md`),
or something in this diagnostic code itself. `WIVRN_HOST_IMAGE_COPY`
defaults to 0 (off); the code is committed and buildable but **should not
be re-enabled for a real test without first adding a defensive check**
(e.g. confirm the resolved `vkCopyImageToMemory`/`vkCopyImageToMemoryEXT`
function pointer is non-null before calling it, and/or run the very first
frame under close, immediate crash-log watching rather than a long
unattended capture window) — do not repeat the un-guarded retry pattern.

**Session conclusion for today**: five mechanistically distinct
mitigation attempts (plain concurrency baseline, serialize `encode()`,
force full GPU-copy sync, 3ms stagger, host-image-copy readback) have
been tried. Four are clean negative results; the fifth is inconclusive
due to an unrelated build mistake consuming the retest window. The
resource-contention hypothesis (two concurrent real-time GPU-compute +
hardware-encode pipelines competing for the same SoC resources) remains
the leading explanation and has not been contradicted by anything tried
so far. Next session should either (a) retry host-image-copy once more
with the defensive check above and a short, closely-watched first frame,
or (b) if that's not worth the risk, move to the pacing/scheduling
direction already proposed, or accept resource contention as a
documented platform limitation similar to the closed zero-copy
investigation.

### HEVC A/B diagnostic: is the black-block corruption AVC-specific?

Added a real, permanent HEVC codepath to `video_encoder_mediacodec` (not a
throwaway branch) to test whether the still-open black-block corruption
above is specific to the AVC bitstream/encoder or common to the whole
input/VPU path. Selectable via the existing, previously-inert-on-Android
`codec` config key (see below) — no new config mechanism needed, since
`common/wivrn_packets.h`, `client/decoder/android/android_decoder.cpp`, and
`server/encoder/encoder_settings.cpp`'s `select_encoder()` already had full
`h265` plumbing end-to-end; the only gate was
`video_encoder_mediacodec.cpp`'s hardcoded AVC-only check and MIME string.

**Real hardware HEVC component identified**: `c2.google.hevc.encoder`
(hardware=true, vendor=true — confirmed via a standalone `MediaCodecInfo`
enumeration probe, `tools/foveation-pc-test/MediaCodecEnum.java`, same
non-tracked-tool convention as this doc's other probes). The software
fallback, `c2.android.hevc.encoder`, is capped at 512×512 and obviously
unusable. `ensure_codec()` now selects the hardware component explicitly by
name (`AMediaCodec_createCodecByName`) rather than
`AMediaCodec_createEncoderByType`, so it can never silently fall back to
software. HEVC is pinned to 8-bit Main profile explicitly (the component
also advertises Main10/HDR10/HDR10+, which would defeat a one-variable
comparison if left to pick its own default).

**Two real, independent bugs found and fixed as a direct result of trying
to actually run this A/B test** (neither is HEVC-specific in nature, both
were just never hit before because nothing had ever selected a non-h264
codec on Android):

1. **`bit_depth` defaulted to 10 for any non-h264/raw codec**
   (`encoder_settings.cpp`), because that default assumes a vaapi/nvenc-class
   backend where h265 genuinely can do 10-bit. `video_encoder_mediacodec`
   is 8-bit-only regardless of codec (its own constructor already asserted
   this for h264). Selecting `mediacodec`+`h265` with no explicit
   `bit_depth` fell through to the 10-bit default and threw before a
   session could even be created. Fixed by also checking for the
   `mediacodec` encoder name in the same `bit_depth = 8` condition as
   h264/raw.
2. **The documented per-encoder `codec` JSON config key
   (`docs/configuration.md`) was silently unusable on Android at all** —
   `configuration::set_config_file()` is only ever called from desktop's
   `main.cpp` (`--config` CLI flag); nothing called it in
   `wivrn_server_jni.cpp`, so `read_configuration()` fell back to merging
   `xdg_config_home()/wivrn/config.json`, and `xdg_config_home()` returns
   `"."` when neither `XDG_CONFIG_HOME` nor `HOME` is set (neither is, on
   this app process) — resolving relative to this process's real `cwd`,
   confirmed live via `/proc/<pid>/cwd`, to be `/`, which the app cannot
   write to. There was no writable path the config loader would ever read.
   Fixed by calling `configuration::set_config_file()` in `nativeStart()`
   pointing at this app's own `files/` subdirectory (writable via
   `run-as`, unlike the app data dir's own root), making
   `{"encoder":{"encoder":"mediacodec","codec":"h265"}}` (or `h264`) via
   `adb shell run-as org.meumeu.wivrn.server sh -c 'echo ... >
   files/config.json'` actually take effect. This is a real, permanent fix
   independent of the HEVC test — config.json was simply dead code on
   Android before this.

Also hardened `video_encoder::SendData()`'s network-send `catch(...)`,
which silently discarded every exception with zero logging (`network_error_logged`
flag, log once per instance). Found no exception ever fires for the issue
below — a real gap either way (a genuinely broken connection was
previously invisible server-side), but ruled out as this bug's cause.

**Corruption result (one run each, HEVC vs AVC, ~90s, dual-stream,
otherwise-identical config)**: excluding loading-splash frames —

| | Left (stream 0) | Right (stream 1) |
|---|---|---|
| AVC | ~1.9% | ~15.0% |
| HEVC | ~1.8% | ~1.4% |

Right-eye corruption dropped roughly 10× under HEVC in this one comparison.
Left eye was essentially unchanged. **Not yet confirmed across repeat
runs** — the existing corruption investigation above already noted
right-eye rate varies run-to-run even under AVC alone, so this needs at
least one more repeat before treating it as a real AVC-specific finding
rather than this run's luck. If it holds up, it's real evidence *for* Case
A (AVC-specific issue) over the resource-contention hypothesis, though it
wouldn't fully explain the nonzero single-stream baseline corruption
either codec still shows.

**A separate, still-unresolved bug blocks live in-headset use of HEVC**:
the Quest 1 client's decoders negotiate and get created successfully
(`OMX.qcom.video.decoder.hevc` × 3, `h265` correctly offered/preferred by
the client), and the server produces a valid, independently-decodable
H.265 bitstream (confirmed via local dump + `ffprobe`/`ffmpeg`, zero syntax
errors, unlike the genuinely-malformed AVC captures seen earlier in this
investigation) — but the client never leaves its own lobby screen
("Connected... Waiting for an application on the server"), and
`shard_accumulator.cpp` logs "frame N was not sent because no shard was
received" for **every single frame**, forever. Ruled out: exception in
`send_stream`/`send_control` (none ever logged, see above), total network
failure (`dumpsys netstats` showed real packets — 51k packets/2.7MB —
actually leaving the Pixel's WiFi interface), and slow HEVC encode latency
(a strong initial suspect, disproven by direct measurement: AVC's own
`encode()` total latency, ~20.3ms avg/28.5ms p95, is if anything *higher*
than HEVC's ~18.1ms avg/24ms p95, yet AVC streams live fine and HEVC never
does — so raw per-call encode latency isn't the differentiator, since both
already exceed a naive single-frame real-time budget and the pipeline is
2-slot-pipelined regardless). Root cause not yet found; next concrete step
would be comparing actual frame sizes/shard counts per frame between the
two codecs and instrumenting `shard_accumulator::push_shard`'s `frame_diff`
directly. Parked for now — does not block the corruption comparison above,
which only needed the local server-side dump.

**Incidental discovery, worth keeping permanently**: Monado's own logger
defaults to `XRT_LOG=warn` (`u_logging.c`), silently dropping every
`U_LOG_I` call before it ever reaches Android's logcat — including the
`frame_timing_stats` `[timing]` summaries and the `WIVRN_DUMP_NV12`
per-frame log lines this investigation and the one before it both relied
on. Neither was actually broken; they were invisible. Like
`WIVRN_ONLY_STREAM`, this is read via Monado's own `DEBUG_GET_ONCE_LOG_OPTION`,
which on Android reads an Android system property directly:
`adb shell setprop debug.xrt.XRT_LOG info` (values: `trace`/`debug`/`info`/
`warn`/`error`), no rebuild needed. Worth setting at the start of any
future on-device debugging session on this branch.

**Tooling**: `tools/foveation-pc-test/MediaCodecEnum.java` (not git-tracked,
same convention as this doc's other probes — see the reproducibility
warning above) enumerates every `video/avc`/`video/hevc` `MediaCodecInfo`
with full capabilities; `debugging/scan_corruption.py` (gitignored scratch,
`/debugging/`) does the per-frame near-black-pixel corruption scan used
above and in the investigation before it, without requiring numpy
(`bytes.translate()` threshold count).

### Real Vulkan validation now works on-device — and finds nothing in the corruption-suspect path

Following up on this milestone's "no validation layers available on this
stock device" limitation (§M of `docs/pixel10-pro-xl-gpu-media-investigation.md`
was about `app_process`-launched standalone probes specifically — the real
installed `server-app` is a Gradle debug build, confirmed live via
`dumpsys package` to be `DEBUGGABLE`, which changes what's possible).

**Android's documented, official no-root mechanism for this
(`enable_gpu_debug_layers`/`gpu_debug_app`/`gpu_debug_layers` settings,
pushing the layer to `/data/local/tmp/vulkan/debug/`) does not work on this
device/OS build at all** — confirmed live: the Vulkan loader's own debug
trace (`adb logcat`, tag `vulkan`) never once searches that directory
regardless of those settings, and enabling the mechanism process-wide also
crashes the app's own hardware-accelerated UI renderer (`libhwui.so`'s
`VulkanManager::initialize`) before the compositor's own Vulkan instance is
ever reached (confirmed reproducible, `hardwareAccelerated="false"` avoids
the crash but the loader still can't find the layer either way —
`ErrorLayerNotPresent` on the compositor's own `vkCreateInstance`).

**What actually works**: bundling `libVkLayer_khronos_validation.so`
directly into the (debug-build-only) APK's own native library directory —
one of the paths the loader *does* search for every app, confirmed live
(`"searching for layers in '.../lib/arm64'"`) — via a `debug`-build-type
`jniLibs.srcDirs` entry in `server-app/build.gradle` pointing at
`tools/vulkan-validation/` (not git-tracked, same `tools/` convention as
the rest of this project's toolchain; silently contributes nothing if
absent). `server/utils/wivrn_vk_bundle.cpp` explicitly requests
`VK_LAYER_KHRONOS_validation` for its own instance only when
`WIVRN_VK_VALIDATION` is set (`adb shell setprop debug.xrt.WIVRN_VK_VALIDATION 1`,
no rebuild needed) — confirmed live: `vulkan: Loaded layer
VK_LAYER_KHRONOS_validation`, routed through the pre-existing
`message_callback`/`VK_EXT_debug_utils` messenger this file already had.

**Three real, independent Vulkan API-misuse bugs found**, none previously
known, none related to the corruption investigation:

1. `vkCreateDevice()`: `VK_ANDROID_external_memory_android_hardware_buffer`
   enabled without its required dependency `VK_EXT_queue_family_foreign`.
2. `vkGetImageMemoryRequirements()` queried on an
   `VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID`
   image before it's bound to memory (spec requires binding first for this
   handle type).
3. `vkCreateImageView()`: `VK_FORMAT_R8G8B8A8_SRGB` view on a
   `VK_FORMAT_R8G8B8A8_UNORM` image without `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`
   set, repeated across multiple `vk_image_collection` images.

**All three fixed.** #1 was our own device extension list
(`wivrn_vk_bundle.cpp`, add `VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME`
alongside the AHB extension it's required by). #2 and #3 were both in
Monado's own vendored `vk_helpers.c`'s `vk_create_image_from_native()` —
its sibling allocating function (`vk_image_allocator.c`'s `create_image()`)
already handled both cases correctly (skips querying memory requirements
for AHB images, forces `MUTABLE_FORMAT_BIT` on the same SRGB→UNORM
downgrade), so the import path was brought in line with it via
`patches/monado/0013-...patch` (this project's existing patch mechanism,
see 0012 for precedent — not hand-edited, since it's re-fetched vendored
source). Re-ran the validated session after all three fixes: session
establishes normally, both encoders open, **zero validation messages at
all** over an extended dual-stream run (confirmed clean, not just
"no new ones" — these three were the entire prior list).

**Corruption investigation relevance — the actually important result**:
ran a real session under real dual-stream concurrent GPU-compute+encode
load, first with plain Core Validation, then with **Synchronization
Validation** explicitly enabled (`adb shell setprop
debug.vulkan.khronos_validation.enables
VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` — a distinct
opt-in feature, not on by default, specifically checks for missing/incorrect
barriers and read-after-write/write-after-write/write-after-read hazards,
i.e. exactly the class of bug the compositor's own compute-write +
`vkCmdCopyImageToBuffer`-readback path would need to have for this to be
our own bug rather than a hardware/driver issue). **Zero sync-hazard
messages over an extended run (~110s combined), only the three unrelated
issues above.** This doesn't prove the corruption is hardware/driver —
Synchronization Validation only catches hazards visible to the Vulkan API's
own synchronization model, not e.g. genuine SoC memory-controller
contention between concurrent GPU-compute and hardware-video-encode DMA —
but it's real, direct evidence (not just behavioral inference from the
force-wait/serialize experiments above) that our own barrier/synchronization
*code* is spec-compliant. Combined with the existing evidence (force-wait
didn't help, serializing made it worse), this further narrows the leading
hypothesis toward genuine hardware/driver resource contention rather than
a closeable bug in this codebase.

**Next step, not yet done**: re-run this with the actual black-block
corruption reproducing during the validated session (this run used the
default AVC path with the same dual-stream load that normally produces the
corruption, but corruption presence wasn't independently confirmed via
`WIVRN_DUMP_VIDEO` in the same run) to be certain the validated window
actually overlapped a corrupted frame, not just similar load conditions.

**Calibrated answer to "is the corruption 100% confirmed to be the GPU
driver's fault"**: no, not 100% — but the evidence strongly points that
way. NV12 input is already corrupt before the encoder ever sees it (rules
out MediaCodec/VPU-as-encoder); corruption scales sharply with concurrent
GPU-compute+encode load (0.5–1.6% single-stream → 8–19% dual-stream);
forcing maximum synchronization didn't help and serializing made it
*worse* (the opposite of what fixing a real app-level race would do); and
now Synchronization Validation — which specifically catches missing/
incorrect barriers and hazards — found nothing over an extended real
dual-stream run. That's a consistent picture pointing at genuine
hardware/driver resource contention rather than a bug in this codebase's
Vulkan usage. It's not airtight: Sync Validation only sees what the
Vulkan API's own synchronization model can see, not e.g. a real SoC
DRAM/L2 race or a driver scheduling bug that's "legal" per the API but
still physically wrong — and the nonzero single-stream baseline (0.5–1.6%
with zero concurrency) isn't fully explained by anything found so far
either.

### Cross-device confirmation: identical app/server, different GPU vendor (Samsung Galaxy S22, Exynos 2200 / Xclipse 920) — clean, no glitches

A second physical Android phone (Samsung Galaxy S22, `s5e9925`/Exynos 2200,
AMD Xclipse 920 GPU — a completely different GPU vendor/architecture from
the Pixel 10 Pro XL's PowerVR DXT-48-1536) was set up as a second
`server-app` install, same APK build, same Quest 1 client, same test app
(`com.UnityTechnologies.com.unity.template.urpblank`). This surfaced one
real, unrelated bug along the way (see below), but once fixed, a live
in-headset session — first HEVC (real hardware `c2.exynos.hevc.encoder`),
then AVC (`c2.exynos.h264.encoder`, forced via a `{"encoder":{"codec":
"h264"}}` config.json, same `select_encoder`/`configuration::encoder.codec`
mechanism desktop already uses) — was confirmed **smooth, with no glitches
and no visible corruption**, direct visual confirmation in-headset, not
just server-side telemetry.

Note on a red herring hit along the way: the Quest client logged
`shard_accumulator`'s "frame N was not sent because no shard was received"
continuously throughout this same clean, glitch-free S22 session — the
exact message previously associated (Milestone 5's HEVC A/B diagnostic,
above) with the Pixel's still-unresolved "stuck in lobby, HEVC never
streams" bug. Since real, smooth video was visibly rendering in the headset
at the same time this log line was spamming, that message is **not**, by
itself, evidence of broken streaming — it fires under some normal
skipped-frame condition too. The Pixel HEVC bug's actual symptom is the
client visibly stuck on its lobby screen forever; this log line alone
isn't a reliable signal for that and shouldn't be treated as one in future
diagnosis.

**This is real, direct cross-hardware evidence for the driver-causation
hypothesis**: identical WiVRn app code, identical compositor/encoder
pipeline, run on a second GPU vendor entirely — zero corruption observed.
Combined with the Synchronization-Validation-clean result above (rules out
a spec-visible app-level race) and the earlier NV12-already-corrupt-
before-the-encoder finding (rules out MediaCodec/VPU), this is the
strongest evidence yet that the black-block corruption is specific to the
Pixel 10 Pro XL's PowerVR driver rather than this codebase's Vulkan usage.
Still not textbook-airtight proof (a single comparison device isn't a
statistical sample, and the nonzero single-stream Pixel baseline is still
unexplained), but the overall picture now points at PowerVR specifically,
not "some Android GPU driver" in general.

**Unrelated bug found and fixed during this cross-device setup**: the
HEVC encoder's hardware component name was hardcoded to
`c2.google.hevc.encoder` (Tensor-only, added during the Pixel HEVC A/B
diagnostic above) with no fallback, and `encoder_settings.cpp`'s
`check_mediacodec()` capability probe didn't exercise
`video_encoder_mediacodec`'s deliberately-lazy `ensure_codec()` — so the
probe passed cleanly on the S22 (nothing tried to actually create the
codec yet) and the server then **crashed the whole process** with an
uncaught `std::runtime_error` the moment the first real session reached
`present_image()`/`ensure_codec()`. Fixed (commit `e73f0884`): added
`video_encoder_mediacodec::probe_ensure_codec()` so the capability probe
actually attempts real codec creation up front (a failure there is now
"codec unsupported", not a live crash), and `ensure_codec()` now falls
back from the hardcoded name to `AMediaCodec_createEncoderByType()` (with
a software-encoder-name-prefix guard, same heuristic
`android_decoder.cpp`'s `hardware_accelerated()` already uses) if the
hardcoded name isn't present. Verified live: the S22 correctly falls back
and finds its own real hardware HEVC encoder (`c2.exynos.hevc.encoder`),
no crash, both streams reach `RUNNING`.

## Milestone 6 — re-enable the shared 3-layer stream image, gated to exclude PowerVR only

Now that the cross-device confirmation above showed the black-block
corruption doesn't reproduce on a non-PowerVR GPU, the natural follow-up
was: Milestone 4.5's single-layer-per-stream split was applied
unconditionally to every device, but the bug it works around is
PowerVR-specific. Every other vendor has been paying for that workaround
(3 image allocations + 3 barriers instead of 1) with no benefit.

**Important nuance this needed to respect**: Bug #1 was a *silent*
compute-write corruption, not a rejected allocation (`AHardwareBuffer_isSupported`
would never have caught it). That rules out any runtime capability-probe
approach -- the gate has to be an explicit device check, not
auto-detection. Went with a vendorID denylist: `vk_bundle::multi_layer_stream_images`,
computed once from `physical_device.getProperties().vendorID` at Vulkan
instance creation, `true` for every vendor except PowerVR/Imagination
Technologies (`0x1010`). Denylist rather than allowlist on purpose: a
not-yet-tested GPU defaults to the fast path, since an allowlist would
mean every new device silently keeps the slower workaround forever with
no way to discover it doesn't need it -- the one GPU actually confirmed
bad is the only exception carved out.

**Implementation** (`compositor.h`/`.cpp`, `video_encoder_mediacodec.h`/`.cpp`,
`wivrn_vk_bundle.h`/`.cpp`): `compositor::stream_image::image` changed
from an owning `image_allocation` to a non-owning `vk::Image` view; the
owning allocation(s) moved to a new `compositor::image::storage` vector
(1 shared 3-array-layer allocation when `multi_layer_stream_images`, or 3
separate single-layer allocations otherwise, unchanged from before).
`video_encoder_mediacodec::present_image()`'s five previously-hardcoded
`baseArrayLayer = 0` sites now call a small `image_layer()` helper
(`stream_idx` when sharing, else `0`) -- it has to compute this
independently rather than receive it as a parameter, since it only has
`vk_bundle` + its own `stream_idx` to go on, not the compositor's
`stream_image` objects. **`foveation.cpp`/`.h`/`.comp` needed zero
changes**: each stream's Y/CbCr `vk::ImageView`s are already built
pointing at the correct array layer at creation time (compositor.cpp's
new `make_stream_view()`), so the compute-shader dispatch side is
completely unaware of (and unaffected by) whether those views happen to
alias one shared image or point at 3 separate ones.

**Verified live on both devices** (session reaches `XR_SESSION_STATE_FOCUSED`,
zero crashes, zero new validation/compositor warnings on either):

| | GPU | `multi_layer_stream_images` |
|---|---|---|
| Pixel 10 Pro XL | PowerVR D-Series DXT-48-1536 MC1 | `false` (old safe path, unchanged) |
| Samsung Galaxy S22 | Samsung Xclipse 920 | `true` (new shared-image fast path) |

Not yet done: a direct corruption-rate comparison (à la the HEVC A/B
diagnostic) isolating this change's effect on the S22 specifically --
the S22 was already confirmed corruption-free before this change, so
there's no regression signal to look for there, and this change makes no
behavioral difference on the Pixel at all (still takes the exact same
code path as before). If a third, non-PowerVR device is ever added to
this investigation, it's worth re-confirming this way rather than
assuming.

## Milestone 7 — VRChat fell back to flat 2D: a native-lib-extraction bug, not a Monado version issue

Third-party OpenXR apps installed on the S22 had a spotty history in this
project (per earlier investigation: only this project's own locally-built
Unity template app reliably worked; `somar`/`openxrdemo` did not). VRChat
specifically launched but silently fell back to flat 2D on the phone's own
screen instead of ever entering VR -- prompted by a report that the
latest Monado master branch fixes this class of problem for Valve's
Steam Frame headset. Rather than assume that report applied here and
blindly bump Monado's vendored revision, captured VRChat's own
`OpenXR-Loader` log on-device first (`adb logcat --pid=<vrchat pid>`) to
see the actual failure, per the plan to test and read logs before
changing anything.

**The real cause, found directly in the log**: VRChat's loader queries
the standard `content://org.khronos.openxr.runtime_broker` provider,
correctly resolves our package and `libopenxr_wivrn.so`, then fails:

```
Got runtime: package: org.meumeu.wivrn.server, so filename: libopenxr_wivrn.so,
    native lib dir: .../base.apk!/lib/arm64-v8a, has functions: no
Error: library .../base.apk!/lib/arm64-v8a/libopenxr_wivrn.so does not appear to exist
Error: RuntimeInterface::LoadRuntimes - failed to load a runtime
[XR] xrCreateInstance: XR_ERROR_RUNTIME_UNAVAILABLE
```

Modern AGP defaults to `android:extractNativeLibs=false`: native `.so`
files are stored **uncompressed inside the APK** rather than extracted
to real files, and `ApplicationInfo.nativeLibraryDir` reports a virtual
`base.apk!/lib/arm64-v8a`-style path (Android's dynamic linker supports
`dlopen()`-ing directly from an uncompressed APK entry since API 23).
Confirmed via `unzip -lv` that our APK's `.so` files were indeed stored
this way (`Stored`, 0% compression). This project's own locally-built
Unity template app's `OpenXR-Loader` build handles that virtual path
fine (confirmed working throughout this whole investigation); VRChat's
bundled `OpenXR-Loader` build apparently doesn't -- it tries something
like a plain `access()`/`open()` on that literal `!`-containing string,
which isn't a real filesystem path.

**Checked whether this was actually a Monado-version issue before
concluding it wasn't**: cloned upstream Monado
(`https://gitlab.freedesktop.org/monado/monado.git`, network access to
which does work in this environment) and diffed all 117 commits between
our pinned `monado-rev` and `origin/main`. None touch anything
Android/native-lib/runtime-broker/Steam-Frame related. Whatever the
Reddit report was describing, it isn't in Monado's own commit history in
a way that would explain this specific symptom -- this bug lives
entirely in the Android APK packaging (WiVRn-Android's own
`server-app/build.gradle`/`AndroidManifest.xml`), not in Monado at all.
**Did not bump Monado's vendored revision** -- would have changed
nothing for this bug and adds real risk (all of `patches/monado/*.patch`
are pinned against the current rev).

**Fix**: `android:extractNativeLibs="true"` in
`server/android/AndroidManifest.xml` (plus the matching
`packagingOptions.jniLibs.useLegacyPackaging = true` AGP wants set
alongside it, in `server-app/build.gradle`) forces Android to actually
extract the native libs to a real file at install time
(`legacyNativeLibraryDir`, confirmed via `dumpsys package` and a direct
`ls` on-device: a real, world-readable `-rwxr-xr-x` file). This is a
strictly more compatible default for a *runtime broker* specifically,
regardless of which client loader version ends up querying it -- every
runtime shipped before uncompressed-native-lib packaging became AGP's
default already worked exactly this way.

**Verified live on the S22**: VRChat's `OpenXR-Loader` now successfully
resolves and loads `libopenxr_wivrn.so`, `xrCreateInstance` succeeds, and
the session reaches `XR_SESSION_STATE_FOCUSED` -- VRChat genuinely
enters VR instead of falling back to flat 2D. No crash on either process.
One remaining, unrelated, non-fatal item: `XR_ERROR_PATH_UNSUPPORTED` for
Valve's Steam-Frame-specific interaction profile
(`/interaction_profiles/valve/frame_controller_valve`), which this
Monado build doesn't implement -- VRChat falls back to its other
supported interaction profiles for this and it isn't a blocker. Worth
re-testing whether `somar`/`openxrdemo` (the other previously-failing
third-party apps from earlier in this investigation) are also fixed by
this same change, since their failure mode was never conclusively
isolated to this exact cause.

## Milestone 8 (branch `feat/adrenotools-turnip`) — custom Vulkan driver loading (adrenotools/Turnip) for Adreno devices

Confirmed VRChat's black-screen investigation (Milestone 7) wasn't the only
new device brought into this project: a Samsung Galaxy Tab (SM-X810,
Snapdragon 778G / Adreno 642L) fails to start the WiVRn server at all --
`GPU does not support Vulkan synchronization2 feature` ->
`xrt_instance_create_system failed` -> the process exits immediately, every
time, before a Quest can even connect. Verified live with a temporary
diagnostic: `device apiVersion = 1.1.128`, and `VK_KHR_synchronization2`
genuinely isn't in the device's own extension list -- a real driver
limitation (this compositor's synchronization2 requirement is load-bearing,
not something that can be relaxed), not a bug in this codebase.

**Scope note, decided explicitly before building anything**: GameNative/
Winlator-style apps solve exactly this class of problem on Adreno via
Turnip (Mesa's open Adreno Vulkan driver) loaded through **adrenotools**
(a rootless driver-swap library), but their "Mali support" (most Tensor
chips, including this project's own confirmed Tensor G5 = PowerVR device)
is an unrelated mechanism (VirGL/Gladio: full software GPU-API translation,
not a driver swap) that wouldn't fix a missing-Vulkan-feature problem like
this one anyway. **No equivalent to Turnip exists for Mali, PowerVR, or AMD
Xclipse (the S22's GPU) today** -- confirmed via research before writing any
code. Built only the real, working Adreno/Turnip path.

**What landed**:
- Vendors `libadrenotools` (github.com/bylaws/libadrenotools) via
  FetchContent, Android-only, including its `lib/linkernsbypass` git
  submodule -- the first FetchContent'd dependency in this repo that needs
  one (`GIT_SUBMODULES` in the `FetchContent_Declare`).
- `server/utils/vulkan_loader.{h,cpp}` (new): resolves the
  `PFN_vkGetInstanceProcAddr` `vk::raii::Context` bootstraps from. Default
  path (no custom driver configured -- every device this project runs on
  today except the tablet): a plain `dlopen("libvulkan.so")` +
  `dlsym(vkGetInstanceProcAddr)`, functionally identical to what
  vulkan-hpp's own internal `DynamicLoader` already did. Custom path:
  `adrenotools_open_libvulkan()`'s isolated, hook-injected driver. Never
  hard-fails on a bad custom driver selection -- falls back to the system
  driver and logs a warning instead.
- `wivrn_vk_bundle.cpp`'s `vk_ctx` construction is now explicit
  (`VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0`, Android-only, `#if`-guarded so
  desktop is untouched) instead of relying on `vk::raii::Context`'s
  implicit default constructor -- **verified live on the S22 that this is
  a no-op behavior change** for every device that doesn't select a custom
  driver (identical GPU-name log, identical `multi_layer_stream_images`
  value, session still reaches `FOCUSED`, before and after).
- A new settings screen (`SettingsActivity.java`, reached from
  `MainActivity`'s options menu) imports the same **ADPKG**-format driver
  `.zip` GameNative/Winlator use (`meta.json` + the driver's `.so` files),
  extracts it into the app's own internal data dir (adrenotools requires
  this, not sdcard), and persists the choice via `SharedPreferences` --
  this app's first use of it, no prior Java-side persistence existed at
  all. No new dependency: `java.util.zip` and `org.json` are both
  JDK/platform stdlib.
- `wivrn_server_jni.cpp`'s `nativeStart()` now takes the app's own
  `ApplicationInfo.nativeLibraryDir` (a Java-only value adrenotools
  requires exactly) plus the persisted driver choice (both null = system
  default).

**Verified on real hardware**: S22 (no driver configured) is provably
unaffected by every step above -- re-ran the full regression check after
each change, not just once at the end. Tablet (no driver configured) still
fails exactly the same pre-existing way, confirming nothing regressed there
either. **Not yet verified**: actually loading a real Turnip build -- needs
a real ADPKG package (out of scope to source/host, matching how
GameNative/Winlator work: the user supplies it) and interaction with the
picker UI (Storage Access Framework's file-picker dialog needs a real touch
interaction, not drivable blind over adb) to confirm end-to-end.

## Milestone 8 follow-up — Turnip verified working on real hardware, HEVC lobby bug repeats, sysmem compat toggle added

Closed the "not yet verified" gap above: got a real Turnip build for the
tablet's Adreno 642L (`The412Banner/Banners-Turnip`, an automated bleeding-edge
Mesa CI packager — picked over the more curated `K11MCH1/AdrenoToolsDrivers`
repo purely because it was tested first; see below for why the K11MCH1 R8
line turned out to be a worse choice for this device), imported it through
the Milestone 8 settings UI, and confirmed: `synchronization2` now present,
GPU-name log shows Turnip/Mesa instead of the Qualcomm proprietary string, no
`XRT_FEATURE`-level regression on the Pixel/S22 (unchanged, re-verified),
session reaches `XR_SESSION_STATE_FOCUSED` with a real Quest connected, both
hardware encoders reach `RUNNING`.

Two more real, on-device findings from that first successful run:

- **The already-documented Milestone 5 HEVC "stuck in lobby, no shard
  received" transport bug reproduces on this device too** — same fix as the
  S22 (`config.json`: `{"encoder":{"codec":"h264"}}` to force AVC). Not a new
  bug, just confirmation it's driver/device-independent.
- **`TU_DEBUG=sysmem`** (Mesa's own documented Turnip env var, forces
  system-memory rendering instead of GMEM tile-based rendering; Turnip's own
  release notes call it out for "glitchy" rendering on some chips) visibly
  fixed a *static* stereo duplication/ghosting artifact seen in one early
  screenshot. Exposed as a real, persisted, opt-in toggle rather than an
  always-on env var: `DriverSettings.java` gained a `sysmemCompat` boolean
  (`SettingsActivity.java`'s "Compatibility mode" checkbox), threaded all the
  way through `WivrnServerService.nativeStart()` → `wivrn_server_jni.cpp` →
  `setenv("TU_DEBUG", "sysmem", 1)`. Deliberately opt-in, not default-on:
  GMEM tiling is a real performance win on tile-based Adreno GPUs, and this
  session's own testing later found a real Adreno-vs-Xclipse latency gap the
  user could feel, so forcing sysmem for every app regardless of need isn't
  free. (There is also an independent `debug.wivrn.tu_debug` adb property
  path into the same `setenv()` call, for testing arbitrary `TU_DEBUG` values
  without rebuilding — both paths set the same env var, either can turn it
  on.)

**Important correction found later in the same investigation**: the
"duplication" artifact `sysmem` fixed turned out to be a *different*, milder
bug than the actual VRChat-specific stereo problem described below — fixing
one static-screenshot artifact does not mean the underlying issue is solved.
Don't treat `sysmemCompat` as a real fix for the Milestone 9 investigation;
it's a real, narrower workaround for a real, narrower symptom.

## Milestone 9 — Steam Frame's OpenXR interaction profile (`XR_VALVE_frame_controller_interaction`): VRChat's controllers were completely unbound, not just unrendered

**Root cause, confirmed with real log evidence, not assumption**: VRChat's
Android build makes exactly one `xrSuggestInteractionProfileBindings` call
covering the *entire session* — for `/interaction_profiles/valve/frame_controller_valve`
— and never falls back to any other profile if it fails. Confirmed by
grepping a full clean session's log for every `SuggestInteractionProfileBindings`
call: only ever the one, for Valve's profile, failing with
`XR_ERROR_PATH_UNSUPPORTED`. This means whatever profiles this driver already
supported (plain Oculus Touch, Touch Pro, Touch Plus, all already enabled —
see Milestone list of `XRT_FEATURE_OPENXR_INTERACTION_*` flags in
`server/CMakeLists.txt`) were **entirely irrelevant** to this bug: VRChat
never asked for any of them.

Separately, VRChat's own `SteamFrameControllerModel` subsystem (visible/
render-model fetching specifically, not input) depends on
`XR_EXT_uuid`/`XR_EXT_render_model`/`XR_EXT_interaction_render_model`, none of
which Monado implements (`xrEnumerateInteractionRenderModelIdsEXT` etc. all
fail to resolve, `result=-7` = `XR_ERROR_FUNCTION_UNSUPPORTED`) — this affects
only the visual controller *mesh*, and is a separate, still-open gap from the
input-binding fix below.

**Why this extension exists at all and why we can't just make VRChat ask for
Oculus Touch instead**: per Valve's own published `com.valvesoftware.openxr.utils`
Unity package docs (`github.com/ValveSoftware/Unity`) — the only real,
first-party source found for this extension, since it is not yet in the
public Khronos registry — *"Without this profile, Steam Frame controllers
are presented to your application as emulated Oculus Touch controllers."*
VRChat's Android build has this Unity feature compiled in and enabled, so it
unconditionally suggests bindings only for the Frame profile; there is no
app-side fallback behavior we can influence from the runtime side. The only
real fix is implementing the profile.

**What landed** (all in `patches/monado/0015-*.patch`, applied through the
existing `patches/monado/` + `apply.sh` mechanism used for every other Monado
patch in this repo — regenerated via a real clone of Monado at the pinned
`monado-rev` commit, with every existing patch reapplied first, then
`git format-patch`, not a hand-written diff):

- `XRT_DEVICE_VALVE_FRAME_CONTROLLER` — new `xrt_device_name` enum value
  (`xrt_defines.h`; this one enum is hand-maintained, unlike `XRT_INPUT_*`/
  `XRT_OUTPUT_*` which are too, it turns out — see below).
- ~35 new `XRT_INPUT_VALVE_FRAME_CONTROLLER_*` / one
  `XRT_OUTPUT_NAME_VALVE_FRAME_CONTROLLER_HAPTIC` enum values, at the next
  free hex block (`0x16`, `0x0D00`) — confirmed the true next-free block by
  scanning all existing `XRT_INPUT_NAME(0x.., ...)` values case-insensitively
  first (a first pass using only lowercase `0x` missed the `0X15` block
  already used by `MAGNETRA2`, giving a wrong answer — case-insensitive scan
  caught it).
- The actual `/interaction_profiles/valve/frame_controller_valve` profile
  entry in `bindings.json` (Monado's own interaction-profile source of
  truth, code-generates the binding tables from this one file) — component
  paths (trigger/squeeze/thumbstick/bumper/menu/view/system/a/b/x/y/dpad_up/
  dpad_left/dpad_down/dpad_right/grip/aim/haptic, including which are
  `"side"`-restricted to one hand) verified against Valve's own published
  doc table, **including every `click`+`touch` component**, not a trimmed
  subset — a first attempt that dropped `touch` components to save effort
  failed live (`xrSuggestInteractionProfileBindings` rejected a real
  VRChat-requested binding for `/user/hand/left/input/squeeze/touch` because
  that subpath didn't exist in our trimmed schema) and had to be redone in
  full.
- `oxr_extension_support.py`: registers `XR_VALVE_frame_controller_interaction`
  the same way every other vendor extension is, one line, alphabetically
  sorted with the rest.
- Root `CMakeLists.txt`: new `XRT_FEATURE_OPENXR_INTERACTION_VALVE_FRAME_CONTROLLER`
  option (default `OFF`, matching the pattern for other niche/new vendor
  extensions like `LOGITECH_MX_INK`); turned `ON` in *this project's*
  `server/CMakeLists.txt`, not upstream Monado's default.
- **The one non-obvious, easy-to-miss piece**: adding the CMake `option()`
  alone does nothing — `xrt_config_build.h.cmake_in` needs its own explicit
  `#cmakedefine XRT_FEATURE_OPENXR_INTERACTION_VALVE_FRAME_CONTROLLER` line,
  or the option's value never becomes an actual compiler-visible `#define`
  anywhere. Missing this produced a very specific, confusing intermediate
  symptom worth remembering: the interaction profile *path* got recognized
  (bindings.json/xrt_defines.h changes were live), but Monado still rejected
  the app's call with a *different* error — `"used but
  XR_VALVE_frame_controller_interaction not supported by runtime"` — because
  the extension-support macro was never actually compiled in. Two visibly
  different Monado error strings for "profile path unknown at all" vs.
  "profile known but extension not enabled" turned out to be the key clue.
- `openxr.h` (vendored inside Monado's own `external/` tree) needed the
  extension's name/version macros hand-added — confirmed first via grep that
  this extension genuinely isn't in the currently-vendored public OpenXR
  headers at all (only the older, unrelated `XR_VALVE_analog_threshold`
  exists), so there was nothing to pick up automatically.
- `server/driver/wivrn_controller.cpp`: a new `frame_controller_input_binding[]`
  / `frame_controller_output_binding[]` table plus a
  `.name = XRT_DEVICE_VALVE_FRAME_CONTROLLER` entry in
  `make_binding_profiles()` — the exact same "remap this profile's abstract
  inputs onto whatever real inputs this device actually has" mechanism
  already used for Touch Pro/Touch Plus/Vive Focus 3, just for a fifth
  profile. Real Quest Touch controllers have no bumper, no system button, no
  dpad, no second per-hand menu button, and no discrete trigger/squeeze
  *click* (analog value only) — those components stay declared in the
  profile (so the app's binding call still succeeds) but simply unbound in
  this table, which is legal and harmless (the app just never receives input
  for them). One asymmetry worth remembering: Frame's own controller has a
  menu-equivalent on *both* hands (`menu` on right, `view` on left); real
  Quest Touch has only one, on the left — only `view` (left) gets bound to
  our real menu input, `menu` (right) stays unbound.

**Verified live**: with this patch, VRChat's `xrSuggestInteractionProfileBindings`
call for the Frame profile succeeds with no error at all (previously failed
every time), and the user confirmed controllers work in-session
(pointing/aiming, button input) after this landed. The
`XR_EXT_render_model`-family gap (visible controller *mesh*) remains open —
VRChat's own `SteamFrameControllerModel` still logs "Feature not ready" for
that specific piece, separately from input, which now works.

**Real supporting research, not guessed**: fetched the actual, current
`xrt_device_name`/`xrt_input_name` enum, `bindings.json` schema
(`bindings.schema.json`), and `oxr_extension_support.py` list from a live
clone of Monado at the exact pinned commit before writing anything — same
standing rule as every other patch in this repo. Component paths came from
Valve's own real published Unity package docs (raw file, not an AI-summarized
version — a first pass at this specific doc via a summarizing fetch produced
a subtly different, less precise component layout than the raw source; only
the raw doc gave the real per-hand path table used above). Also checked (and
ruled out) Monado's own upstream `master` branch and this device's SteamVR
install for any existing implementation of this extension before starting —
neither had anything; this is a real, from-scratch first implementation.

## Milestone 10 (still open) — VRChat stereo-fusion / right-eye desync investigation on Adreno/Turnip

A separate, harder problem from Milestone 9, on the same Adreno tablet:
VRChat's stereo image doesn't fuse correctly through the headset — reported
as overlapping/cross-eyed, "wobbly"/curved UI panel edges, a vertical
up/down mismatch between eyes, and edge content disappearing/reappearing
when turning the head. This project's own reference test app (Unity URP
blank template) shows none of this on the same server/driver setup, isolating
it to VRChat specifically (or to VRChat's interaction with this specific
Adreno/Turnip combination — see below, not fully resolved either way).

**Ruled out, with real on-device evidence, not assumption, in this order**:

1. **Server-side per-view pose/FOV/array-index extraction**
   (`compositor.cpp`'s `layer_commit()` fast path) — a temporary per-frame
   diagnostic (kept, gated behind `debug.xrt.WIVRN_LOG_VIEW_POSE`, see
   `log_view_pose` in `compositor.cpp`) proved both views get correct,
   symmetric pose data every single frame: only X differs, by a constant IPD
   offset; orientation is identical between eyes every frame, evolving
   smoothly and correctly with real head motion. This rules out our own
   view-extraction code.
2. **`multi_layer_stream_images`'s shared 3-layer fast path** (Milestone 6) —
   a runtime override (`WIVRN_MULTI_LAYER_STREAM_IMAGES` debug property,
   already committed in `wivrn_vk_bundle.cpp`) reproduced the identical bug
   with the fast path forced off, ruling out our own layer-squashing/packing
   code.
3. **The compositor's write path into its own internal stream image** —
   traced `layer_commit()`'s fast-path view extraction →
   `foveation::foveate()`'s per-eye compute dispatch (binds `src[0]`/`src[1]`,
   writes `content[0]`/`content[1]` via a per-eye push constant, `eye=0`
   writes from `src[0]`, `eye=1` from `src[1]`) → the `foveation.comp` shader
   itself (`iz = pc.eye`, reads `source[iz]`, writes to that eye's own
   dedicated destination image) end-to-end. Every stage is self-consistent
   and matches standard OpenXR view-0-is-left convention; no swap or
   cross-wiring found anywhere by reading the code.
4. **Server-side foveation index-table math** (`fill_ubo()`/`compute_params()`
   in `foveation.cpp`) — a real, well-founded suspicion (an unsigned
   underflow producing huge/wrapped index values, matching the
   user's own "content from the right edge reappears at the left edge"
   description almost exactly) was directly disproven with real captured
   data: added logging (gated behind `debug.xrt.WIVRN_LOG_FOVEATION_UBO`,
   see `log_foveation_ubo`) of the actual per-eye source rect and the first/
   last 5 entries of each eye's UBO index table. Both eyes' tables are
   clean, monotonic, and correctly bounded to the real source extent (1728px)
   — no underflow, no huge values. The two eyes' tables differ only in
   *density distribution* (a legitimate, expected difference from
   per-eye foveation-center placement), not in correctness.
5. **A pure gaze/eye-position-driven asymmetry in the foveation center**
   specifically — `debug.xrt.WIVRN_DISABLE_FOVEATION` (misleadingly named;
   it does *not* disable foveation/compression — doing that outright
   crashes, `fill_ubo()` asserts `count>0`, confirmed live, since this
   device's actual encode resolution is genuinely smaller than VRChat's real
   render resolution and the "no compression" code path assumes the
   opposite) forces the foveation *center* to dead-ahead (angle 0) for both
   eyes symmetrically, leaving the real compression ratio/math untouched.
   The wobble/seam persisted with this active, ruling out gaze-driven
   center asymmetry as the (sole) cause.

**Confirmed real and reproducible, with hard evidence**:

- **A genuine content-association issue exists somewhere upstream of the
  final encoder read.** Swapping which array layer each MediaCodec encoder
  instance reads from (`WIVRN_SWAP_EYE_LAYERS` debug property,
  `video_encoder_mediacodec.cpp`'s `image_layer()` — deliberately only the
  encoder's own *read* side, not `compositor.cpp`'s `image_layer()`, which
  also governs where the compositor *renders/writes* each eye and would
  change actual rendering, not just which content ends up on which
  channel) measurably reduced the overlapping/cross-eyed sensation. This is
  real signal, **not yet root-caused** — the compositor's own write path was
  independently verified correct in step 3 above, so either VRChat itself
  submits its two stereo views in a non-standard order for this specific
  runtime/profile combination, or the actual association error is
  client-side (decoder-to-eye texture/array-layer mapping on the Quest,
  never directly tested — see below). **Do not treat the encoder-side swap
  as a fix**: it defaults off, and the user's own follow-up testing showed a
  *different*, still-unexplained symptom (right-eye horizontal
  wrap-around/seam, described below) persists regardless of this swap,
  meaning it papers over one symptom without addressing the actual cause.
- **A separate, later, much more specific symptom, found by decoding the raw
  H.264 the server actually sent (before any client/network/decoder
  involvement) frame-by-frame**: the right eye shows a sharp, consistent
  vertical seam around x≈143 of 896px, described by the user as content
  from the right edge appearing to wrap to the left edge. **Proven to
  originate before encoding, not after**: extracted a single clean raw NV12
  frame directly from the pre-MediaCodec buffer dump (`WIVRN_DUMP_VIDEO`/
  `WIVRN_DUMP_NV12`, gated behind `debug.wivrn.dump`, both already existing
  Milestone 5 mechanisms) for both eyes at the same frame index, decoded via
  `ffmpeg -f rawvideo -pixel_format nv12`, and the seam is already visibly
  present in the right eye's raw pre-encode buffer, absent from the left's.
  This rules out MediaCodec, H.264, the network, and the client decoder —
  the corruption is server-side, before encoding.
- **This is genuinely correlated with the Turnip driver specifically, not
  our own compositor code, based on the evidence gathered so far**: steps
  1–4 above independently verified every piece of *our own* compositor code
  in this path (pose/view extraction, layer packing, the write dispatch
  itself, the foveation index math) is correct. That leaves either a Turnip/
  Adreno GPU synchronization or execution bug in this exact compute dispatch
  (candidates not yet directly tested: the per-eye descriptor-set caching in
  `foveation::foveate()`, which only rewrites bindings 1–5 "when they
  haven't changed since last time" while binding 0 — the source images — is
  rewritten unconditionally every frame; or a missing/incorrect barrier
  between VRChat's own render of its right-eye view and this compute
  shader's read of it), or VRChat's own view submission order being
  non-standard for this specific runtime — **not yet distinguished between
  the two**.

**A real technical side-finding from this investigation, independent of the
stereo bug itself**: got the WiVRn *client* (the Quest-side app, root Gradle
project — previously undocumented as unbuildable from this from-scratch
toolchain, see the Toolchain table) to compile successfully for the first
time, all the way to the final link step, by resolving three more missing
host tools the same documented way as `gettext`/`rsvg-convert`/`ktx`:
- `glslang-tools` (provides `glslangValidator`, the actual shader compiler
  `CompileGLSL.cmake`'s `Vulkan::glslangValidator` target needs) — obtained
  via `apt-get download glslang-tools` + `dpkg-deb -x`, same no-root pattern
  as every other host tool in this project.
- `gettext`'s own `msgfmt` additionally needed its runtime shared library
  (`libgettextsrc-0.21.so`, under the same extracted package's `lib/`
  directory) on `LD_LIBRARY_PATH` — present in the extracted package all
  along, just never linked into the environment before.
- CMake's `find_program`/`find_package(Vulkan)` results get cached at
  configure time — adding a tool to `PATH` after a previous failed configure
  requires actually deleting the stale `.cxx` build directory to force a
  real reconfigure, not just re-running Gradle.

Build stopped one step short of a working APK: the final link fails with
`ld.lld: error: undefined symbol: vkGetDeviceBufferMemoryRequirements` (and
`vkGetDeviceImageMemoryRequirements`) — VMA (`external/vk_mem_alloc.h`,
`VMA_VULKAN_VERSION=1003000` set in `common/CMakeLists.txt`) statically links
against these Vulkan 1.3 core-promoted symbols by default
(`VMA_STATIC_VULKAN_FUNCTIONS`), and the NDK's build-time stub `libvulkan.so`
(used only for link-time symbol resolution, not the real on-device driver)
doesn't export them. The known, standard fix — `VMA_STATIC_VULKAN_FUNCTIONS=0`
+ populating a `VmaVulkanFunctions` struct with at least
`vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` so VMA fetches the rest
dynamically — was identified but not yet applied (`common/vk/vk_allocator.cpp`
doesn't populate `pVulkanFunctions` at all today, and this macro is set at
the shared `wivrn-common` target level, used by both client and server, so
the fix should be scoped to avoid any server-side behavior change). **A
real, live client build (with the `debug_swap_eyes()` diagnostic already
added in `client/scenes/stream.cpp`, gated behind
`debug.wivrn.swap_eyes`) is the most direct remaining way to test whether
this is a client-side eye/texture-association bug** rather than continuing
to reason about it from server-side evidence alone — this is the natural
next step if this investigation continues.

**A related, separate, still-open observation from the same testing**:
edge content disappearing/reappearing when turning the head, and "curling"
of straight lines during head motion, look like a distinct reprojection/
FOV-overscan-margin symptom (the runtime timewarping to a newer head pose
than what was actually rendered, revealing the edge of a render that has no
extra margin past its own FOV) rather than a left/right association problem
— raised late in this investigation and not yet chased down.

**Driver testing matrix, for the record (all real, on-device tests, not
guesses)** — every alternative to the originally-working
`The412Banner/Banners-Turnip` build made things *worse*, not better, on this
device:

| Driver | Source | Result |
|---|---|---|
| `Turnip-v26.3.0-20260915-r4.zip` | The412Banner/Banners-Turnip | **Working baseline** — runs VRChat, has the stereo bug above |
| `Turnip_v26.0.0_R8.zip` | K11MCH1/AdrenoToolsDrivers | Crashes: `VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT` importing VRChat's own AHardwareBuffer swapchain |
| `Turnip_v26.0.0_R8_Sysmem.zip` | K11MCH1/AdrenoToolsDrivers | Same crash — confirms it's the DRM-modifier/tiled-layout import path, not GMEM tiling, since sysmem forcing doesn't touch it |
| `Turnip 22.3.6` (user-sourced) | unknown | Loads, `synchronization2` present, but this build predates Mesa's AHardwareBuffer/external-memory Vulkan support — `VkExternalMemoryTypeFlagBits(0x400) unsupported`, segfaults in the compositor thread |
| 4 proprietary Qualcomm driver blobs (`v819.2`, `v837`, `v840`, `v849`) | K11MCH1/AdrenoToolsDrivers | All extracted from 2024–2025 flagship devices (Quest 3, Meta Ray-Ban Display, a Vivo iQOO phone) — all Adreno 7xx/8xx, a real architecture mismatch with this A6xx tablet. All fail with the exact original blocker: `GPU does not support Vulkan synchronization2 feature` |

No proprietary/vendor driver update path exists for this exact GPU
generation (Adreno 642L, Snapdragon 778G, A6xx) — every available package is
sourced from a materially newer chip family. Turnip remains the only route
to a working `synchronization2`-capable driver on this hardware.

### Milestone 10 follow-up — the seam is already present in VRChat's raw imported swapchain image, before WiVRn's own code ever touches it; isolated to the Turnip driver, not WiVRn's architecture

Two wrong turns worth recording before the real isolation, since both looked
plausible at first and were disproved by actually reading the shared Monado
source rather than reasoning from the architecture diagram alone:

1. **"In-process Monado never uses AHardwareBuffer at all"** — wrong.
   `XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER` (`xrt_handles.h`) is gated
   purely on `XRT_OS_ANDROID_USE_AHB` + API level, an Android-platform-wide
   compile flag, not an IPC-vs-in-process one. Every Android Monado build
   uses AHardwareBuffer as the swapchain image's native representation,
   regardless of process topology.
2. **"The client-side AHardwareBuffer import is IPC-specific"** — also
   wrong. `client_vk_compositor_create()` / `vk_create_image_from_native()`
   (`compositor/client/comp_vk_client.c`) is generic Monado code sitting on
   top of *any* `xrt_compositor_native`, in-process or IPC alike. WiVRn's own
   server code (`server/`) doesn't touch the app's swapchain usage
   flags/format either (confirmed by grep — no `XRT_SWAPCHAIN_USAGE`
   references anywhere in `server/`), so there's no WiVRn-specific
   image-creation difference to point to.

**The real, confirmed difference**: the comparison point on this tablet
isn't the historical "Cardboard-style" in-process build at all — it's
`org.freedesktop.monado.openxr_runtime.out_of_process`, Monado's own stock
Android OpenXR runtime broker, which is *itself* out-of-process (same
IPC/AHardwareBuffer-sharing architecture as WiVRn — the package name says so
directly). Confirmed live: this runtime runs on the tablet's **stock/system
Adreno Vulkan driver**. WiVRn, on the same tablet, is forced onto **Turnip**
instead, because the stock driver is missing `VK_KHR_synchronization2`
(Milestone 10's original blocker) — Turnip is the only currently-working
route to a `synchronization2`-capable driver on this Adreno 642L. So with
architecture held constant (out-of-process, IPC, AHardwareBuffer swapchain
sharing, in both cases), **the one real variable between the working
comparison and WiVRn is the Vulkan driver itself**, not anything about
WiVRn's own code or process topology.

**Test**: a new one-shot diagnostic, `dump_app_image_once()` in
`server/compositor/compositor.cpp` (gated behind
`WIVRN_DUMP_APP_IMAGE`, read live via a direct `debug_get_num_option()` call
rather than the cached `DEBUG_GET_ONCE_NUM_OPTION` macro — see the code
comment; the macro latches its value on first read for the rest of the
process lifetime, which would make it impossible to toggle live once a
one-shot flag has already fired). It uses the same proven methodology as the
historical Milestone 4.5 investigation: a `vkCmdCopyImageToBuffer` of
VRChat's own imported swapchain image, at the exact array layer WiVRn is
about to read (`data.sub.array_index`), straight to a raw RGBA file —
inserted in the fast-path loop right after `get_layer_image()` and *before*
`get_image_view()`/anything else touches the image. This captures VRChat's
content exactly as Monado handed it to us, before foveation, before
encoding, before anything WiVRn-specific.

Captured on the Adreno tablet (SM-X810) with VRChat actually rendering (not
its loading splash — the one-shot had to be flipped on live via `setprop`
once frames were confirmed flowing, since the cached-macro version would
otherwise always capture the first, black, loading-screen frame). Raw
buffers are 1728×1910 RGBA (matches VRChat's real swapchain extent, same
size seen in the earlier Pixel AHardwareBuffer-allocation error).

**Result**: a column-wise edge-strength scan (cumulative per-row color delta
between adjacent columns, sampled across the full 1910-row height) found:
- Eye 0 (left): no standout column — the highest cumulative diff is ~440,
  consistent with ordinary gradient/dithering noise and the floating login
  panel's own diagonal edge.
- Eye 1 (right): column x≈383 (of 1728) has a cumulative diff of **~4200**
  — roughly 10x every other column — a hard, consistent vertical seam
  running the full image height, isolated to this one column, present only
  in eye 1.

This is decisive: **the seam already exists in the raw content Monado handed
us, before WiVRn's own foveation shader or any other WiVRn-specific
processing ever runs.** This clears `foveation.comp` and the fast-path
image-view/barrier code (already suspected innocent since it's shared
Monado code, confirmed above to be identical regardless of driver or process
topology) of blame.

Combined with the driver isolation above, this points at a **Turnip/Mesa
driver bug**, not anything in WiVRn's code or architecture: same
out-of-process/IPC/AHardwareBuffer-sharing design on both sides, same
`comp_vk_client.c` import code on both sides, only the driver differs, and
the corruption is provably already present in the bytes Turnip/Monado handed
us before any WiVRn-specific code runs. Reading/importing array layer ≥1 of
a multi-layer AHardwareBuffer-backed image is a genuinely narrow,
under-exercised Vulkan/Android feature combination — exactly where a young,
still-actively-developed open-source driver like Turnip is far more likely
to have a real bug than the mature vendor-shipped proprietary Adreno driver.

**Not yet done**: confirming this against Turnip's own issue tracker/recent
commits (a fix or existing report may already exist upstream), and testing
whether a single-array-layer-per-eye (non-multiview, two separate
swapchains) VRChat path avoids the seam entirely, which would further
confirm multi-layer AHardwareBuffer import specifically (not AHardwareBuffer
import in general) as the trigger. Not something fixable in this repo — the
fix, if one doesn't already exist upstream, belongs in Turnip/Mesa itself.

## Milestone 11 (new device, still open) — Galaxy S22 (Exynos 2200 / Xclipse 920): MdiEx driver test, VRChat renders nothing

Separate device, separate investigation, prompted by testing whether
`avavo/MdiEx` (a Samsung Xclipse driver package, bundled for Winlator/DXVK
game-compatibility use, not built for this project) could help or hurt
anything here. **Key finding: our own existing adrenotools-based custom
driver mechanism needed zero code changes to work on this non-Adreno
device** — confirmed from adrenotools' own real hook source
(`bylaws/libadrenotools`, `hook_impl.cpp`): its `hook_android_dlopen_ext`
only checks `strstr(filename, "vulkan.")`, the generic Android HAL driver
naming convention (`vulkan.<ro.hardware.vulkan>.so`, `vulkan.samsung.so` on
this device) used by every GPU vendor, not anything Adreno-specific. The one
real Adreno-only piece (GSL memory-mapping hooks, `ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT`)
is a separate opt-in feature flag this project's own `vulkan_loader.cpp`
never sets. Repackaged MdiEx's raw `vulkan.xclipse2.0.0.so` (extracted
Samsung driver, no Vortek/Wine-compat layer — that's irrelevant for a native
Vulkan app) with a hand-written ADPKG-compatible `meta.json`, and it loaded
and ran successfully through the existing Settings UI with no new code.

**What was ruled out**: an initial theory (zero AHardwareBuffer-exportable
depth formats reported: `VK_FORMAT_D32_SFLOAT`/`D16_UNORM`/`D24_UNORM_S8_UINT`/
etc. all "not supported") was directly disproven — the identical pattern
appears with the *stock* system driver too, confirmed by resetting to system
default and re-testing. Not a MdiEx regression; very likely unrelated to
normal (non-AHardwareBuffer-export) rendering entirely.

**Confirmed, with real evidence**: VRChat has never successfully rendered on
this device (unlike the Adreno tablet) — audio and world logic run fine
(confirmed: ambient audio plays, session stays connected), but the headset
receives solid black frames, and — using the same pre-encode NV12 dump
technique from Milestone 10 — the frame is **already solid black before our
compositor/encoder/driver choice ever touches it**, identically on both the
stock driver and MdiEx. This rules out our server code, the encoder, and the
driver choice entirely: VRChat's own Vulkan rendering successfully creates
and imports its swapchain (correct format `VK_FORMAT_R8G8B8A8_SRGB`, correct
1728x1910 size, logged via `comp_swapchain_import_init`) but never actually
draws real content into it.

A related but likely-separate symptom found in the same session: VRChat's
`AVProVideo` plugin (its in-world video player) continuously throws
`GL_INVALID_FRAMEBUFFER_OPERATION` from an **ANGLE-on-Vulkan** OpenGL ES
context (`glRenderer: ANGLE ((Samsung Xclipse 920) on Vulkan 1.3.279)`) — a
separate rendering subsystem from VRChat's main scene, which uses native
Vulkan directly. Not yet confirmed whether this is the actual cause of the
blank main render or an unrelated broken subsystem; not chased further this
session. This looks like a VRChat/Unity-on-Xclipse compatibility problem
specific to VRChat's own rendering, not something fixable by changing our
Vulkan driver — no proprietary or open driver swap changed the outcome.

## Milestone 12 (Adreno tablet, blocked upstream) — VK_LAYER_KHRONOS_synchronization2 gets past the original blocker, then hits a second, separate stock-driver bug

Prompted by Milestone 10's conclusion that the right-eye seam is a Turnip/Mesa
bug, not ours: since Turnip is *only* needed on this tablet because the stock
Adreno driver lacks `VK_KHR_synchronization2` (the original Milestone 10
blocker, `GPU does not support Vulkan synchronization2 feature`), running on
the stock driver instead (mature, no known AHardwareBuffer-array-layer bug)
would sidestep Turnip entirely — *if* the missing extension can be supplied
some other way.

**The fix attempted**: `VK_LAYER_KHRONOS_synchronization2`
(KhronosGroup/Vulkan-ExtensionLayer) is a real, official, portable
implementation of `VK_KHR_synchronization2` for drivers that lack it
natively — pure API-ergonomics translation (submit2/barrier2/etc. down to
the classic Vulkan 1.0/1.1 primitives), no new hardware capability required.
Android has a real, no-root mechanism for exactly this: a non-debuggable app
can bundle a layer's `.so` directly in its own native library directory
(`libVkLayer_*.so` naming) and enable it explicitly via `vkCreateInstance`'s
`ppEnabledLayerNames` — confirmed via Android's own NDK docs, no system
install or rooting needed.

**Implementation** (this work lives on `experiment/vulkan-sync2-compat-layer`,
branched from `feat/adrenotools-turnip`, not merged into it — it's a
sibling alternative to Turnip loading, and ultimately blocked, see below):
- Vendored `Vulkan-Headers` + `Vulkan-Utility-Libraries` + `Vulkan-ExtensionLayer`
  (top-level `CMakeLists.txt`, Android-only fetch site in `server/CMakeLists.txt`,
  all three pinned to `vulkan-sdk-1.4.328.1` — matching the Vulkan header
  version already vendored at `tools/vulkan-headers` (`VK_HEADER_VERSION 328`),
  not the newest available tag. This matters: `Vulkan-Headers`' own
  `add_library(Vulkan::Headers ALIAS ...)` collides with a target of the same
  name CMake's own bundled `FindVulkan.cmake` module already creates
  elsewhere in this build (both guard with `if (NOT TARGET Vulkan::Headers)`
  as of CMake 3.24+, so whichever runs first silently wins) — a version
  mismatch between the two would then silently compile
  `Vulkan-Utility-Libraries` (pinned to the newer tag) against the *other*,
  older header set, which really did happen once during this work
  (`unknown type name 'VkPhysicalDeviceShaderAbortFeaturesKHR'`) before the
  tags were aligned. `patches/vulkan-headers/0001-...patch` adds that guard
  (upstream doesn't have it) to fix the actual collision, once versions were
  already aligned.
- `server-app/build.gradle`: added `VkLayer_khronos_synchronization2` to the
  CMake `targets` list, same mechanism as adrenotools' hook libraries.
- `server/utils/wivrn_vk_bundle.cpp`: requests the layer at instance creation
  (mirrors the existing `VK_LAYER_KHRONOS_validation` pattern exactly — query
  `enumerateInstanceLayerProperties()`, push the name if present, no-op
  elsewhere since the layer isn't bundled on desktop). Requesting it
  unconditionally is safe: without `VK_SYNCHRONIZATION2_FORCE_ENABLE` set (not
  set here), the layer is a no-op passthrough on any driver that already has
  native synchronization2 (Pixel, S22).
- `server/utils/wivrn_vk_bundle.{h,cpp}`: the synchronization2 *feature*
  query/enable needed a real fix, not just the extension: the existing code
  unconditionally chained `vk::PhysicalDeviceVulkan13Features` (the Vulkan
  1.3 core aggregate struct) to check/enable `synchronization2` — invalid on
  a device reporting apiVersion 1.1 (this tablet's stock driver), which never
  populates that struct regardless of what layers are active. Added
  `vk::PhysicalDeviceSynchronization2FeaturesKHR` (the original, discrete
  per-extension struct, which the layer *does* correctly answer) as a
  fallback query when the 1.3 struct comes back false, with
  `vk::StructureChain::unlink<T>()` used to keep only one of the two structs
  in the actual `vkCreateDevice` pNext chain (chaining both, even
  harmlessly, violates `VUID-VkDeviceCreateInfo-pNext-06532`).

**Result, live on the tablet**: real progress, then a real, different wall.
With no custom driver configured (stock Adreno driver) the server no longer
hard-fails at the synchronization2 check — logcat confirms
`added global layer 'VK_LAYER_KHRONOS_synchronization2' ... Loaded layer
VK_LAYER_KHRONOS_synchronization2`, encoders get created, the server reaches
its normal idle state. VRChat, connected through it, gets all the way to
`xrCreateSwapchain` (further than the original blocker ever allowed). Then
the **server itself segfaults** — `SIGSEGV`/`SEGV_MAPERR`, fault addr
`0x28`, inside `qglinternal::vkQueueSubmit` in the stock driver
(`/vendor/lib64/hw/vulkan.adreno.so`), reached via the layer's translation
of `wivrn::compositor::layer_commit()`'s per-frame `vkQueueSubmit2` call
(`compositor.cpp`'s frame-submit site, which signals a real timeline
semaphore — `vk::SemaphoreSubmitInfo{.value = ++sem_value, ...}`) down to a
classic `vkQueueSubmit` with a chained `VkTimelineSemaphoreSubmitInfo`.

A first hypothesis — that the layer unconditionally chains
`VkTimelineSemaphoreSubmitInfo`/`VkDeviceGroupSubmitInfo` onto *every*
translated submit whenever those device *features* are enabled at all,
regardless of whether that specific submit actually uses them — turned out
to be real (confirmed by reading `Vulkan-ExtensionLayer`'s own
`synchronization2.cpp`) and worth fixing regardless
(`patches/vulkan-extensionlayer/0001-...patch`, only chain when the struct
actually has non-zero wait/signal counts), but **did not fix this specific
crash** — this submission legitimately uses a timeline semaphore signal, so
the struct was already correctly being chained either way. Retested with the
patch applied: identical crash, same fault address, same stack.

**Conclusion**: this is a second, separate stock-driver bug from the
original missing-extension gap — the stock Adreno driver's own classic
`vkQueueSubmit`, when handed a `VkTimelineSemaphoreSubmitInfo`-chained
submission (exactly what any synchronization2-emulation layer *must*
produce, since the driver has no native `vkQueueSubmit2` to call directly),
crashes. Not something patchable in the compat layer — the translated input
is spec-correct; the driver's handling of it is broken. Blocked upstream
(Qualcomm's proprietary driver), same as the Turnip AHardwareBuffer bug is
blocked upstream in Mesa. **This branch does not fix the tablet** — Turnip
(`feat/adrenotools-turnip`, live with the known right-eye seam) remains the
only working path on this hardware for now.

## Milestone 13 (new device, confirms the isolation) — Samsung Galaxy Z Fold 7 (Snapdragon 8 Elite / Adreno 830): stream works perfectly, no seam, no crash

Tested the same server build (this branch,
`experiment/vulkan-sync2-compat-layer`) on a Galaxy Z Fold 7 instead of the
Adreno 642L tablet. Result: streaming works cleanly — no right-eye seam
(Milestone 10/10-follow-up), no `vkQueueSubmit` segfault (Milestone 12). This
device's stock Adreno 830 driver supports `VK_KHR_synchronization2` natively,
so it never needs Turnip or the sync2 compat layer in the first place —
removing both suspected fault paths at once.

This is the confirmation Milestone 10's follow-up predicted: the right-eye
seam and the sync2-crash are properties of the Adreno 642L tablet's
particular driver stack (forced onto Turnip for the seam bug; stock driver
segfaults on translated `VkTimelineSemaphoreSubmitInfo` submits for the crash
bug), not of WiVRn's architecture, this project's Android port, or Adreno
GPUs in general. A newer SoC/driver with native synchronization2 support
sidesteps both issues entirely. **Not a fix** — the SM-X810 tablet remains
blocked upstream in Turnip/Mesa (seam) and the stock driver (segfault) — but
it confirms nothing else in this codebase needs to change for that hardware
class; the ceiling is the vendor driver, not this repo.

## Milestone 14 (new device, open) — Samsung Galaxy S26 Ultra (Snapdragon 8 Elite Gen 5 / Adreno): client connects, nothing streams once the OpenXR app launches

Confirmed, not yet root-caused: the client successfully connects to the
server (handshake/control channel over TCP/9757 completes), the target
OpenXR app launches, but no video ever arrives. Confirmed **app-agnostic** —
every app tried (not just VRChat) fails the same way on this device. This is
the key difference from Milestone 11's Xclipse S22 case, where only VRChat
fails and the reference app/Somar work fine — the S26 Ultra failure is a
different, more fundamental break, not a VRChat-specific compatibility
problem. No logs captured yet (no server-side device connected this
session).

**Next steps when the device is available**: `logcat -c` before launch, then
`logcat -d` after, on both the phone (server) and headset (client) — check
for encoder creation failure (`video_encoder_mediacodec.cpp`, same class of
bug as the earlier HEVC hardcoded-codec crash, Milestone 7 area), whether
`xrCreateSwapchain`/`xrBeginFrame` are even reached, and whether any frames
leave the server at all (`WIVRN_DUMP_VIDEO`, see this doc's capture section
above). Silent-no-stream, app-agnostic, suggests the failure is earlier in
the pipeline than Milestones 10-13 — likely encoder init/codec negotiation
given this is a new Snapdragon generation, not yet confirmed.

## Milestone 15 (third-party test, open) — sync2 compat layer confirmed necessary and non-crashing on another device; audio streams, no video

A friend tested `experiment/vulkan-sync2-compat-layer` on a different phone
(not this project's own devices, not connected here — model/SoC not yet
recorded). Two things confirmed:
- That device's stock driver genuinely lacks native `VK_KHR_synchronization2`
  too (same class of gap as the SM-X810 tablet, Milestone 12) — the sync2
  compat layer was necessary for the server to run at all there, not just a
  tablet-specific workaround.
- Unlike the SM-X810 tablet, the stock driver here does **not** segfault on
  the layer's translated `VkTimelineSemaphoreSubmitInfo`-chained submits —
  the server runs normally. This confirms Milestone 12's `vkQueueSubmit`
  crash is specific to that tablet's driver, not an inherent property of the
  sync2 compat layer approach itself.

New symptom, not yet investigated: audio streams to the Quest 1 correctly,
but no video ever displays. Since audio and video are independent pipelines
server-side, this points at the video encode/send path specifically (encoder
creation, frame production, or transport), not a session/connection-level
failure. No logs captured — different setup, no device connected to this
machine.

## Milestone 16 (open) — HEVC (H265) encoder path: app starts, stuck on lobby, no frames reach the Quest 1

Distinct from the per-device streaming failures above — this is a codec-path
bug, reproduced with the HEVC MediaCodec encoder selected instead of H264.
The app starts normally and the client reaches the lobby, but never
progresses past it: no frames ever arrive at the Quest 1. Not yet
root-caused; H264 on the same hardware does not show this, so the fault is
somewhere in the HEVC-specific path (`video_encoder_mediacodec.cpp`'s HEVC
configuration, or codec negotiation — the same general area as the earlier
hardcoded-codec-name crash, Milestone 7). Needs a logcat capture with HEVC
forced on to make progress.

## Milestone 17 (open) — Galaxy Note 20 Ultra (Adreno 6xx-class): runs VRChat, but really laggy

**Correction first**: "Galaxy Tab S7 FE" in earlier notes on this was the
same physical device already covered in Milestones 8-13 as "the SM-X810
tablet" — one device, not two. Its consumer name is Galaxy Tab S7 FE
(SM-X810, Snapdragon 778G / Adreno 642L). So its "VRChat runs but is
visibly glitchy" symptom is almost certainly just Milestone 10's
already-diagnosed right-eye stereo-fusion seam (a Turnip/Mesa driver bug,
not something fixable here) showing up again, described informally —
not a new, separate mystery. Nothing new to investigate there; see
Milestone 10/10-follow-up for the actual root cause.

**The real new data point**: Galaxy Note 20 Ultra (Adreno 6xx family,
same generation as the SM-X810/Tab S7 FE above but a newer chip within
that family) runs VRChat successfully — rendering is correct, no seam, no
crash — but performance is really laggy. Unlike the SM-X810's seam, this
isn't yet explained by a known root cause, and the fact that the newer
chip still lags argues against a simple "old/weak GPU" explanation.
Since the SM-X810's own degradation turned out to have a specific,
already-diagnosed driver-bug cause rather than a general hardware
ceiling, the "older Adreno 6xx generation is RAM/GPU-throughput
constrained" theory floated earlier is weaker than it looked — the two
devices' symptoms likely have different causes, not one shared one. Still
open: not yet profiled, no memory-pressure/GPU-frame-time data captured on
the Note 20 Ultra.

## Milestone 18 — speaker audio (game → headset) implemented via AudioPlaybackCaptureConfiguration; two real server-thread stalls found and fixed live

The desktop server's only audio backend is PipeWire (`server/audio/audio_pipewire.cpp`) — Android has no PipeWire, and `server-app/build.gradle` sets `WIVRN_USE_PIPEWIRE=OFF`, so `audio_device::create()` (`audio_setup.cpp`) previously just logged "No audio backend available" and returned null. No audio ever reached the headset; the local OpenXR app's sound just played out the phone's own speaker like any normal Android app, since nothing captured or redirected it.

**Implementation**: `android_audio_device` (`server/android/wivrn_server_jni.cpp`, wired into `audio_setup.cpp`/`audio_setup.h` via a new `create_android_audio_handle()` declared `#ifdef __ANDROID__`, stubbed to return null in `wivrn_server_android_stub_main.cpp` for the non-JNI executable variant). Speaker-only — no virtual-microphone equivalent yet, see below.

- **Server side (native)**: mirrors `audio_pipewire.cpp`'s `pipewire_device` shape — `resume()` sends `to_headset::audio_stream_description`, `on_audio_data()` (called from Java) does `session.send_control(audio_data{...})` exactly like `speaker_process()` does. `process_mic_data()` is a no-op stub.
- **Capture (Java, `WivrnServerService.java`)**: `AudioPlaybackCaptureConfiguration` (API 29+, restricted to `USAGE_GAME`/`USAGE_MEDIA`/`USAGE_UNKNOWN`) feeding a plain `AudioRecord`, read in a background thread that forwards each buffer to native via `nativeAudioData()`.
- **Consent (`MainActivity.java`)**: `AudioPlaybackCaptureConfiguration` needs a `MediaProjection` token, which only comes from the system's screen-recording-style consent dialog — there's no audio-only variant of that prompt. Requested automatically at server start (RECORD_AUDIO permission, then the projection intent), not lazily on first headset connect, so it doesn't interrupt an already-streaming session.
- **Manifest**: `RECORD_AUDIO`, `FOREGROUND_SERVICE_MEDIA_PROJECTION` permissions; `WivrnServerService`'s `foregroundServiceType` gained `mediaProjection` alongside the existing `connectedDevice`.

**Bug #1, found live**: `WivrnServerService.onStartCommand()`'s very first `startForeground()` call (2-arg form) defaults to the manifest's *entire* declared type set — so it was validated against `mediaProjection`'s requirements before the app had ever shown the consent dialog, let alone been granted it. `SecurityException: Starting FGS with type mediaProjection ... requires ... android:project_media`, crashing the service on every launch. Fixed: the initial call now explicitly passes `FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE` only; a second explicit call (with both types) happens once a projection is actually about to be used.

**Bug #2, found live, even earlier than expected**: `MediaProjectionManager.getMediaProjection()` *itself* throws `SecurityException: Media projections require a foreground service of type ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION` unless that type is already active *before* that call — not just before actually starting capture, which is where the type-upgrade call was originally placed. Fixed by moving `WivrnServerService.upgradeForMediaProjection()` to run in `MainActivity.onActivityResult()`, immediately before `getMediaProjection()`, via a static `instance` reference (same process, no Binder needed).

**Bug #3, the real regression** (confirmed via a controlled A/B — reverted these changes on the exact same tablet, streaming worked; re-applied them, it didn't): `onAudioStreamStart()`/`onAudioStreamStop()` (the native→Java callbacks fired from inside `wivrn_session`'s constructor, itself running on the server thread) called `startAudioCapture()`/`stopAudioCapture()` **directly and synchronously**, unlike every other callback in this file (`onHeadsetConnected` etc.), which only ever posts to a `Handler` and returns immediately. Since the JNI call (`CallVoidMethod`) blocks its caller until the Java method returns, and `AudioRecord.Builder().build()` with a capture config does real binder work, this stalled the server thread mid-construction of the session — the compositor never got a chance to start. Symptom: the headset connects fine (that happens before this point), but nothing ever streams, and the local OpenXR app (VRChat) times out waiting for a runtime session and falls back to flat 2D — the *exact* symptom independently reported for the Galaxy S26 Ultra (Milestone 14), but here it was a self-inflicted regression, not a device issue. Fixed by dispatching both calls onto a plain background thread instead of running inline.

**Echo/local playback**: `AudioPlaybackCaptureConfiguration` is a capture/tap API (built for recording gameplay audio while still hearing it normally), not a routing change like real screen-cast/Chromecast (which registers a virtual *output device*, so the OS's normal routing logic never plays it locally at all). Capturing alone doesn't stop the phone's own speaker from also playing the sound, which produced an audible echo/delay between the immediate local copy and the network-relayed one reaching the headset. Fixed by muting `AudioManager.STREAM_MUSIC` (what `USAGE_GAME`/`USAGE_MEDIA` map to for volume control) once capture is confirmed running, and unmuting on stop — the captured mix itself isn't affected by stream volume/mute, only the physical output is.

**Verified working end to end** on the Galaxy Tab S7 FE (SM-X810) + Quest 1: audio reaches the headset only, no echo, no server-thread stall.

**Not done yet**: microphone forwarding (headset mic → local OpenXR app's input). Unlike speaker capture, Android has no unprivileged equivalent to PipeWire's virtual-source trick — there's no public API for a normal app to register a system-wide virtual recording device that another app's `AudioRecord` would pick up; the privileged `AudioPolicy`/`MODIFY_AUDIO_ROUTING` mechanism (signature-level permission) is what real virtual-mic apps rely on, and isn't available to a normal installed app without root or system-app status. Being investigated as a follow-up, including whether the headset could instead pair as a real Bluetooth (LE Audio) peripheral to the phone, which would make both directions (speaker *and* mic) "just work" through the OS's own normal Bluetooth audio routing instead of needing either of these custom capture/injection mechanisms.

## Key architecture facts worth remembering (established by reading real
source and by running the real thing on-device, not assumed)

- WiVRn's compositor (`server/compositor/compositor.cpp`) renders the
  final distorted/composited stereo image **directly into the same image
  that gets encoded** — confirmed zero `vkCmdCopyImage`/readback/extra
  render pass anywhere in that path. The `video_encoder` interface's
  `present_image(vk::Image y_cbcr, ...)` receives that image directly.
- **This compositor never touches a physical on-screen `Surface` at all**
  — unlike the earlier, separate Monado+Cardboard experiment (Unity app +
  `comp_window_android.c`, a completely different code path), WiVRn's own
  compositor was built from the start to composite → encode → send over
  network, never to display locally, on desktop too (the desktop
  `wivrn-server` process has no visible window). So there is nothing to
  suppress or redirect — the phone's screen during actual streaming shows
  only whatever this app's own UI displays, nothing else.
- **`ipc_server_main_common()` calls `create_system()` immediately at
  startup**, not lazily on first client — see Milestone 2's crash #2 above.
  Anything calling this function must have already populated the global
  `connection` (`wivrn_ipc.h`) first.
- Monado (vendored via `FetchContent`, pinned in `monado-rev`, currently
  resolving to `f037264d2` at time of writing — check `build-android/_deps/monado-src`
  or `server-app/.cxx/*/arm64-v8a/_deps/monado-src` for what actually got
  fetched) already has real, working Android support throughout — this is
  not something being added, it's being *used*. It does have at least one
  real portability bug of its own (missing `#include <vector>`, see the
  toolchain table above) — patched via this project's existing
  `patches/monado/*` mechanism, not hand-edited.
