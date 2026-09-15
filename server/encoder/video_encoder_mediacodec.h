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

	// Milestone 5 diagnostic (docs/ANDROID_PORT.md's perf branch entry):
	// opt-in (WIVRN_DUMP_NV12 env var, same pattern as the pre-existing
	// WIVRN_DUMP_VIDEO), continuous raw capture of the EXACT NV12 bytes
	// handed to AMediaCodec_queueInputBuffer(), each frame prefixed with
	// its 8-byte little-endian frame_index so it can be correlated
	// against the WIVRN_DUMP_VIDEO encoded-output capture (which logs
	// the same frame_index per output when this is enabled) to answer
	// one specific question: for a visibly corrupted encoded frame, was
	// its source NV12 already corrupt, or clean? Kept as a real,
	// committed, always-available diagnostic (not a throwaway edit) --
	// intentionally NOT removed once this investigation concludes,
	// unlike the temporary debug scaffolding removed in past milestones,
	// since black-macroblock corruption of unknown cause is still open.
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

	// Which array layer of the image present_image() is handed lives at:
	// always 0 on the PowerVR single-layer workaround path (compositor.cpp
	// gives each stream its own dedicated image there), or this stream's
	// own index (0=left, 1=right, 2=alpha) when compositor.cpp instead
	// shares one 3-layer image across streams on every other GPU vendor --
	// see compositor.h's struct image comment and
	// vk_bundle::multi_layer_stream_images. Mirrors compositor.cpp's own
	// image_layer() helper, computed independently here since this class
	// only has vk_bundle + its own stream_idx to go on, not the
	// compositor's stream_image objects. Defined out-of-line (.cpp) since
	// vk_bundle is only forward-declared where `vk` is declared
	// (video_encoder.h).
	uint32_t image_layer() const;

public:
	video_encoder_mediacodec(wivrn::vk_bundle & vk, const encoder_settings & settings, uint8_t stream_idx);

	// For encoder_settings.cpp's check_mediacodec() capability probe only:
	// the constructor alone does NOT exercise ensure_codec() (it's
	// deliberately lazy, see that function's own comment), so a probe that
	// only constructs this class and catches exceptions never actually
	// tries to create the real underlying MediaCodec -- confirmed the hard
	// way when a hardcoded HEVC component name (only valid on the one
	// device it was found on) passed the probe cleanly, then crashed the
	// whole server process with an uncaught exception the moment a real
	// session's first present_image() call reached ensure_codec() on a
	// different device. This gives the probe a way to actually attempt
	// codec creation up front, where a failure is just "codec unsupported"
	// instead of a live crash.
	void probe_ensure_codec() { ensure_codec(); }

	void present_image(vk::Image y_cbcr, vk::SemaphoreSubmitInfo info, uint8_t slot, uint64_t frame_index) override;

	std::optional<data> encode(uint8_t slot, uint64_t frame_id) override;
};

} // namespace wivrn
