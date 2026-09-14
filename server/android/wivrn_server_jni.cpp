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

#include "android/android_globals.h"
#include "utils/method.h"

#include "driver/configuration.h"
#include "driver/wivrn_connection.h"
#include "server/ipc_server.h"
#include "server/ipc_server_interface.h"
#include "server/ipc_server_mainloop_android.h"
#include "target_instance_wivrn.h"
#include "wivrn_ipc.h"
#include "wivrn_sockets.h"

#include <atomic>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace
{

// Mirrors server/ipc_server_cb.cpp's pattern (same method_pointer2 trampoline
// technique) -- and MUST replicate its mainloop_entering/leaving behavior
// exactly: instance::create_system() (target_instance_wivrn.cpp) asserts
// that wivrn::instance::server is set, which only happens via
// instance::set_ipc_server(). A first version of this file stashed the
// ipc_server* into its own atomic but never called set_ipc_server(), which
// crashed on launch (SIGABRT, "assertion server failed") the moment a
// client tried to connect.
//
// Beyond that, this class also bridges client_connected/client_disconnected
// up to WivrnServerService's onClientConnected/onClientDisconnected (see
// nativeStart below for how the JavaVM*/jobject are cached), so the UI can
// show live connection status instead of nothing.
class android_ipc_server_cb : public ipc_server_callbacks
{
	void init_failed(xrt_result_t)
	{}

	void mainloop_entering(ipc_server * server, xrt_instance * xrt_inst)
	{
		running_server.store(server, std::memory_order_release);
		static_cast<wivrn::instance *>(xrt_inst)->set_ipc_server(server);
	}

	void mainloop_leaving(ipc_server *, xrt_instance * xrt_inst)
	{
		running_server.store(nullptr, std::memory_order_release);
		static_cast<wivrn::instance *>(xrt_inst)->set_ipc_server(nullptr);
	}

	void client_connected(ipc_server *, uint32_t client_id);
	void client_disconnected(ipc_server *, uint32_t client_id);

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

// Cached in nativeStart, used by call_service_method() below.
JavaVM * g_vm = nullptr;
jobject g_service = nullptr; // GlobalRef

// client_connected/client_disconnected (and run_server()'s headset-level
// calls below) run on the server thread, not whatever thread called
// nativeStart, so this attaches/detaches around the actual JNI call rather
// than reusing an env captured elsewhere.
//
// Two different, deliberately separate signals get bridged to Java through
// here -- conflating them was an actual bug (see run_server()'s comment):
// ipc_server_callbacks::client_connected/disconnected (Monado's own hooks,
// used by call_service_method_int) fire for a *local OpenXR application* on
// the phone connecting to this runtime over IPC -- nothing does that yet
// (no OpenXR runtime broker registration), so these never fire in practice
// today. onHeadsetConnected/onHeadsetDisconnected (call_service_method_str /
// the plain call below) are about the actual network connection to a remote
// headset, fired directly from run_server() itself since that's the code
// that actually owns accepting it.
void call_service_method_int(const char * name, uint32_t client_id)
{
	if (!g_vm || !g_service)
		return;

	JNIEnv * env = nullptr;
	bool attached = false;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
	{
		if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
			return;
		attached = true;
	}

	jclass cls = env->GetObjectClass(g_service);
	jmethodID mid = env->GetMethodID(cls, name, "(I)V");
	if (mid)
		env->CallVoidMethod(g_service, mid, jint(client_id));
	env->DeleteLocalRef(cls);

	if (attached)
		g_vm->DetachCurrentThread();
}

void call_service_method_str(const char * name, const std::string & arg)
{
	if (!g_vm || !g_service)
		return;

	JNIEnv * env = nullptr;
	bool attached = false;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
	{
		if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
			return;
		attached = true;
	}

	jclass cls = env->GetObjectClass(g_service);
	jmethodID mid = env->GetMethodID(cls, name, "(Ljava/lang/String;)V");
	if (mid)
	{
		jstring jarg = env->NewStringUTF(arg.c_str());
		env->CallVoidMethod(g_service, mid, jarg);
		env->DeleteLocalRef(jarg);
	}
	env->DeleteLocalRef(cls);

	if (attached)
		g_vm->DetachCurrentThread();
}

void call_service_method_void(const char * name)
{
	if (!g_vm || !g_service)
		return;

	JNIEnv * env = nullptr;
	bool attached = false;
	if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
	{
		if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
			return;
		attached = true;
	}

	jclass cls = env->GetObjectClass(g_service);
	jmethodID mid = env->GetMethodID(cls, name, "()V");
	if (mid)
		env->CallVoidMethod(g_service, mid);
	env->DeleteLocalRef(cls);

	if (attached)
		g_vm->DetachCurrentThread();
}

void android_ipc_server_cb::client_connected(ipc_server *, uint32_t client_id)
{
	call_service_method_int("onClientConnected", client_id);
}

void android_ipc_server_cb::client_disconnected(ipc_server *, uint32_t client_id)
{
	call_service_method_int("onClientDisconnected", client_id);
}

// wivrn::instance::create_system() (target_instance_wivrn.cpp) does
// std::move(connection) on the extern global declared in wivrn_ipc.h --
// populated on desktop by main.cpp's headset_connected(), *before*
// start_server() is ever called, which is why ipc_server_main_common()
// can safely call create_system() immediately at startup: it assumes a
// connection already exists. Skipping straight to ipc_server_main_common()
// without populating it first crashed here (SIGSEGV constructing a
// headset_info_packet from a moved-from-null connection) the moment
// create_system() ran, regardless of whether a headset had connected --
// this is not optional plumbing, it's required for this call to be safe
// at all. So: accept a TCP connection and construct wivrn_connection first,
// exactly like headset_connected() does, then start the compositor.
//
// Known gap: unlike desktop, there is no PIN/pairing UI here yet, so
// encryption_state::enabled (desktop's default) rejects every client with
// "Pairing is disabled on server" -- there's no known key and nothing here
// ever puts the server into `pairing` state to learn one. Using
// encryption_state::disabled unconditionally instead, same as desktop's
// explicit `--no-encrypt` flag (main.cpp). Revisit alongside NsdManager-based
// discovery and a real pairing UI (see docs/ANDROID_PORT.md).
//
// Known gap: listener.accept() is a blocking call not interruptible by
// std::stop_token; if nativeStop() is called while still waiting for a
// first connection, server_thread->join() will block until a connection
// (or a spurious wake) arrives. Fine for now, worth fixing before this
// goes further.
void run_server(std::stop_token stop)
{
	while (!stop.stop_requested())
	{
		try
		{
			// listener is scoped to this try block, not the outer loop:
			// it must be closed (RAII, on scope exit) before
			// ipc_server_main_common() below runs. If the connection
			// later drops mid-session, Monado's own reconnect logic
			// (wivrn_session::reconnect -> accept_connection(), reused
			// as-is from desktop) opens its *own* TCPListener on this
			// same configured port from this same process -- there's no
			// separate child process to isolate it like on desktop. A
			// first version kept one TCPListener alive for the whole
			// function, which held the port for the entire session and
			// made every reconnect attempt fail with "Address already
			// in use" in a tight, uninterruptible retry loop (visible in
			// logcat as endless "Exception while connecting headset:
			// Address already in use").
			wivrn::TCPListener listener(wivrn::configuration().port);
			wivrn::TCP tcp = listener.accept().first;
			connection = std::make_unique<wivrn::wivrn_connection>(
			        stop, wivrn::wivrn_connection::encryption_state::disabled, "", std::move(tcp));
		}
		catch (std::exception & e)
		{
			std::cerr << "WiVRn: client connection failed: " << e.what() << std::endl;
			continue;
		}

		// The connection constructor above blocks until the initial
		// handshake (including from_headset::headset_info_packet) is
		// complete, so info() is already populated here -- system_name is
		// the same client-guessed model string visible in the headset's own
		// logcat ("Guessing HMD model from... ro.product.model"). This is
		// the real, network-level "a headset is connected" signal -- unlike
		// ipc_server_callbacks::client_connected below, which is about a
		// *local OpenXR app* on the phone using this runtime over IPC, a
		// completely different thing nothing exercises yet (see
		// android_ipc_server_cb's comment). Known gap: only fires for the
		// first accept() of a session, not for Monado's own mid-session
		// reconnect() (wivrn_session.cpp, unmodified from desktop, no clean
		// hook without touching shared code) -- fine since reconnects are
		// meant to be a brief, transparent blip, not a new device.
		call_service_method_str("onHeadsetConnected", connection->info().system_name);

		android_ipc_server_cb server_cb;

		ipc_server_main_info server_info{
		        .udgci = {
		                .window_title = "WiVRn",
		                .open = U_DEBUG_GUI_OPEN_NEVER,
		        },
		        .exit_on_disconnect = false,
		        .no_stdin = true,
		};

		// Blocks until ipc_server_stop() is called (from nativeStop()) or
		// the server otherwise decides to exit (e.g. the headset
		// disconnects). Returns to accept() afterward for the next
		// connection, unless a stop was requested meanwhile.
		ipc_server_main_common(&server_info, &server_cb, nullptr);

		call_service_method_void("onHeadsetDisconnected");
	}
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_server_WivrnServerService_nativeStart(JNIEnv * env, jobject thiz)
{
	if (server_thread)
		return; // already running

	env->GetJavaVM(&g_vm);
	g_service = env->NewGlobalRef(thiz);

	// A Service is itself an android.content.Context (just not an
	// android.app.Activity), and this is all Monado's own aux_android
	// glue actually needs registered here -- without it,
	// android_globals_get_context() returns null, and
	// comp_multi_compositor.c's Android-only refresh-rate-change-notify
	// path (multi_compositor_request_display_refresh_rate) SIGSEGVs the
	// moment a local OpenXR app connects and requests a session, taking
	// the whole process down (and with it, the Quest's TCP connection --
	// looks like "the headset disconnected" but the real cause is this).
	// Found by testing against a real OpenXR app, not from reading the
	// header comments alone: no test before this ever had a local
	// OpenXR client connect at all.
	android_globals_store_vm_and_context(g_vm, g_service);


	server_thread.emplace([](std::stop_token stop) {
		run_server(stop);
	});
}

extern "C" JNIEXPORT void JNICALL
Java_org_meumeu_wivrn_server_WivrnServerService_nativeStop(JNIEnv * env, jobject)
{
	if (!server_thread)
		return;

	if (ipc_server * server = android_ipc_server_cb::running_server.load(std::memory_order_acquire))
		ipc_server_stop(server);

	server_thread->request_stop();
	server_thread->join();
	server_thread.reset();

	if (g_service)
	{
		env->DeleteGlobalRef(g_service);
		g_service = nullptr;
	}
	g_vm = nullptr;
}

// Called from MonadoIpcService.connect() (a *different* Java Service, bound
// via Monado's own IMonado AIDL interface by a local OpenXR app's loader --
// see that file's own comment and docs/ANDROID_PORT.md's OpenXR runtime
// broker section) whenever a local OpenXR app connects. Mirrors Monado's own
// service_target.cpp's Java_org_freedesktop_monado_ipc_MonadoImpl_nativeAddClient
// exactly, dup() included -- MonadoIpcService.connect() closes its
// ParcelFileDescriptor right after this call returns (correct, matching
// upstream's own MonadoImpl.connect()), and ipc_server_mainloop_add_fd's
// handoff to the new per-client thread (which calls epoll_ctl on this exact
// fd number) isn't complete by the time this function returns -- without the
// dup() here, that close() can race ahead of it, closing the fd first and
// making that later epoll_ctl fail with EBADF ("Error epoll_ctl(listen_socket)
// failed '-1'" in logcat, immediately followed by the client eventually
// giving up with "Connection reset by peer" once its own request for shared
// memory never gets serviced). Found by testing against a real OpenXR app
// (Unity), not something the header comments alone would have caught.
//
// Known gap: if no headset is connected yet (running_server null -- the
// compositor session, and therefore its ipc_server, only exists once a
// headset has completed its handshake, see run_server() above), this fails
// outright rather than queuing the app to connect once one does. Fine for a
// first cut -- matches the natural expectation that you pair the headset
// first -- revisit if that ordering proves annoying in practice.
extern "C" JNIEXPORT jint JNICALL
Java_org_meumeu_wivrn_server_WivrnServerService_nativeAddIpcClient(JNIEnv *, jclass, jint fd)
{
	ipc_server * server = android_ipc_server_cb::running_server.load(std::memory_order_acquire);
	if (!server)
	{
		std::cerr << "WiVRn: rejecting local OpenXR client, no headset connected yet" << std::endl;
		return -1;
	}
	int native_fd = dup(fd);
	return ipc_server_mainloop_add_fd(server, &server->ml, native_fd);
}
