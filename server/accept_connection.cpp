/*
 * WiVRn VR streaming
 * Copyright (C) 2022-2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022-2024  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "accept_connection.h"

#include "util/u_logging.h"
#include "utils/overloaded.h"

#include "driver/configuration.h"
#include "driver/wivrn_session.h"
#include "wivrn_ipc.h"
#include "wivrn_sockets.h"

#include <sys/poll.h>

std::unique_ptr<wivrn::TCP> wivrn::accept_connection(wivrn_session & cnx, std::stop_token stop, std::function<void(wivrn_session &)> tick)
{
#ifndef __ANDROID__
	// On desktop, wivrn-server coordinates with a separate, system-installed
	// Monado OpenXR service process over wivrn_ipc_socket_monado (headset
	// connect/disconnect notifications, and a way for that process to ask
	// this loop to stop). On Android, Monado is linked directly into this
	// same process -- there is no separate service to coordinate with.
	wivrn_ipc_socket_monado->send(from_monado::headset_disconnected{});
#endif

	wivrn::TCPListener listener(configuration().port);

#ifndef __ANDROID__
	pollfd fds[2]{
	        {.fd = listener.get_fd(), .events = POLLIN},
	        {.fd = wivrn_ipc_socket_monado->get_fd(), .events = POLLIN},
	};
#else
	pollfd fds[1]{
	        {.fd = listener.get_fd(), .events = POLLIN},
	};
#endif

	while (not stop.stop_requested())
	{
		if (poll(fds, std::size(fds), 100) < 0)
		{
			perror("poll");
			return {};
		}

		if (fds[0].revents & POLLIN)
		{
#ifndef __ANDROID__
			wivrn_ipc_socket_monado->send(from_monado::headset_connected{});
#endif
			return std::make_unique<wivrn::TCP>(listener.accept().first);
		}

#ifndef __ANDROID__
		if (fds[1].revents & POLLIN)
		{
			auto packet = receive_from_main();
			if (packet)
				std::visit(utils::overloaded{
				                   [&cnx](to_monado::stop) {
					                   // gets handled in wivrn_session::reconnect since we return nullptr
					                   U_LOG_I("Received stop packet during reconnect, stopping");
					                   cnx.request_stop();
				                   },
				                   [](auto &&) {
					                   // Ignore request when no headset is connected
				                   },
				           },
				           *packet);
		}
#endif

		if (tick)
			tick(cnx);
	}

	return {};
}
