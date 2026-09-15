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

#include "video_encoder_mediacodec.h"

#include "encoder/encoder_settings.h"
#include "encoder/idr_handler.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <vector>
#include <media/NdkMediaFormat.h>
#include <stdexcept>

// Milestone 5 diagnostic (docs/ANDROID_PORT.md's perf branch entry): see
// present_image()'s own comment at the call site. `adb shell setprop
// debug.xrt.WIVRN_FORCE_GPU_WAIT 1` on Android; WIVRN_FORCE_GPU_WAIT env
// var on desktop.
DEBUG_GET_ONCE_NUM_OPTION(force_gpu_wait, "WIVRN_FORCE_GPU_WAIT", 0)

namespace
{
// MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar (Java-side
// constant; the NDK only exposes AMEDIAFORMAT_KEY_COLOR_FORMAT, the string
// key, not this value) -- i.e. NV12: one full-resolution Y plane, then one
// half-resolution interleaved UV plane. Exactly the layout
// video_encoder_raw.cpp already extracts from the compositor's y_cbcr image
// (same ePlane0/ePlane1 copy below), so no conversion is needed, just get
// those same bytes into MediaCodec's own input buffer.
constexpr int32_t color_format_yuv420_semiplanar = 21;

// MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME / PARAMETER_KEY_VIDEO_BITRATE:
// runtime-only AMediaCodec_setParameters() keys, deliberately different
// strings from the AMEDIAFORMAT_KEY_* used at configure time (e.g. bitrate
// is "bitrate" at configure time, "video-bitrate" at runtime) -- a real,
// documented Android API quirk, not a typo. No NDK header exposes these as
// macros, so they're spelled out here.
constexpr const char * key_request_sync = "request-sync";
constexpr const char * key_video_bitrate = "video-bitrate";

void check(media_status_t status, const char * msg)
{
	if (status != AMEDIA_OK)
		throw std::runtime_error(std::string(msg) + ": MediaCodec error " + std::to_string(int(status)));
}

vk::raii::CommandPool make_cmd_pool(wivrn::vk_bundle & vk, uint8_t stream_idx)
{
	auto res = vk.device.createCommandPool(vk::CommandPoolCreateInfo{
	        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
	        .queueFamilyIndex = vk.transfer_queue ? vk.transfer_queue.family_index : vk.queue.family_index,
	});
	vk.name(res, std::format("mediacodec encoder {} command pool", stream_idx));
	return res;
}
} // namespace

