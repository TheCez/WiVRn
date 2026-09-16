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
#include <fstream>
#include <media/NdkMediaCodec.h>
#include <span>
#include <vector>

namespace wivrn
{

// Android's hardware video encoder, via the NDK's byte-buffer AMediaCodec API.
//
// ponytail: copies the compositor's rendered image to a host-visible VMA
// buffer (present_image()) then memcpy's into MediaCodec's input buffer
// (encode()) -- not zero-copy. Every public GPU->MediaCodec zero-copy route
// (Surface-input, Vulkan STORAGE writes, exported AHardwareBuffer,
// VK_ANDROID_external_format_resolve, GL_EXT_YUV_target) was tried and failed
// on this device/driver for a diagnosed reason; MediaCodec's Block Model +
// setHardwareBuffer() does work, the remaining blocker is populating that
// HardwareBuffer from the GPU. Revisit only with a different device/driver.
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

	// Opt-in timing breakdown of the GPU-copy -> CPU-memcpy -> MediaCodec
	// handoff, logged every ~240 frames when WIVRN_TIMING_LOG is set --
	// instance members (not statics) since the stage name needs stream_idx.
	frame_timing_stats t_present_fence_wait{std::format("mediacodec[{}] present_image fence wait (stale slot)", stream_idx)};
	frame_timing_stats t_encode_fence_wait{std::format("mediacodec[{}] encode fence wait (this frame's copy)", stream_idx)};
	frame_timing_stats t_codec_input_wait{std::format("mediacodec[{}] dequeueInputBuffer wait", stream_idx)};
	frame_timing_stats t_memcpy{std::format("mediacodec[{}] memcpy", stream_idx)};
	frame_timing_stats t_codec_output_wait{std::format("mediacodec[{}] dequeueOutputBuffer wait", stream_idx)};
	frame_timing_stats t_encode_total{std::format("mediacodec[{}] encode() total", stream_idx)};

	// Diagnostic (WIVRN_DUMP_NV12 env var, like WIVRN_DUMP_VIDEO): raw capture
	// of the exact NV12 bytes queued to AMediaCodec, each frame prefixed with
	// its frame_index to correlate against WIVRN_DUMP_VIDEO's encoded output --
	// answers whether a corrupted encoded frame's source was already corrupt.
	std::ofstream nv12_dump;

	// SPS+PPS, captured once from the encoder's first (CODEC_CONFIG-flagged)
	// output buffer. Devices don't reliably repeat these inline before every
	// keyframe on their own (unlike x264 with b_repeat_headers=1), and the
	// client's decoder expects them there (in-band, no csd-0/csd-1 passed to
	// AMediaCodec_configure on the client side -- see android_decoder.cpp) --
	// so this gets prepended by hand before every IDR we send.
	std::vector<uint8_t> csd;

	// Real per-stream bitrate/fps/codec, cached so the lazily-created codec
	// (see ensure_codec below) can be configured with the same values the
	// constructor would have used, without needing to hold onto the whole
	// encoder_settings.
	uint32_t bitrate;
	float fps;
	video_codec codec_kind;

	// Deferred to the first present_image() rather than the constructor:
	// the compositor always constructs all 3 encoders up front, but the
	// alpha stream is rarely used, and this SoC has a small, fixed number
	// of concurrent hardware encode sessions -- an unused alpha stream must
	// never claim one.
	void ensure_codec();

	// Copies `payload` into its own heap buffer and hands it to the shared
	// background sender thread instead of calling the inherited SendData()
	// directly -- see encode()'s own comment for why. Used for the cached
	// SPS/PPS (csd), which the real per-frame payload's automatic push
	// (encode()'s return value) can't cover since that only pushes one
	// payload per call.
	void push_async(std::span<const uint8_t> payload, bool control);

	// Mirrors compositor.cpp's own image_layer() -- see compositor.h's struct
	// image. Computed independently since this class only has vk_bundle +
	// stream_idx to go on. Out-of-line: vk_bundle is only forward-declared here.
	uint32_t image_layer() const;

public:
	video_encoder_mediacodec(wivrn::vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx);

	// For encoder_settings.cpp's check_mediacodec() probe: the constructor
	// alone doesn't exercise ensure_codec() (lazy, see above), so without
	// this a codec unsupported on this device would crash the server on
	// the first real present_image() instead of failing the probe.
	void probe_ensure_codec() { ensure_codec(); }

	void present_image(vk::Image y_cbcr, vk::SemaphoreSubmitInfo info, uint8_t slot, uint64_t frame_index) override;

	std::optional<data> encode(uint8_t slot, uint64_t frame_id) override;
};

} // namespace wivrn
