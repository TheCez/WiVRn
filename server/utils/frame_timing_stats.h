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

#include "os/os_time.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wivrn
{

// Lightweight rolling latency-stage logger for the readback/encode pipeline --
// A/B a pipeline change over a short live test via plain logcat, without
// Perfetto tooling (wivrn::trace is complementary, not replaced by this).
// Off by default (a single cached branch when disabled); enable by setting
// WIVRN_TIMING_LOG to any non-empty value.
class frame_timing_stats
{
public:
	static bool enabled();

	// One instance per named pipeline stage (e.g.
	// "mediacodec[0] copy_fence_wait"), typically a function-local static
	// or a per-encoder-instance member. `window` samples are accumulated
	// before one avg/p50/p95/max summary line is logged and the window
	// resets -- default 240 samples is ~a few seconds at 60-90fps.
	explicit frame_timing_stats(std::string stage_name, size_t window = 240);

	// Record one duration, in nanoseconds (os_monotonic_get_ns() deltas).
	// Thread-safe; a no-op unless enabled().
	void sample(int64_t duration_ns);

private:
	void flush_locked();

	std::string name;
	size_t window;
	std::mutex mutex;
	std::vector<int64_t> samples;
};

// RAII wrapper: samples os_monotonic_get_ns() elapsed time between
// construction and destruction into the given frame_timing_stats. Covers
// every exit path (early return, exception, normal fall-through) of the
// scope it's declared in, matching this codebase's existing idle_setter
// pattern (video_encoder.cpp).
class scoped_timing_sample
{
public:
	explicit scoped_timing_sample(frame_timing_stats & stats) :
	        stats(stats), begin_ns(frame_timing_stats::enabled() ? os_monotonic_get_ns() : 0) {}
	~scoped_timing_sample()
	{
		if (begin_ns)
			stats.sample(os_monotonic_get_ns() - begin_ns);
	}
	scoped_timing_sample(const scoped_timing_sample &) = delete;
	scoped_timing_sample & operator=(const scoped_timing_sample &) = delete;

private:
	frame_timing_stats & stats;
	int64_t begin_ns;
};

} // namespace wivrn
