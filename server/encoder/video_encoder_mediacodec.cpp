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
#include <string_view>
#include <vector>
#include <media/NdkMediaFormat.h>
#include <stdexcept>

// Diagnostic: see present_image()'s comment at the call site.
// `adb shell setprop debug.xrt.WIVRN_FORCE_GPU_WAIT 1` (or the env var on desktop).
DEBUG_GET_ONCE_NUM_OPTION(force_gpu_wait, "WIVRN_FORCE_GPU_WAIT", 0)

// Diagnostic: use vkCopyImageToMemory() (VK_EXT_host_image_copy) instead of a
// queue-submitted vkCmdCopyImageToBuffer() for readback, to test whether
// concurrent-stream corruption is GPU-queue-contention-related. Requires
// vk_bundle::host_image_copy. `adb shell setprop debug.xrt.WIVRN_HOST_IMAGE_COPY 1`.
DEBUG_GET_ONCE_NUM_OPTION(host_image_copy, "WIVRN_HOST_IMAGE_COPY", 0)

// Diagnostic: swaps which array layer each stream_idx reads pixel content
// from (0<->1 only), to isolate a wrong-stream content association from a
// desync elsewhere. Only this read-side derivation, not compositor.cpp's
// image_layer() (which also governs render targets).
// `adb shell setprop debug.xrt.WIVRN_SWAP_EYE_LAYERS 1`.
DEBUG_GET_ONCE_NUM_OPTION(swap_eye_layers, "WIVRN_SWAP_EYE_LAYERS", 0)

namespace
{
// NV12: full-res Y plane then half-res interleaved UV -- the same layout
// video_encoder_raw.cpp already extracts, so no conversion is needed.
constexpr int32_t color_format_yuv420_semiplanar = 21;

// AMediaCodec_setParameters() runtime keys, deliberately different strings
// from the AMEDIAFORMAT_KEY_* used at configure time (a real Android API
// quirk, not a typo) -- no NDK header exposes these as macros.
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
	if (settings.codec != h264 and settings.codec != h265)
		throw std::runtime_error("mediacodec encoder only supports h264/h265 for now");

	if (auto dump_nv12 = std::getenv("WIVRN_DUMP_NV12"))
		nv12_dump.open(std::string(dump_nv12) + "-" + std::to_string(stream_idx) + ".nv12raw", std::ios::binary);

	// Buffer is always full NV12 size regardless of stream: MediaCodec is
	// configured to expect that on every queueInputBuffer call. Stream 2
	// (alpha, Y-only) gets its chroma half filled with a neutral 0x80 below --
	// leaving it as whatever plane1 of a single-channel VkImage contained
	// produced a corrupted third H.264 stream (magenta/green color wash).
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
			// VMA_MEMORY_USAGE_AUTO picks the actual memory type at allocation
			// time -- log it once. encode()'s CPU read relies on HOST_COHERENT
			// (no manual invalidate); if this ever logs without eHostCoherent,
			// that read may see stale data.
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
	codec_kind = settings.codec;
}

void wivrn::video_encoder_mediacodec::ensure_codec()
{
	if (codec)
		return;

	// Prefer this exact hardware component name (avoids a silent software
	// fallback on Tensor); it's vendor-specific though, so fall back to
	// createEncoderByType elsewhere and still reject a known software-only
	// component by name prefix (the NDK has no isHardwareAccelerated() query).
	bool hevc = codec_kind == h265;
	const char * mime = hevc ? "video/hevc" : "video/avc";
	if (hevc)
		codec.reset(AMediaCodec_createCodecByName("c2.google.hevc.encoder"));
	if (not codec)
		codec.reset(AMediaCodec_createEncoderByType(mime));
	if (not codec)
		throw std::runtime_error(std::string("failed to create mediacodec ") + mime + " encoder");

	if (hevc)
	{
		char * name = nullptr;
		if (AMediaCodec_getName(codec.get(), &name) == AMEDIA_OK and name)
		{
			bool software = std::string_view(name).starts_with("c2.android.") or
			                 std::string_view(name).starts_with("OMX.google.");
			AMediaCodec_releaseName(codec.get(), name);
			if (software)
			{
				codec.reset();
				throw std::runtime_error("mediacodec video/hevc encoder is software-only on this device");
			}
		}
	}

	AMediaFormat * format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, extent.width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, extent.height);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, color_format_yuv420_semiplanar);
	// Pin 8-bit Main profile explicitly (no named NDK constant) -- this
	// component also advertises Main10/HDR10/HDR10+ and could otherwise
	// silently pick one of those instead.
	if (hevc)
		AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PROFILE, 0x1 /* HEVCProfileMain */);
	// Byte-buffer input isn't guaranteed tightly packed by default -- some
	// encoders assume a padded stride/slice-height and misinterpret our
	// actually-unpadded buffer (green blocky corruption) without this.
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_STRIDE, extent.width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT, extent.height);
	// Without this, Codec2 can size the input ByteBuffer smaller than one
	// real NV12 frame, silently -- encode() guards against writing past it,
	// but that means truncation, encoding as solid green for the remainder.
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

