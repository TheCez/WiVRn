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

#pragma once

// std::ranges::enumerate_view (C++23, P2164) is not yet implemented by the
// libc++ shipped in current Android NDKs (checked: no __ranges/enumerate_view.h,
// while __ranges/zip_view.h is present), even with -fexperimental-library.
// This polyfills just the "for (auto&& [i, x] : enumerate_view(r))" usage
// this codebase relies on, using the zip+iota building blocks that are
// available, rather than waiting on the toolchain.
#ifndef __cpp_lib_ranges_enumerate
#include <ranges>

namespace std::ranges
{

template <std::ranges::viewable_range R>
auto enumerate_view(R && r)
{
	return std::views::zip(std::views::iota(std::ranges::range_difference_t<R>(0)), std::forward<R>(r));
}

} // namespace std::ranges
#endif
