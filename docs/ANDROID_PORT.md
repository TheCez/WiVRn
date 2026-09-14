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

Read this file before touching anything here again — it has the toolchain
paths, exact commands, and *why* behind non-obvious decisions, so none of
it needs rediscovering.

## Toolchain — entirely self-contained under `ForeverXR/tools/`

**Never touches Unity's own NDK/SDK** (`~/Unity/Hub/Editor/6000.3.23f1/.../PlaybackEngines/AndroidPlayer/...`) —
explicit user requirement, since that install must keep building for Quest 1
via Unity. Everything below was downloaded fresh, read-only reuse only.

| Tool | Path | Why a separate copy was needed |
|---|---|---|
| CMake 3.31.6 | `ForeverXR/tools/cmake/bin/cmake` | WiVRn requires CMake ≥3.28; Unity's bundled Android SDK cmake is 3.22.1, and no `cmake`/`ninja` exist on the host at all |
| Ninja | Unity's Android SDK bundles one at `~/Unity/Hub/Editor/6000.3.23f1/.../SDK/cmake/3.22.1/bin/ninja` (1.10.2) — this one *is* reused read-only, since it's just a build tool with no version sensitivity here | — |
| NDK r28b (Clang 19) | `ForeverXR/tools/ndk` | Unity's bundled NDK's libc++ is missing `std::jthread`/`std::stop_token` entirely (not just a flag gate) at the API levels tried. A newer NDK was needed; even r28b/Clang 19 still needs `-fexperimental-library` for jthread/stop_token, and still has **no** `std::ranges::enumerate_view` implementation at all (see below) |
| Vulkan-Headers 1.4.328 | `ForeverXR/tools/vulkan-headers` | The NDK's bundled Vulkan headers report `VK_HEADER_VERSION` too old for WiVRn's `Vulkan_VERSION >= 1.4.304` check. Passed via `-DVulkan_INCLUDE_DIR=...`; the NDK's own `libvulkan.so` stub is still used for linking (headers/stub-lib version need not match — the real driver is what's on-device at runtime, confirmed 1.4.317 there) |
| glslangValidator | `ForeverXR/tools/glslang/extracted/usr/bin/glslangValidator` | Needed as a **host** tool (compiles shaders to SPIR-V at build time) by both Monado's `cmake/SPIR-V.cmake` (`GLSLANGVALIDATOR_COMMAND`) and WiVRn's own `cmake/CompileGLSL.cmake` (`Vulkan::glslangValidator` imported target, via `Vulkan_GLSLANG_VALIDATOR_EXECUTABLE`). Obtained via `apt-get download glslang-tools` + `dpkg-deb -x` (no root needed — `apt-get download` doesn't require sudo, unlike `apt-get install`) |
| Eigen3, nlohmann_json, CLI11 | `ForeverXR/tools/prefix` (a real install prefix — `cmake --install`'d there, header-only libs so no cross-compilation needed) | Not present on the host at all, and this codebase has no `FetchContent` fallback for these three (unlike Boost, which it does fetch itself when `WIVRN_USE_SYSTEM_BOOST=OFF`) |

No `sudo` was used anywhere in this toolchain (deliberately — avoids needing
an interactive password mid-session). `apt-get download` (not `install`)
worked without root for glslang-tools.

## Exact working commands

Two build variants share one CMake option, `WIVRN_ANDROID_JNI` (default
`OFF`):
- **OFF** (default): `wivrn-server` is a plain ELF executable — push with
  `adb push` and run directly, fastest iteration loop, no APK needed.
- **ON**: `wivrn-server` is `libwivrn-server.so`, JNI-loadable, for the
  Android Service wrapper (Milestone 2).

```bash
CMAKE=/home/thecez/VR_Development/projects/ForeverXR/tools/cmake/bin/cmake
NINJA=/home/thecez/Unity/Hub/Editor/6000.3.23f1/Editor/Data/PlaybackEngines/AndroidPlayer/SDK/cmake/3.22.1/bin/ninja
NDK=/home/thecez/VR_Development/projects/ForeverXR/tools/ndk
SRC=/home/thecez/VR_Development/projects/ForeverXR/wivrn-android
VKHDR=/home/thecez/VR_Development/projects/ForeverXR/tools/vulkan-headers/include
PREFIX=/home/thecez/VR_Development/projects/ForeverXR/tools/prefix
GLSLANG=/home/thecez/VR_Development/projects/ForeverXR/tools/glslang/extracted/usr/bin/glslangValidator

"$CMAKE" -G Ninja -B "$SRC/build-android" -S "$SRC" \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-34 \
  -DANDROID_STL=c++_static \
  -DVulkan_INCLUDE_DIR="$VKHDR" \
  -DVulkan_GLSLANG_VALIDATOR_EXECUTABLE="$GLSLANG" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
  -DGLSLANGVALIDATOR_COMMAND="$GLSLANG" \
  -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_SERVER_LIBRARY=OFF -DWIVRN_BUILD_CLIENT=OFF -DWIVRN_BUILD_DASHBOARD=OFF \
  -DWIVRN_BUILD_WIVRNCTL=OFF -DWIVRN_BUILD_DISSECTOR=OFF -DWIVRN_BUILD_TEST=OFF \
  -DWIVRN_USE_SYSTEM_BOOST=OFF -DWIVRN_USE_SYSTEM_FREETYPE=OFF \
  -DWIVRN_USE_NVENC=OFF -DWIVRN_USE_VAAPI=OFF -DWIVRN_USE_VULKAN_ENCODE=OFF \
  -DWIVRN_USE_X264=OFF -DWIVRN_USE_PIPEWIRE=OFF \
  -DWIVRN_FEATURE_STEAMVR_LIGHTHOUSE=OFF -DWIVRN_FEATURE_SOLARXR=OFF \
  -DWIVRN_OPTIMIZE_SHADERS=OFF \
  -DWIVRN_ANDROID_JNI=OFF   # or =ON for the .so variant

"$NINJA" -C "$SRC/build-android" wivrn-server
```

Test on device (either variant — both need the self-built `libcrypto.so`
alongside them; Android's *system* `libcrypto.so` is not a stable/complete
public API and is missing symbols this OpenSSL 3.6.0 build uses, e.g.
`EVP_PKEY_encapsulate_init`):

```bash
adb push build-android/server/wivrn-server /data/local/tmp/   # or libwivrn-server.so
adb push build-android/_deps/openssl/lib/libcrypto.so /data/local/tmp/
adb shell chmod 755 /data/local/tmp/wivrn-server
adb shell "LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/wivrn-server"   # expect exit 0, placeholder main()
```

For the `.so` variant, confirm the JNI exports instead of running it directly
(a `.so` isn't a runnable executable):
```bash
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" -D build-android/server/libwivrn-server.so | grep native
```

## Milestone 1 — DONE: `wivrn-server` compiles, links, and runs on-device

Commit `15fb6ae8`. Proved the reusable core (compositor, driver/tracking,
`video_encoder` base + `raw` backend, Monado itself) actually builds under
the Android NDK before investing in app packaging.

| File | What / why |
|---|---|
| `CMakeLists.txt`, `server/CMakeLists.txt`, `common/CMakeLists.txt` | Gate every desktop-only dependency (GDBus codegen, Avahi, glib2, libnotify, librsvg/LibArchive/PNG, native_app_glue) behind `NOT ANDROID`; the "no encoder selected" check now also allows Android (falls back to the always-built `raw` encoder) |
| `server/hostname.cpp` | GDBus hostname lookup wrapped in `#ifndef __ANDROID__`; already had a `gethostname()` fallback for when D-Bus fails, Android just always takes that path |
| `server/driver/wivrn_uinput_android.cpp` (new) | No-op sibling of `wivrn_uinput.cpp` (`/dev/uinput` inaccessible to unprivileged apps) — same class from the shared header, since `wivrn_session.h` holds a real `std::optional<wivrn_uinput>` member |
| `server/android/wivrn_server_android_stub_main.cpp` (new) | Placeholder entry point replacing `main.cpp` (D-Bus dashboard IPC — GDBus/glib/libnotify) and its exclusive dependents `avahi_publisher.cpp`, `sleep_inhibitor.cpp`, `start_systemd_unit.cpp`, `start_application.cpp`, `active_runtime.cpp` (Linux OpenXR-runtime-manifest registration — Android uses a completely different broker mechanism) |
| `server/android/wivrn_ipc_socket_monado_stub.cpp` (new, later split out of the stub-main file) | `wivrn_ipc_socket_monado` (talks to a separate desktop Monado *process* that doesn't exist here — this build links Monado directly in-process) is given a real connected loopback socketpair instead of staying `std::nullopt`, since ~20 call sites across `wivrn_session.cpp`/`wivrn_connection.*` dereference it unconditionally — leaving it empty would be undefined behavior the first time any of them ran |
| `server/accept_connection.cpp` | Its desktop-only side effect (notifying that same separate Monado process) `#ifdef`'d out in place; the rest of the function (accepting the headset's TCP connection) is genuinely needed on Android too |
| `server/driver/wivrn_session.cpp` | `get_application_list` handler (Steam/Flatpak app picker) replies with an empty list on Android instead of calling `wivrn-common-server` (PNG/librsvg/LibArchive) — there's nothing to list, the streamed app is this process itself |
| `common/utils/enumerate_polyfill.h` (new) | `std::ranges::enumerate_view` (C++23) isn't implemented by this NDK's libc++ at all (checked: `__ranges/zip_view.h` exists, `__ranges/enumerate_view.h` doesn't) — polyfills the `for (auto&& [i,x] : enumerate_view(r))` usage via `zip`+`iota`, guarded by `__cpp_lib_ranges_enumerate` so it steps aside the moment a real implementation ships |
| `server/driver/configuration.cpp` | Unused `to_iso8601(std::chrono::zoned_time<T>)` overload guarded behind the same `__cpp_lib_chrono` feature-test macro the file's *other* overload already used — `zoned_time` needs an IANA timezone database this libc++ wasn't built with |
| `server/CMakeLists.txt` | `-fexperimental-library` added for the Android target (unlocks `jthread`/`stop_token`/`zoned_time`'s header declarations, though not `zoned_time`'s actual timezone-db-dependent functionality — see above) |

## Milestone 2 — IN PROGRESS: JNI entry point + Android Service wrapper

Not committed yet (uncommitted in the working tree as of this writing —
commit once reviewed).

**Done:**
- `WIVRN_ANDROID_JNI` CMake option (default OFF) — builds `wivrn-server` as
  `libwivrn-server.so` instead of an executable when ON, sharing the exact
  same source list (`_wivrn_server_sources` variable in
  `server/CMakeLists.txt`) so there's no duplication to drift.
- `server/android/wivrn_server_jni.cpp` (new) — real native entry point.
  Key finding that made this small: `main.cpp`'s `start_server()` doesn't
  reimplement anything itself, it just forks and calls Monado's own
  `ipc_server_main_common()` — the actual complete compositor/IPC-server
  entry point, already fully wired via this target's `XRT_*` CMake config.
  Exports `Java_org_meumeu_wivrn_server_WivrnServerService_nativeStart`/
  `nativeStop`, running `ipc_server_main_common()` on a background
  `std::jthread`; `nativeStop` calls Monado's `ipc_server_stop()` on the
  `ipc_server*` captured during the `mainloop_entering` callback (mirrors
  `server/ipc_server_cb.cpp`'s existing pattern, since that class has no
  accessor for what it captures). Verified: builds clean, `libwivrn-server.so`
  exports both symbols (checked with `llvm-nm -D`), same `libcrypto.so`
  dependency as Milestone 1 (fine — packaged into a real APK's
  `lib/arm64-v8a/`, Android's dynamic linker resolves same-APK `.so`s
  automatically, no `LD_LIBRARY_PATH` hack needed there).
- `server/android/java/WivrnServerService.java` (new) — a real foreground
  `Service` (`org.meumeu.wivrn.server` package, matching the client's own
  `org.meumeu.wivrn` naming convention and its flat `client/java/`
  source-dir convention). `System.loadLibrary("wivrn-server")`,
  `nativeStart()` in `onStartCommand`, `nativeStop()` in `onDestroy`,
  `START_NOT_STICKY` (don't silently resurrect a streaming session if
  Android kills the process).
- `server/android/AndroidManifest.xml` (new) — **draft only**, not merged
  into a real manifest yet. Uses Android's `connectedDevice` foreground
  service type + `FOREGROUND_SERVICE_CONNECTED_DEVICE` permission —
  confirmed this is the right category by observing it already in use by
  `org.freedesktop.monado.openxr_runtime`'s own out-of-process runtime
  broker service in `adb logcat` during earlier device testing on this
  exact phone.

**Known, deliberate simplification vs. desktop** (documented in
`wivrn_server_jni.cpp`'s own comment too): `main.cpp`'s `inner_main()` is
actually a *second*, outer session-manager loop — pairing/PIN security,
Avahi publish, its own `create_listen_socket()` — that only calls
`start_server()` once a headset makes initial contact, i.e. the compositor
is normally started on-demand per connection. This JNI entry skips that
tier entirely and just runs the compositor continuously from Service start.
`accept_connection.cpp` (already Android-safe) still does the actual
per-connection TCP accept *inside* the compositor, so a headset can connect
— what's missing is the outer pairing/PIN flow and Avahi-based discovery.
Fine for getting something running end-to-end; needs reconciling with
upstream's actual security model before this is PR-ready.

**NOT done — concrete next step:** none of the above is wired into a
buildable/installable APK yet. There is no Gradle module for it — the
existing `build.gradle` is a single module building only the *client*
(`-DWIVRN_BUILD_SERVER=OFF` is hardcoded into its CMake args). Next
concrete step is one of:
1. Add a second Android application module (new `settings.gradle` +
   module-specific `build.gradle`) that builds with
   `-DWIVRN_ANDROID_JNI=ON -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_CLIENT=OFF`,
   packaging `libwivrn-server.so` + `server/android/java/*.java` +
   `server/android/AndroidManifest.xml` (merged with a real
   `<application>` root) into its own APK; **or**
2. Skip JNI/Gradle-module complexity for now and have a Service launch the
   already-working plain executable as a subprocess instead (Android does
   allow executing files extracted from an APK's own `lib/<abi>/` — this
   is how apps that bundle native CLI tools do it, e.g. via
   `ProcessBuilder`). Genuinely simpler, reuses the Milestone-1 executable
   unchanged, no JNI marshalling at all. Not the direction taken so far
   (the user asked for "the JNI entry point" specifically), but worth
   knowing as the fallback if Gradle-module setup turns out heavier than
   it looks.

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
- Android `NsdManager`-based discovery to replace Avahi (`avahi_publisher.cpp`
  is currently just excluded, no replacement written).
- An audio backend (`WIVRN_USE_PIPEWIRE=OFF`, no Android audio path written;
  `audio/audio_setup.cpp` already degrades gracefully to "no audio backend"
  so this compiles fine as-is, just doesn't do anything).
- Reconciling the Milestone 2 simplification above (outer session-manager
  tier / pairing / on-demand compositor start) with something closer to
  desktop's actual behavior.
- Actually pairing with and streaming to a Quest 1 end-to-end. Nothing
  beyond "the server process runs without crashing" has been verified at
  runtime yet — no client has connected to it.

## Key architecture facts worth remembering (established by reading real
source, not assumed — see the full trace earlier in this project's history
for the receipts)

- WiVRn's compositor (`server/compositor/compositor.cpp`) renders the
  final distorted/composited stereo image **directly into the same image
  that gets encoded** — confirmed zero `vkCmdCopyImage`/readback/extra
  render pass anywhere in that path. The `video_encoder` interface's
  `present_image(vk::Image y_cbcr, ...)` receives that image directly.
- Monado (vendored via `FetchContent`, pinned in `monado-rev`, currently
  resolving to `f037264d2` at time of writing — check `build-android/_deps/monado-src`
  for what actually got fetched) already has real, working Android support
  throughout — this is not something being added, it's being *used*.
