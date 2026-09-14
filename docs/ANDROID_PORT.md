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
devices connected" (screenshot-verified). No real headset has connected
yet — that's the next real unknown, not yet exercised.

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
- **No PIN/pairing security flow.** `encryption_state::enabled` with an
  empty pin, unconditionally. Desktop's actual pairing UX (the dashboard's
  "add new device" flow, PIN entry, `encryption_state::pairing`) isn't
  represented at all yet.
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

## Milestone 3+ — not started

- Real hardware video encoder. **Confirmed via an on-device Vulkan
  extension probe (cross-compiled with the NDK, run via `adb shell`)**:
  this Pixel 10 Pro XL has **zero** Vulkan Video support (`VK_KHR_video_queue`,
  `VK_KHR_video_encode_queue`, etc. all absent from all 161 device
  extensions) — so `encoder/video_encoder_vulkan.cpp` is not usable here.
  The device *does* expose `VK_ANDROID_external_memory_android_hardware_buffer`
  (spec 5) — the mechanism a `video_encoder_mediacodec.cpp` backend would
  use to export the compositor's rendered `VkImage` as an `AHardwareBuffer`
  for a `MediaCodec` encoder's input `Surface`, genuinely zero-copy. Not
  written yet; `encoder/video_encoder_raw.cpp` (uncompressed, already
  building) is the only backend right now.
- PIN/pairing security flow (see Milestone 2's simplifications above).
- Android `NsdManager`-based discovery to replace Avahi.
- An audio backend (`WIVRN_USE_PIPEWIRE=OFF`, no Android audio path written;
  `audio/audio_setup.cpp` already degrades gracefully to "no audio backend"
  so this compiles fine as-is, just doesn't do anything).
- Actually pairing with and streaming to a Quest 1 end-to-end. The server
  listens and doesn't crash on a bogus connection, but no *real* WiVRn
  client has connected yet — that's the next genuine unknown.

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
