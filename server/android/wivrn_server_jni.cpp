/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
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

// Real JNI entry point for the Android Service wrapper (built only when
// WIVRN_ANDROID_JNI=ON, producing a .so instead of a plain executable --
// see server/CMakeLists.txt).
//
// On desktop, main.cpp's start_server() forks a child process that calls
// Monado's own ipc_server_main_common() -- the actual compositor/IPC-server
// entry point, already fully wired up by this target's XRT_* CMake config
// (see server/CMakeLists.txt) via WiVRn's own driver
// (target_instance_wivrn.cpp) and callbacks (ipc_server_cb.cpp). Nothing
// about that function itself is desktop-specific. This file just calls it
// directly, on a background thread, instead of forking -- there is no
// separate process to fork into on Android, and no reason to want one.
//
// Known simplification vs. desktop, worth revisiting before this is more
// than a first cut: main.cpp's inner_main() is actually a *second*, outer
// session-manager loop (pairing/PIN, Avahi publish, an outer
// create_listen_socket()) that only calls start_server() once a headset
// has made initial contact, i.e. the compositor is started on demand per
// connection. This JNI entry skips that tier entirely and just runs the
// compositor continuously from Service start -- accept_connection.cpp
// (already Android-safe, see its own comment) does the actual per-connection
// TCP accept inside the compositor itself, so a headset can still connect;
// what's missing is the outer pairing/PIN security flow and Avahi-based
// discovery (NsdManager replacement) that desktop's inner_main() provides.

#include "jni.h"

#include "utils/method.h"

#include "server/ipc_server_interface.h"

#include <atomic>
#include <thread>

namespace
{

// Mirrors server/ipc_server_cb.cpp's pattern (same method_pointer2 trampoline
// technique), but additionally stashes the ipc_server* so nativeStop() can
// call ipc_server_stop() on it -- ipc_server_cb itself has no accessor for
// this.
class android_ipc_server_cb : public ipc_server_callbacks
{
	void init_failed(xrt_result_t)
	{}

	void mainloop_entering(ipc_server * server, xrt_instance *)
	{
		running_server.store(server, std::memory_order_release);
	}

	void mainloop_leaving(ipc_server *, xrt_instance *)
	{
		running_server.store(nullptr, std::memory_order_release);
	}

	void client_connected(ipc_server *, uint32_t)
	{}

	void client_disconnected(ipc_server *, uint32_t)
	{}

public:
	using base_t = void;

	static std::atomic<ipc_server *> running_server;

	android_ipc_server_cb() :
	        ipc_server_callbacks{
	                .init_failed = wivrn::method_pointer2<&android_ipc_server_cb::init_failed>,
	                .mainloop_entering = wivrn::method_pointer2<&android_ipc_server_cb::mainloop_entering>,
	                .mainloop_leaving = wivrn::method_pointer2<&android_ipc_server_cb::mainloop_leaving>,
	                .client_connected = wivrn::method_pointer2<&android_ipc_server_cb::client_connected>,
	                .client_disconnected = wivrn::method_pointer2<&android_ipc_server_cb::client_disconnected>,
	        }
	{}
};

std::atomic<ipc_server *> android_ipc_server_cb::running_server = nullptr;

std::optional<std::jthread> server_thread;

void run_server()
{
	android_ipc_server_cb server_cb;

	ipc_server_main_info server_info{
	        .udgci = {
	                .window_title = "WiVRn",
	                .open = U_DEBUG_GUI_OPEN_NEVER,
	        },
	        .exit_on_disconnect = false,
	        .no_stdin = true,
	};

	// Blocks until ipc_server_stop() is called (from nativeStop()) or the
	// server otherwise decides to exit.
	ipc_server_main_common(&server_info, &server_cb, nullptr);
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_server_WivrnServerService_nativeStart(JNIEnv *, jobject)
{
	if (server_thread)
		return; // already running

	server_thread.emplace([](std::stop_token) {
		run_server();
	});
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_server_WivrnServerService_nativeStop(JNIEnv *, jobject)
{
	if (!server_thread)
		return;

	if (ipc_server * server = android_ipc_server_cb::running_server.load(std::memory_order_acquire))
		ipc_server_stop(server);

	server_thread->request_stop();
	server_thread->join();
	server_thread.reset();
}
