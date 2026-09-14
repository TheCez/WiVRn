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

// Android replacement for wivrn_uinput.cpp: unprivileged Android apps cannot
// open /dev/uinput, so virtual keyboard/mouse/gamepad forwarding is not
// available on Android. This keeps the wivrn_uinput.h interface (and thus
// wivrn_session's std::optional<wivrn_uinput> member) working unchanged, as
// a no-op.

#include "wivrn_uinput.h"

void wivrn_uinput::handle_input(wivrn::from_headset::hid::input &)
{}

void wivrn_uinput::handle_gamepad(const wivrn::from_headset::inputs &)
{}

void wivrn_uinput::destroy_gamepad()
{}

std::vector<wivrn::to_headset::haptics> wivrn_uinput::read_rumble()
{
	return {};
}
