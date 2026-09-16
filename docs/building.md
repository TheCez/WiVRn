# Building

# Server (PC)

## Compile

From your checkout directory, with automatic detection of encoders
```bash
cmake -B build-server . -GNinja -DWIVRN_BUILD_CLIENT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-server
```

It is possible to disable specific encoders, by adding options
```
-DWIVRN_USE_NVENC=OFF
-DWIVRN_USE_VAAPI=OFF
-DWIVRN_USE_VULKAN_ENCODE=OFF
-DWIVRN_USE_X264=OFF
```

Force specific audio backends
```
-DWIVRN_USE_PIPEWIRE=ON
```

Systemd service and pretty hostname support
```
-DWIVRN_USE_SYSTEMD=ON
```

Lighthouse driver support for use with lighthouse-tracked devices
```
-DWIVRN_FEATURE_STEAMVR_LIGHTHOUSE=ON
```

Additionally, if your environment requires absolute paths inside the OpenXR runtime manifest, you can add `-DWIVRN_OPENXR_MANIFEST_TYPE=absolute` to the build configuration.

## Profiling / tracing build

To build the server with Perfetto tracing for local profiling, use the `server-tracing` preset (it is the `server` preset plus `WIVRN_USE_PERFETTO=ON`):

```bash
cmake --preset server-tracing
cmake --build build-server-tracing
```

This requires the Perfetto amalgamated SDK installed where CMake looks for it (`/usr/share/perfetto/sdk/perfetto.{h,cc}`, override with `-DPERFETTO_SDK_DIR=<dir>`). Tracing is gated at runtime by the `WIVRN_TRACING` env var, so a tracing-enabled build has no cost until you set it.

To also instrument Monado under the same switch, use the `server-tracing-monado` preset (adds `WIVRN_TRACE_MONADO=ON`); this requires [percetto](https://github.com/olvaffe/percetto/) to be discoverable at configure time.

See [profiling](profiling.md) for how to run the server with tracing and capture/analyse traces.

# Dashboard

The WiVRn dashboard requires Qt6, and the WiVRn server.

## Compile

From your checkout directory, compile both the server and the dashboard:
```bash
cmake -B build-dashboard . -GNinja -DWIVRN_BUILD_CLIENT=OFF -DWIVRN_BUILD_SERVER=ON -DWIVRN_BUILD_DASHBOARD=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-dashboard
```

See [Server](#server-pc) for the server compile options.

# Server (Android phone)

Runs the WiVRn server directly on an Android phone instead of a PC. Shares the same [build dependencies](#build-dependencies) and [Android environment](#android-environment) setup as the [client](#client-headset) below -- both build from this repository's root `CMakeLists.txt`, just with different CMake arguments (`WIVRN_BUILD_SERVER` instead of `WIVRN_BUILD_CLIENT`).

Vulkan-Headers/glslangValidator/spirv-tools resolve from the same system packages listed under [build dependencies](#build-dependencies), same as the client (`CMAKE_FIND_ROOT_PATH_MODE_*=BOTH` lets CMake's `find_package`/`find_program` see host-installed packages despite the NDK cross-compile sysroot). If your system packages are too old or otherwise incompatible, `server-app/build.gradle` also supports overriding them with a pinned local copy at a `tools/` directory next to this checkout -- only used if present:

```
<parent-of-this-checkout>/
├── wivrn/                       (this repository)
└── tools/
    ├── vulkan-headers/include/  (Vulkan-Headers' include/ directory)
    ├── glslang/extracted/usr/bin/glslangValidator
    └── spirv-tools/extracted/usr/bin/  (spirv-opt, etc.)
```

#### Server build
From the main directory.
```bash
export ANDROID_HOME=~/Android
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk/

./gradlew :server-app:assembleDebug
```

Outputs will be in `server-app/build/outputs/apk/debug/server-app-debug.apk`. This is a debug build (auto-signed with the default debug keystore) -- see [Apk signing](#apk-signing) below if you need a release build.

#### Install and run
Same `adb` setup as the [client](#install-apk-with-adb) below.
```bash
adb install server-app/build/outputs/apk/debug/server-app-debug.apk
adb shell monkey -p org.meumeu.wivrn.server -c android.intent.category.LAUNCHER 1
```

# Client (headset)

#### Build dependencies
As Arch package names: git pkgconf glslang cmake jdk17-openjdk librsvg cli11 ktx-software-bin ([AUR](https://aur.archlinux.org/packages/ktx-software-bin))

OpenSSL build dependencies are also needed, as described [here](https://github.com/openssl/openssl/blob/master/INSTALL.md#prerequisites), in particular perl 5.

#### Android environment
Download [sdkmanager](https://developer.android.com/tools/sdkmanager) commandline tool and extract it to any directory.
Create your `ANDROID_HOME` directory, for instance `~/Android`.

Review and accept the licenses with
```bash
sdkmanager --sdk_root="${HOME}/Android" --licenses
```

Install the correct cmake version with
```bash
sdkmanager --install "cmake;3.31.5"
```

#### Apk signing
Your device may refuse to install an unsigned apk, so you must create signing keys before building the client
```
# Create key, then enter the password and other information that the tool asks for
keytool -genkey -v -keystore ks.keystore -alias default_key -keyalg RSA -keysize 2048 -validity 10000

# Substitute your password that you entered in the command above instead of YOUR_PASSWORD
echo signingKeyPassword="YOUR_PASSWORD" > gradle.properties
```
Once you have generated the keys, the apk will be automatically signed at build time

#### Client build
From the main directory.
```bash
export ANDROID_HOME=~/Android
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk/

./gradlew assembleRelease
```

Outputs will be in `build/outputs/apk/release/WiVRn-release.apk`

#### Install apk with adb
Before using adb you must enable usb debugging on your device:
 * Pico - https://developer.picoxr.com/document/unity-openxr/set-up-the-development-environment/ (see first step)
 * Quest - https://developer.oculus.com/documentation/unity/unity-env-device-setup/#headset-setup (see "Set Up Meta Headset" and "Test an App on Headset" until step 4)

Also add your device in udev rules: https://wiki.archlinux.org/title/Android_Debug_Bridge#Adding_udev_rules

Then connect the device via usb to your computer and execute the following commands
```
# Start adb server
adb start-server

# Check if the device is connected
adb devices

# Install apk
adb install build/outputs/apk/release/WiVRn-release.apk

# When you're done, you can stop the adb server
adb kill-server
```
