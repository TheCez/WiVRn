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

#include "frame_timing_stats.h"

#include "util/u_logging.h"

#include <algorithm>
#include <cstdlib>

bool wivrn::frame_timing_stats::enabled()
{
	static const bool e = [] {
		const char * v = getenv("WIVRN_TIMING_LOG");
		return v && *v;
	}();
	return e;
}

wivrn::frame_timing_stats::frame_timing_stats(std::string stage_name, size_t window) :
        name(std::move(stage_name)), window(window)
{
	samples.reserve(window);
}

void wivrn::frame_timing_stats::sample(int64_t duration_ns)
{
	if (not enabled())
		return;

	std::lock_guard lock(mutex);
	samples.push_back(duration_ns);
	if (samples.size() >= window)
		flush_locked();
}

void wivrn::frame_timing_stats::flush_locked()
{
	std::ranges::sort(samples);
	const size_t n = samples.size();
	auto us = [](int64_t ns) { return ns / 1000.0; };

	int64_t sum = 0;
	for (auto s: samples)
		sum += s;

	const double avg = us(sum / int64_t(n));
	const double p50 = us(samples[n * 50 / 100]);
	const double p95 = us(samples[std::min(n - 1, n * 95 / 100)]);
	const double worst = us(samples.back());

	U_LOG_I("[timing] %-42s n=%-4zu avg=%9.1fus p50=%9.1fus p95=%9.1fus max=%9.1fus",
	        name.c_str(), n, avg, p50, p95, worst);

	samples.clear();
}