uint32_t wivrn::video_encoder_mediacodec::image_layer() const
{
	uint8_t effective_stream_idx = stream_idx;
	if (stream_idx < 2 and debug_get_num_option_swap_eye_layers())
		effective_stream_idx = 1 - stream_idx;
	return vk.multi_layer_stream_images ? effective_stream_idx : 0;
}

void wivrn::video_encoder_mediacodec::present_image(vk::Image y_cbcr, vk::SemaphoreSubmitInfo compositor_sem, uint8_t slot, uint64_t)
{
	ensure_codec();

	// vkCopyImageToMemory() diagnostic path (see host_image_copy above):
	// entirely synchronous, so in[slot].fence is left untouched (still
	// signaled) -- encode()'s waitForFences() on it is a harmless
	// immediate-return here, relying on the base class's existing
	// present_slot/encode_slot busy/idle gate instead of its own fence dance.
	if (debug_get_num_option_host_image_copy() and vk.host_image_copy)
	{
		auto t_wait_begin = os_monotonic_get_ns();
		vk::SemaphoreWaitInfo wait_info{
		        .semaphoreCount = 1,
		        .pSemaphores = &compositor_sem.semaphore,
		        .pValues = &compositor_sem.value,
		};
		auto wait_result = vk.device.waitSemaphores(wait_info, 1'000'000'000);
		t_present_fence_wait.sample(os_monotonic_get_ns() - t_wait_begin);
		if (wait_result == vk::Result::eTimeout)
		{
			U_LOG_E("Timeout waiting for compositor semaphore on stream %d", stream_idx);
			return;
		}

		std::array regions{
		        vk::ImageToMemoryCopy{
		                .pHostPointer = in[slot].buffer.map(),
		                .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::ePlane0, .baseArrayLayer = image_layer(), .layerCount = 1},
		                .imageExtent = {.width = extent.width, .height = extent.height, .depth = 1},
		        },
		        vk::ImageToMemoryCopy{
		                .pHostPointer = (uint8_t *) in[slot].buffer.map() + vk::DeviceSize(extent.width) * extent.height,
		                .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::ePlane1, .baseArrayLayer = image_layer(), .layerCount = 1},
		                .imageExtent = {.width = extent.width / 2, .height = extent.height / 2, .depth = 1},
		        },
		};
		uint32_t region_count = stream_idx < 2 ? 2 : 1;
		vk::CopyImageToMemoryInfo copy_info{
		        .srcImage = y_cbcr,
		        .srcImageLayout = vk::ImageLayout::eGeneral,
		        .regionCount = region_count,
		        .pRegions = regions.data(),
		};
		vk.device.copyImageToMemory(copy_info);
		return;
	}

	// Identical to video_encoder_raw.cpp: copy the compositor's already-NV12
	// image to our host-visible buffer (see video_encoder_mediacodec.h).
	//
	// This wait is for a STALE slot (num_slots==2, so `slot` was last used
	// two calls ago and should already be done) -- non-zero wait time here
	// means the render thread is genuinely stalling on this encoder.
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

	// See compositor.h's struct image and this class's image_layer().
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
		                             .baseArrayLayer = image_layer(),
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
	                        .baseArrayLayer = image_layer(),
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
	                        .baseArrayLayer = image_layer(),
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

	// Diagnostic: blocks the render thread here until this copy fully
	// completes, instead of letting encode() wait on the fence later, to
	// isolate a timing/synchronization gap from other causes.
	// `adb shell setprop debug.xrt.WIVRN_FORCE_GPU_WAIT 1`.
	if (debug_get_num_option_force_gpu_wait())
		(void) vk.device.waitForFences(*in[slot].fence, true, UINT64_MAX);
}

std::optional<wivrn::video_encoder::data> wivrn::video_encoder_mediacodec::encode(uint8_t slot, uint64_t frame_index)
{
	scoped_timing_sample total_timer(t_encode_total);

	// Dynamic bitrate is a real runtime AMediaCodec feature (see
	// key_video_bitrate). Dynamic framerate isn't -- no equivalent runtime
	// call exists, so this just drains pending_framerate without acting on it.
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
		// Waits for THIS frame's GPU copy (unlike present_image()'s own
		// fence wait, which is for a stale slot from 2 frames ago).
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

	// This staging buffer is HOST_VISIBLE but not HOST_COHERENT on this
	// device (see the property log at construction) -- invalidate is
	// required before this CPU read sees the GPU's write. Cheap no-op if
	// the memory type were ever coherent instead.
	in[slot].buffer.invalidate();

	// AMEDIAFORMAT_KEY_MAX_INPUT_SIZE (ensure_codec()) should make this
	// impossible -- keep the check anyway rather than silently truncating.
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

	// See nv12_dump's declaration.
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

		// Copy out of MediaCodec's output buffer before releasing it (invalid
		// the instant release happens), and hand off to the shared background
		// sender (matching video_encoder_raw.cpp/video_encoder_vulkan.cpp) so
		// SendData()'s blocking socket I/O stays off the encode-dispatch path.
		if (is_idr and not csd.empty())
			push_async(csd, true);
		auto payload_copy = std::make_shared<std::vector<uint8_t>>(payload.begin(), payload.end());
		AMediaCodec_releaseOutputBuffer(codec.get(), out_idx, false);
		// Pairs with the nv12_dump write above via frame_index (KEY_LATENCY=1
		// makes this one-in-one-out).
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