wivrn::video_encoder_mediacodec::video_encoder_mediacodec(
        wivrn::vk_bundle & vk,
        const encoder_settings & settings,
        uint8_t stream_idx) :
        video_encoder(vk,
                      stream_idx,
                      vk.transfer_queue ? vk.transfer_queue.family_index : vk.queue.family_index,
                      settings,
                      std::make_unique<default_idr_handler>(),
                      true),
        vk{vk},
        cmd_pool{make_cmd_pool(vk, stream_idx)}
{
	if (settings.bit_depth != 8)
		throw std::runtime_error("mediacodec encoder only supports 8-bit encoding");
	if (settings.codec != h264)
		throw std::runtime_error("mediacodec encoder only supports h264 for now");

	if (auto dump_nv12 = std::getenv("WIVRN_DUMP_NV12"))
		nv12_dump.open(std::string(dump_nv12) + "-" + std::to_string(stream_idx) + ".nv12raw", std::ios::binary);

	// Buffer is always full NV12 size regardless of stream: MediaCodec was
	// configured (KEY_COLOR_FORMAT/KEY_STRIDE/KEY_SLICE_HEIGHT below) to
	// expect that size on every queueInputBuffer call, for every stream --
	// there's no per-stream color format here, unlike video_encoder_raw.cpp
	// which can just send a shorter buffer since it isn't feeding a real
	// codec. Streams 0/1 are the real left/right color eyes; the Vulkan
	// copy below fills both planes for those every frame. Stream 2 is a
	// single-channel (Y-only) matte/passthrough layer -- matching
	// video_encoder_raw.cpp's own stream_idx < 2 special case for *which
	// planes actually hold real data* -- so its chroma half is filled once
	// below with a neutral 0x80 (instead of whatever plane1 of a
	// single-channel VkImage happened to contain, which produced a
	// corrupted third H.264 stream and showed up on-device as a strong
	// magenta/green color wash over otherwise-correct geometry -- not a
	// stride issue at all, despite the visual symptom looking like one).
	vk::DeviceSize buffer_size = vk::DeviceSize(extent.width) * extent.height;
	buffer_size += buffer_size / 2;

	auto command_buffers = vk.device.allocateCommandBuffers(
	        {.commandPool = *cmd_pool,
	         .commandBufferCount = num_slots});

	for (size_t i = 0; i < num_slots; ++i)
	{
		in[i].cmd = std::move(command_buffers[i]);
		vk.name(in[i].cmd, std::format("mediacodec {} transfer command buffer {}", stream_idx, i));
		in[i].buffer = buffer_allocation(
		        vk.device,
		        {
		                .size = buffer_size,
		                .usage = vk::BufferUsageFlagBits::eTransferDst,
		        },
		        {
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
		        },
		        "mediacodec stream buffer");
		in[i].fence = vk::raii::Fence(vk.device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});

		if (i == 0)
		{
			// Part D (readback pipeline investigation): VMA_MEMORY_USAGE_AUTO
			// picks the actual memory type at allocation time -- log it once
			// rather than assuming. This buffer is only ever written by the
			// GPU (vkCmdCopyImageToBuffer) and read by the CPU (memcpy in
			// encode()), never the other way, so HOST_COHERENT (no manual
			// vkInvalidateMappedMemoryRanges needed before the CPU read) is
			// what we want and currently rely on implicitly -- if this ever
			// logs without eHostCoherent set, encode()'s CPU read is missing
			// a required invalidate and may see stale data.
			auto props = in[i].buffer.properties();
			U_LOG_I("mediacodec[%d] staging buffer memory properties: %s%s%s(raw=%#x)",
			        stream_idx,
			        (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE " : "",
			        (props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT " : "",
			        (props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL " : "",
			        props);
			if (not(props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
				U_LOG_W("mediacodec[%d] staging buffer is NOT HOST_COHERENT -- CPU reads in encode() need an explicit invalidate, currently missing", stream_idx);
		}

		if (stream_idx >= 2)
		{
			// present_image() never touches this stream's chroma half
			// (only plane0/luma is real data for it) -- fill it once
			// with neutral 0x80 rather than leaving it whatever VMA
			// happened to hand back, since it's never written again.
			vk::DeviceSize luma_size = vk::DeviceSize(extent.width) * extent.height;
			memset((uint8_t *) in[i].buffer.map() + luma_size, 0x80, buffer_size - luma_size);
		}
	}

	// codec itself is created lazily -- see ensure_codec() and this class's
	// own header comment.
	bitrate = settings.bitrate;
	fps = settings.fps;
}

void wivrn::video_encoder_mediacodec::ensure_codec()
{
	if (codec)
		return;

	codec.reset(AMediaCodec_createEncoderByType("video/avc"));
	if (not codec)
		throw std::runtime_error("failed to create mediacodec h264 encoder");

	AMediaFormat * format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, extent.width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, extent.height);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, color_format_yuv420_semiplanar);
	// Byte-buffer input on Android is not guaranteed tightly packed by
	// default -- some encoders (this Pixel's included, going by the
	// symptom: green blocky corruption, the textbook sign of the chroma
	// plane being read at the wrong offset) assume a *padded* stride/
	// slice-height unless told otherwise, and silently misinterpret a
	// tightly-packed buffer like the one present_image() actually builds
	// (matching video_encoder_raw.cpp's own layout exactly: width*height
	// Y bytes, then width*height/2 interleaved UV bytes, no row padding
	// anywhere). Setting these explicitly to our real width/height (not a
	// padded value) tells the codec our buffer genuinely has no padding.
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_STRIDE, extent.width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT, extent.height);
	// Without this, Codec2 sizes the input ByteBuffer from an internal
	// default that can be smaller than one actual NV12 frame (observed on
	// this Pixel: 1MiB for one stream, 512KiB for another -- both well
	// under 896*960*1.5 = ~1.23MiB) -- silently, no error anywhere. encode()
	// below already guards against writing past whatever the codec actually
	// gives us, but that guard existing at all means frames COULD be
	// truncated: everything past the codec's real buffer size would stay
	// zeroed, encoding as solid green (Y=0,U=0,V=0 -> RGB(0,135,0)) for the
	// remainder of the frame. This is a real, confirmed-fixed bug (verified:
	// zero "too small" truncation warnings across full sessions once this
	// line was added) -- but it turned out NOT to be the cause of the
	// green-corruption symptom under investigation in docs/ANDROID_PORT.md's
	// Milestone 4.5: that corruption is byte-identical before and after this
	// fix. Kept because it's a genuine latent bug independent of that one.
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, int32_t(extent.width) * extent.height * 3 / 2);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, int32_t(bitrate));
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BITRATE_MODE, 2 /* BITRATE_MODE_CBR */);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, int32_t(std::lround(fps)));
	// Effectively "never": IDR timing is driven entirely by idr_handler via
	// explicit request-sync calls in encode(), same philosophy as desktop's
	// x264 backend (i_keyint_max = X264_KEYINT_MAX_INFINITE).
	AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 100000.0f);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PRIORITY, 0 /* realtime */);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_LATENCY, 1 /* minimize, one-in-one-out where supported */);

	media_status_t configure_status = AMediaCodec_configure(codec.get(), format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
	AMediaFormat_delete(format);
	check(configure_status, "AMediaCodec_configure");

	check(AMediaCodec_start(codec.get()), "AMediaCodec_start");
}

