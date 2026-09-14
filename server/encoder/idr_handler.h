/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "wivrn_packets.h"

#include <cstdint>
#include <mutex>
#include <variant>

namespace wivrn
{

class idr_handler
{
public:
	virtual ~idr_handler();
	virtual void on_feedback(const from_headset::feedback &) = 0;
	virtual void reset() = 0;
	virtual bool should_skip(uint64_t frame_id) = 0;
};

// handler for unknown P-frames
// any lost frame triggers an I-frame
// skip frames until th I frame is received
class default_idr_handler : public idr_handler
{
	std::mutex mutex;
	struct need_idr
	{};
	struct wait_idr_feedback
	{
		uint64_t idr_id;
	};
	struct idr_received
	{};
	struct running
	{
		uint64_t first_p;
	};
	std::variant<need_idr, wait_idr_feedback, idr_received, running> state;
	// NOTE: parens, not braces -- brace-init here would pick the
	// initializer_list<uint64_t> constructor (both args convert to
	// uint64_t), making this a 2-element vector instead of a 512-element
	// one filled with -1.
	std::vector<uint64_t> non_ref_frames = std::vector<uint64_t>(512, uint64_t(-1));
	// Frame indices this encoder actually encoded and sent, most recent
	// 512. The compositor can drop a frame under load before the encoder
	// ever sees it (server/compositor/compositor.cpp's layer_commit(),
	// when encode_request is still >= 0 from the previous frame) -- such a
	// frame's index never reaches get_type()/present_image() for this
	// stream at all. The client still reports feedback for it (frame_index
	// was never delivered), which on_feedback()'s "running" branch would
	// otherwise mistake for a genuinely lost/corrupted frame and demand a
	// new IDR -- entering a self-sustaining livelock (see this file's own
	// should_skip(): every frame is skipped until that IDR is acknowledged,
	// repeatedly, since the *next* frame can just as easily also be one the
	// compositor dropped). Each of the 2-3 streams runs this state machine
	// independently, so this desyncs them from each other, not just from
	// real time -- see docs/ANDROID_PORT.md's Milestone 4.5 entry for the
	// full trail. Only a frame index present here can legitimately trigger
	// an IDR request; genuine loss (this encoder DID send it) still does.
	std::vector<uint64_t> sent_frames = std::vector<uint64_t>(512, uint64_t(-1));

public:
	enum class frame_type
	{
		i,
		p,
	};

	void on_feedback(const from_headset::feedback &) override;
	void reset() override;
	bool should_skip(uint64_t frame_id) override;
	void set_non_ref(uint64_t frame_index);
	bool is_non_ref_frame(uint64_t frame_index);
	bool was_sent(uint64_t frame_index);
	frame_type get_type(uint64_t frame_index);
};
} // namespace wivrn
