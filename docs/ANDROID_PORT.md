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