void wivrn::video_encoder_mediacodec::push_async(std::span<const uint8_t> payload, bool control)
{
	auto copy = std::make_shared<std::vector<uint8_t>>(payload.begin(), payload.end());
	video_encoder::push_async(data{
	        .encoder = this,
	        .span = *copy,
	        .mem = copy,
	        .prefer_control = control,
	});
}

void wivrn::video_encoder_mediacodec::present_image(vk::Image y_cbcr, vk::SemaphoreSubmitInfo compositor_sem, uint8_t slot, uint64_t)
{
	ensure_codec();

	// Identical to video_encoder_raw.cpp's present_image(): copy the
	// compositor's already-NV12 image out to our own host-visible buffer.
	// See this class's own comment (video_encoder_mediacodec.h) for why
	// that's the deliberate first-pass tradeoff here.
	//
	// This wait is for a STALE slot: with num_slots==2, `slot` was last
	// used two present_image() calls ago, and its GPU copy should long
	// since have finished (encode() -- below -- already waited on this
	// same fence before this call could even happen, via the base
	// class's own present_slot/encode_slot busy/idle gate). Non-zero
	// wait time here means the render thread is genuinely stalling on
	// this encoder's own pipeline, not just the base class's slot gate.
	auto t_wait_begin = os_monotonic_get_ns();
	auto wait_result = vk.device.waitForFences(*in[slot].fence, true, 1'000'000'000);
	t_present_fence_wait.sample(os_monotonic_get_ns() - t_wait_begin);
	if (wait_result == vk::Result::eTimeout)
	{
		U_LOG_E("Timeout on stream %d", stream_idx);
		return;
	}

	auto & cmd = in[slot].cmd;
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	// y_cbcr is now this stream's own dedicated single-array-layer image
	// (see compositor.h's struct image comment -- was array layer
	// stream_idx of one shared 3-layer image; a real GPU driver bug on
	// this hardware corrupts compute writes to array layer >=1 of a
	// multi-planar image, so each stream now gets its own image and
	// baseArrayLayer is always 0).
	if (need_transfer)
	{
		vk::ImageMemoryBarrier2 barrier{
		        .dstStageMask = vk::PipelineStageFlagBits2KHR::eTransfer,
		        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
		        .srcQueueFamilyIndex = vk.queue.family_index,
		        .dstQueueFamilyIndex = target_queue,
		        .image = y_cbcr,
		        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
		                             .baseMipLevel = 0,
		                             .levelCount = 1,
		                             .baseArrayLayer = 0,
		                             .layerCount = 1},
		};
		cmd.pipelineBarrier2({
		        .imageMemoryBarrierCount = 1,
		        .pImageMemoryBarriers = &barrier,
		});
	}

	std::array regions{
	        vk::BufferImageCopy{
	                .imageSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::ePlane0,
	                        .baseArrayLayer = 0,
	                        .layerCount = 1,
	                },
	                .imageExtent = {
	                        .width = extent.width,
	                        .height = extent.height,
	                        .depth = 1,
	                },
	        },
	        vk::BufferImageCopy{
	                .bufferOffset = vk::DeviceSize(extent.width) * extent.height,
	                .imageSubresource = {
	                        .aspectMask = vk::ImageAspectFlagBits::ePlane1,
	                        .baseArrayLayer = 0,
	                        .layerCount = 1,
	                },
	                .imageExtent = {
	                        .width = extent.width / 2,
	                        .height = extent.height / 2,
	                        .depth = 1,
	                },
	        },
	};
	cmd.copyImageToBuffer(
	        y_cbcr, vk::ImageLayout::eGeneral, in[slot].buffer,
	        std::span(regions.data(), stream_idx < 2 ? 2 : 1));

	cmd.end();

	std::unique_lock lock(vk.transfer_queue ? vk.transfer_queue.mutex : vk.queue.mutex);
	vk::CommandBufferSubmitInfo cmd_info{
	        .commandBuffer = *cmd,
	};
	compositor_sem.stageMask = vk::PipelineStageFlagBits2::eTransfer;

	vk.device.resetFences(*in[slot].fence);
	(vk.transfer_queue ? vk.transfer_queue : vk.queue)
	        .queue.submit2(vk::SubmitInfo2{
	                               .waitSemaphoreInfoCount = 1,
	                               .pWaitSemaphoreInfos = &compositor_sem,
	                               .commandBufferInfoCount = 1,
	                               .pCommandBufferInfos = &cmd_info,
	                       },
	                       *in[slot].fence);

	// Milestone 5 diagnostic (docs/ANDROID_PORT.md's perf branch): the
	// decisive brute-force synchronization test. If set, block the
	// RENDER thread here until THIS copy is fully complete before
	// returning from present_image() at all -- eliminating any
	// possibility of the render thread moving on to other GPU work
	// (the next frame's compute dispatch, the other stream's copy) while
	// this copy is still in flight, rather than letting encode() wait on
	// this fence later, asynchronously, on a different thread. If this
	// eliminates the corruption, it proves a timing/synchronization gap
	// (ours or the driver's); if it doesn't, timing is not the cause.
	// `adb shell setprop debug.xrt.WIVRN_FORCE_GPU_WAIT 1`.
	if (debug_get_num_option_force_gpu_wait())
		(void) vk.device.waitForFences(*in[slot].fence, true, UINT64_MAX);
}

