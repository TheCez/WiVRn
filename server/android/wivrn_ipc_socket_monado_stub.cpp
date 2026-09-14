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

// wivrn_ipc_socket_monado (declared extern in wivrn_ipc.h, defined in
// main.cpp on desktop) is how wivrn-server talks to a separate,
// system-installed Monado OpenXR service process -- send_to_main(),
// receive_from_main(), and several direct .send()/.get_fd() call sites in
// driver/wivrn_session.cpp and driver/wivrn_connection.* all use it
// unconditionally. On Android, Monado is linked directly into this same
// process (both the plain-executable and the JNI .so builds), so there is
// no separate process to talk to, but leaving the optional permanently
// empty would make every one of those call sites undefined behavior the
// first time any of them ran (operator-> on a disengaged optional).
// Instead, give it a real connected loopback datagram socket pair: sends
// succeed harmlessly (buffered, never read), and get_fd() returns a valid
// pollable fd that just never signals readable. The peer end is
// deliberately kept open (not closed) so writes never fail with
// ECONNREFUSED.
//
// Shared by both server/android/wivrn_server_android_stub_main.cpp (plain
// executable, for adb-based testing) and server/android/wivrn_server_jni.cpp
// (JNI .so, for the Android Service wrapper) -- see server/CMakeLists.txt.

#include "wivrn_ipc.h"

#include <sys/socket.h>

std::optional<wivrn::typed_socket<wivrn::UnixDatagram, to_monado::packets, from_monado::packets>> wivrn_ipc_socket_monado;

namespace
{
struct wivrn_ipc_socket_monado_stub_init
{
	wivrn::fd_base peer;

	wivrn_ipc_socket_monado_stub_init()
	{
		int fds[2];
		if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) == 0)
		{
			wivrn_ipc_socket_monado.emplace(fds[0]);
			peer = wivrn::fd_base(fds[1]);
		}
	}
} wivrn_ipc_socket_monado_stub_init_instance;
} // namespace
