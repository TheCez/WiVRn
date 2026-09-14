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

#include "utils/frame_timing_stats.h"
#include "video_encoder.h"
#include "vk/allocation.h"

#include <array>
#include <format>
#include <media/NdkMediaCodec.h>
#include <span>
#include <vector>

namespace wivrn
{

// Android's hardware video encoder, via the NDK's byte-buffer AMediaCodec
// API (NdkMediaCodec.h) -- see docs/ANDROID_PORT.md's Milestone 3 entry for
// the full story of why this exists at all.
//
// ponytail: this copies the compositor's rendered image out to a
// host-visible VMA buffer first (present_image(), identical to
// video_encoder_raw.cpp's own copy step) and memcpy's that into MediaCodec's
// own input buffer in encode() -- not genuinely zero-copy.
//
// A real, exhaustive zero-copy investigation was done and every public
// GPU->MediaCodec route on this device/driver was tried and failed for a
// specific, diagnosed reason (MediaCodec's own Surface-input path, direct
// Vulkan STORAGE writes, Vulkan-exported AHardwareBuffer,
// VK_ANDROID_external_format_resolve, and GL_EXT_YUV_target) -- see
// docs/pixel10-pro-xl-gpu-media-investigation.md for the full record,
// section H for the summary table. MediaCodec's Block Model +
// QueueRequest.setHardwareBuffer() DOES work on this device (section G) --
// the remaining blocker is populating that HardwareBuffer from the GPU,
// not MediaCodec itself. If revisited (different device/firmware, or a
// driver update), re-run that document's probes first rather than
// re-deriving this from scratch.
class video_encoder_mediacodec : public video_encoder
{
	vk_bundle & vk;
	vk::raii::CommandPool cmd_pool;

	struct AMediaCodec_deleter
	{
		void operator()(AMediaCodec * c) const
		{
			if (c)
			{
				AMediaCodec_stop(c);
				AMediaCodec_delete(c);
			}
		}
	};
	std::unique_ptr<AMediaCodec, AMediaCodec_deleter> codec;

	struct in_t
	{
		vk::raii::Fence fence = nullptr;
		vk::raii::CommandBuffer cmd = nullptr;
		buffer_allocation buffer; // single NV12 buffer, Y then interleaved UV
	};
	std::array<in_t, num_slots> in;

	// Part B (readback pipeline investigation, docs/ANDROID_PORT.md's perf
	// branch): opt-in, near-zero-cost-when-disabled timing breakdown of the
	// GPU-copy -> CPU-memcpy -> MediaCodec handoff, one line per stage
	// every ~240 frames when WIVRN_TIMING_LOG is set. See
	// frame_timing_stats.h; these are function/instance members rather
	// than function-local statics only because the stage name needs this
	// stream's index baked in.
	frame_timing_stats t_present_fence_wait{std::format("mediacodec[{}] present_image fence wait (stale slot)", stream_idx)};
	frame_timing_stats t_encode_fence_wait{std::format("mediacodec[{}] encode fence wait (this frame's copy)", stream_idx)};
	frame_timing_stats t_codec_input_wait{std::format("mediacodec[{}] dequeueInputBuffer wait", stream_idx)};
	frame_timing_stats t_memcpy{std::format("mediacodec[{}] memcpy", stream_idx)};
	frame_timing_stats t_codec_output_wait{std::format("mediacodec[{}] dequeueOutputBuffer wait", stream_idx)};
	frame_timing_stats t_encode_total{std::format("mediacodec[{}] encode() total", stream_idx)};

	// SPS+PPS, captured once from the encoder's first (CODEC_CONFIG-flagged)
	// output buffer. Devices don't reliably repeat these inline before every
	// keyframe on their own (unlike x264 with b_repeat_headers=1), and the
	// client's decoder expects them there (in-band, no csd-0/csd-1 passed to
	// AMediaCodec_configure on the client side -- see android_decoder.cpp) --
	// so this gets prepended by hand before every IDR we send.
	std::vector<uint8_t> csd;

	// Real per-stream bitrate/fps, cached so the lazily-created codec (see
	// ensure_codec below) can be configured with the same values the
	// constructor would have used, without needing to hold onto the whole
	// encoder_settings.
	uint32_t bitrate;
	float fps;

	// AMediaCodec_create/configure/start is deferred to the first actual
	// present_image() call rather than done unconditionally in the
	// constructor: on this hardware, streams 0/1 (real eyes) are always
	// used, but stream 2 (alpha/passthrough matte) almost never is -- most
	// apps never submit an alpha-blend layer -- yet the compositor always
	// constructs all 3 encoders up front (see compositor.cpp). Configuring
	// and starting a real hardware encoder session for a stream that may
	// then sit fed-nothing for the entire lifetime of the session wastes
	// one of a small, fixed number of concurrent hardware video-encode
	// sessions this SoC actually has -- starving the two streams that are
	// really needed. Deferring means an unused alpha stream never touches
	// the hardware encoder at all. See docs/ANDROID_PORT.md's Milestone 4.5
	// entry for the investigation this came out of.
	void ensure_codec();

	// Copies `payload` into its own heap buffer and hands it to the shared
	// background sender thread instead of calling the inherited SendData()
	// directly -- see encode()'s own comment for why. Used for the cached
	// SPS/PPS (csd), which the real per-frame payload's automatic push
	// (encode()'s return value) can't cover since that only pushes one
	// payload per call.
	void push_async(std::span<const uint8_t> payload, bool control);

public:
	video_encoder_mediacodec(wivrn::vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx);

	void present_image(vk::Image y_cbcr, vk::SemaphoreSubmitInfo info, uint8_t slot, uint64_t frame_index) override;

	std::optional<data> encode(uint8_t slot, uint64_t frame_id) override;
};

} // namespace wivrn