std::optional<wivrn::video_encoder::data> wivrn::video_encoder_mediacodec::encode(uint8_t slot, uint64_t frame_index)
{
	scoped_timing_sample total_timer(t_encode_total);

	// Dynamic bitrate: real runtime AMediaCodec feature (see key_video_bitrate's
	// own comment). Dynamic framerate isn't: unlike x264 (full param reconfig)
	// there's no equivalently well-supported MediaCodec runtime call for it, so
	// this drains pending_framerate (so a change request doesn't pile up stale)
	// without acting on it -- a real gap, not forgotten, worth reconciling
	// alongside the zero-copy upgrade mentioned in the header.
	pending_framerate.exchange(0);
	if (auto bitrate = pending_bitrate.exchange(0))
	{
		AMediaFormat * params = AMediaFormat_new();
		AMediaFormat_setInt32(params, key_video_bitrate, int32_t(bitrate));
		AMediaCodec_setParameters(codec.get(), params);
		AMediaFormat_delete(params);
	}

	auto & idr_handler = (default_idr_handler &) *idr;
	bool is_idr = idr_handler.get_type(frame_index) == default_idr_handler::frame_type::i;
	if (is_idr)
	{
		AMediaFormat * params = AMediaFormat_new();
		AMediaFormat_setInt32(params, key_request_sync, 0); // value is ignored, presence requests it
		AMediaCodec_setParameters(codec.get(), params);
		AMediaFormat_delete(params);
	}

	{
		// This is the wait for THIS frame's GPU copy (present_image(),
		// same slot) to finish -- unlike present_image()'s own fence
		// wait (which waits on a stale slot from 2 frames ago). Real
		// wait time here means the CPU is idle waiting on the GPU
		// specifically for the frame this call is trying to encode.
		scoped_timing_sample t(t_encode_fence_wait);
		if (vk.device.waitForFences(*in[slot].fence, true, 1'000'000'000) == vk::Result::eTimeout)
		{
			U_LOG_E("Timeout on stream %d", stream_idx);
			return {};
		}
	}

	ssize_t in_idx;
	{
		scoped_timing_sample t(t_codec_input_wait);
		in_idx = AMediaCodec_dequeueInputBuffer(codec.get(), 10'000 /* 10ms */);
	}
	if (in_idx < 0)
	{
		U_LOG_W("mediacodec: no input buffer available on stream %d", stream_idx);
		return {};
	}
	size_t in_size = 0;
	uint8_t * in_buf = AMediaCodec_getInputBuffer(codec.get(), in_idx, &in_size);
	size_t payload_size = in[slot].buffer.info().size;
	auto * src = (uint8_t *) in[slot].buffer.map();

	// Real, confirmed finding on this device (see the HOST_COHERENT log
	// at construction, above): this staging buffer's memory type is
	// HOST_VISIBLE but NOT HOST_COHERENT, so the GPU's
	// vkCmdCopyImageToBuffer write (present_image()) is not guaranteed
	// visible to this CPU read without an explicit invalidate first --
	// this was previously missing. No-op-cheap if the type were ever
	// coherent instead (VMA checks internally), so this is safe to leave
	// unconditional rather than branch on the one-time property log.
	in[slot].buffer.invalidate();

	// AMEDIAFORMAT_KEY_MAX_INPUT_SIZE (ensure_codec(), above) should make
	// this impossible now -- keep the check anyway rather than silently
	// std::min()-ing and truncating the frame again if it ever isn't. This
	// was a real, independently-confirmed bug (verified: zero "too small"
	// truncation warnings after the fix), but it turned out NOT to be the
	// cause of the green-chroma corruption investigated in
	// docs/ANDROID_PORT.md's Milestone 4.5 -- that was a GPU driver bug
	// (compute writes to array layer >=1 of a multi-planar image), fixed in
	// compositor.h/.cpp instead. Kept here as a real, separate hardening.
	if (in_size < payload_size)
	{
		U_LOG_E("mediacodec: input buffer too small on stream %d: %zu < %zu, dropping frame",
		        stream_idx, in_size, payload_size);
		AMediaCodec_queueInputBuffer(codec.get(), in_idx, 0, 0, 0, 0); // give it back unused
		return {};
	}

	{
		scoped_timing_sample t(t_memcpy);
		memcpy(in_buf, src, payload_size);
	}

	// Milestone 5 diagnostic: capture the EXACT bytes MediaCodec is about
	// to encode, tagged with frame_index, to answer "was the NV12 already
	// corrupt, or did MediaCodec/hardware produce the corruption" for any
	// visibly-corrupted encoded frame found in the WIVRN_DUMP_VIDEO
	// capture. See video_encoder_mediacodec.h's nv12_dump comment.
	if (nv12_dump)
	{
		nv12_dump.write((const char *) &frame_index, sizeof(frame_index));
		nv12_dump.write((const char *) in_buf, payload_size);
		U_LOG_I("mediacodec[%d] nv12_dump: frame_index=%lu slot=%d bytes=%zu",
		        stream_idx, (unsigned long) frame_index, slot, payload_size);
	}

	check(AMediaCodec_queueInputBuffer(codec.get(), in_idx, 0, payload_size, os_monotonic_get_ns() / 1000, 0),
	      "AMediaCodec_queueInputBuffer");

	// One dequeue loop per queued input: the very first call also drains the
	// codec's one-time CODEC_CONFIG (SPS+PPS) buffer before the real frame
	// data appears, every later call gets exactly the one frame's payload
	// (KEY_LATENCY=1 requests one-in-one-out where the device honors it).
	// Bounded, not a true infinite loop: 100 * 10ms = 1s, matching the fence
	// wait timeouts used throughout this file.
	scoped_timing_sample output_wait_timer(t_codec_output_wait);
	for (int attempt = 0; attempt < 100; ++attempt)
	{
		AMediaCodecBufferInfo info{};
		ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec.get(), &info, 10'000 /* 10ms */);
		if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
			continue;
		if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED or out_idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
			continue;
		if (out_idx < 0)
		{
			U_LOG_E("mediacodec: dequeueOutputBuffer failed on stream %d: %zd", stream_idx, out_idx);
			return {};
		}

		size_t out_size = 0;
		uint8_t * out_buf = AMediaCodec_getOutputBuffer(codec.get(), out_idx, &out_size);
		std::span<uint8_t> payload(out_buf + info.offset, size_t(info.size));

		if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)
		{
			csd.assign(payload.begin(), payload.end());
			AMediaCodec_releaseOutputBuffer(codec.get(), out_idx, false);
			push_async(csd, true);
			continue; // real frame data is a separate, later output
		}

		// Copy out of MediaCodec's own output buffer *before* releasing it
		// (the buffer is invalid/reusable the instant release happens) and
		// hand the copy to the shared background sender thread instead of
		// calling SendData() here directly. encoder_work() (compositor.cpp)
		// runs one encode() per stream concurrently, but SendData() still
		// does real, possibly-multi-shard blocking socket I/O -- pushing it
		// onto the shared async sender (matching video_encoder_raw.cpp/
		// video_encoder_vulkan.cpp, the only two other backends, both of
		// which already use this) keeps that I/O off the encode-dispatch
		// path entirely. See docs/ANDROID_PORT.md's Milestone 4.5 entry.
		if (is_idr and not csd.empty())
			push_async(csd, true);
		auto payload_copy = std::make_shared<std::vector<uint8_t>>(payload.begin(), payload.end());
		AMediaCodec_releaseOutputBuffer(codec.get(), out_idx, false);
		// Milestone 5 diagnostic: pair with the nv12_dump write above --
		// this output corresponds to the frame_index just queued in THIS
		// call (KEY_LATENCY=1, one-in-one-out), so a corrupted encoded
		// access unit found in the WIVRN_DUMP_VIDEO capture can be
		// matched to its exact source NV12 frame in nv12_dump by
		// frame_index, without guessing from file position/ordering.
		if (nv12_dump)
			U_LOG_I("mediacodec[%d] output: frame_index=%lu size=%zu flags=%u idr=%d",
			        stream_idx, (unsigned long) frame_index, payload.size(), info.flags, is_idr);
		return data{
		        .encoder = this,
		        .span = *payload_copy,
		        .mem = payload_copy,
		};
	}

	U_LOG_W("mediacodec: timed out waiting for encoded output on stream %d", stream_idx);
	return {};
}
