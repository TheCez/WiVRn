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

## Milestone 4.5 — DONE: root-caused and fixed the green-artifact/chroma
## corruption bug; found (not yet fixed) a second, separate frame-desync bug

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
