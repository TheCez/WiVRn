/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Ajay Chodankar <achodankar28@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Placeholder entry point for the Android build of wivrn-server.
//
// main.cpp (the desktop entry point) is excluded from the Android target
// because it is tightly coupled to the D-Bus dashboard IPC (GDBus, glib,
// libnotify) that main.cpp's start_server()/on_name_acquired() etc. depend
// on. This file exists only so the executable target has a valid entry
// point and everything else (compositor, driver, encoder, Monado itself)
// can be proven to compile and link for Android.
//
// This is used when WIVRN_ANDROID_JNI=OFF (the default) -- for the real JNI
// entry point used by the Android Service wrapper, see
// android/wivrn_server_jni.cpp. Either way, wivrn_ipc_socket_monado (used
// throughout driver/wivrn_session.cpp and driver/wivrn_connection.*) still
// needs a definition; that part is shared in
// android/wivrn_ipc_socket_monado_stub.cpp.
//
// TODO(wivrn-android): now that android/wivrn_server_jni.cpp exists, this
// plain-executable variant is mainly useful for adb-based iteration outside
// an APK; consider whether it's still worth keeping once the Service
// wrapper is proven out.
int main()
{
	return 0;
}

#include "audio/audio_setup.h"

// audio_setup.cpp calls this unconditionally on __ANDROID__; the real
// implementation (android/wivrn_server_jni.cpp) only builds with
// WIVRN_ANDROID_JNI=ON, so this plain-executable variant needs its own
// no-op so both configurations still link.
namespace wivrn
{
std::unique_ptr<audio_device> create_android_audio_handle(
        const from_headset::headset_info_packet &,
        wivrn_session &)
{
	return nullptr;
}
} // namespace wivrn
