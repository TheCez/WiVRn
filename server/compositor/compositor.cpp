/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.

#include "compositor.h"

// Monado includes
#include "driver/xrt_cast.h"
#include "main/comp_frame.h"
#include "util/comp_render_helpers.h"
#include "util/comp_vulkan.h"
#include "util/u_debug.h"
#include "util/u_handles.h"
#include "util/u_time.h"
#include "vk/vk_helpers.h"

#include "driver/wivrn_session.h"
#include "encoder/video_encoder.h"
#include "inplace_vector.hpp"
#include "utils/enumerate_polyfill.h"
#include "utils/method.h"
#include "utils/wivrn_trace.h"

#include "xrt/xrt_config_build.h" // IWYU pragma: keep

#include <chrono>
#include <format>
#include <fstream>

#ifdef XRT_FEATURE_RENDERDOC
#include "renderdoc_app.h"

static auto renderdoc()
{
	auto x = []() {
		RENDERDOC_API_1_5_0 * rdoc_api = nullptr;
		const char * env = std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
		if (not env or env != std::string_view("1"))
			return rdoc_api;
		void * mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
		if (mod)
		{
			pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
			XRT_MAYBE_UNUSED int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_5_0, (void **)&rdoc_api);
			assert(ret == 1);
		}
		return rdoc_api;
	};
	static auto res = x();
	return res;
}
#endif

DEBUG_GET_ONCE_LOG_OPTION(log, "XRT_COMPOSITOR_LOG", U_LOGGING_INFO)

// Diagnostic: -1 (default) both streams run normally; 0 or 1 restricts real
// present_image()/encode() calls to that stream_idx only, to isolate
// per-stream vs. concurrent-operation corruption.
// Android: `adb shell setprop debug.xrt.WIVRN_ONLY_STREAM 0` (env var forwarding
// doesn't reach this macro's Android backend, must use setprop).
DEBUG_GET_ONCE_NUM_OPTION(only_stream, "WIVRN_ONLY_STREAM", -1)

// Diagnostic: forces encode() calls back to sequential (pre-concurrent-encode
// behavior) without changing GPU submission concurrency, to isolate CPU-thread
// races from GPU/VPU contention. `adb shell setprop debug.xrt.WIVRN_SERIALIZE_ENCODE 1`.
DEBUG_GET_ONCE_NUM_OPTION(serialize_encode, "WIVRN_SERIALIZE_ENCODE", 0)

// Diagnostic: delays the second stream's present_image()/encode() start by this
// many microseconds relative to the first, to spread GPU/VPU work apart in time
// without dropping frames. `adb shell setprop debug.xrt.WIVRN_STAGGER_US <us>`.
DEBUG_GET_ONCE_NUM_OPTION(stagger_us, "WIVRN_STAGGER_US", 0)

// Diagnostic: logs per-view pose/array_index/image_index every frame on the fast
// path, to check for view-extraction bugs. `adb shell setprop debug.xrt.WIVRN_LOG_VIEW_POSE 1`.
DEBUG_GET_ONCE_NUM_OPTION(log_view_pose, "WIVRN_LOG_VIEW_POSE", 0)

namespace details
{
template <auto Method, typename Result, typename... Args>
struct method_trait<Method, Result (wivrn::compositor::*)(Args...)>
{
	static Result magic(xrt_compositor * arg, Args... args)
	{
		return std::invoke(
		        Method,
		        static_cast<wivrn::compositor *>(reinterpret_cast<struct comp_base *>(arg)),
		        args...);
	}
};

} // namespace details

namespace
{
const comp_swapchain_image & get_layer_image(const comp_layer & layer, uint32_t swapchain_index, uint32_t image_index)
{
	return reinterpret_cast<struct comp_swapchain *>(comp_layer_get_swapchain(&layer, swapchain_index))->images[image_index];
}

// Diagnostic: dumps the app's own swapchain image (before our foveation
// shader touches it) to a raw RGBA file, one-shot per view, to check whether
// corruption is already present in what Monado handed us.
// `adb shell setprop debug.xrt.WIVRN_DUMP_APP_IMAGE 1`. Not DEBUG_GET_ONCE_NUM_OPTION:
// that macro latches its value for the process lifetime, so it wouldn't work
// as a live toggle flipped mid-session.
void dump_app_image_once(wivrn::vk_bundle & vk, vk::raii::CommandPool & cmd_pool, const comp_layer & layer, uint32_t swapchain_index, uint32_t image_index, uint32_t array_index, int view)
{
	static std::array<bool, 2> dumped{false, false};
	if (view < 0 or view > 1 or dumped[view] or not debug_get_num_option("WIVRN_DUMP_APP_IMAGE", 0))
		return;
	dumped[view] = true;

	auto * sc = reinterpret_cast<struct comp_swapchain *>(comp_layer_get_swapchain(&layer, swapchain_index));
	vk::Image image = sc->vkic.images[image_index].handle;
	vk::Extent2D extent{sc->vkic.info.width, sc->vkic.info.height};

	vk::DeviceSize size = vk::DeviceSize(extent.width) * extent.height * 4;
	buffer_allocation staging(
	        vk.device,
	        {
	                .size = size,
	                .usage = vk::BufferUsageFlagBits::eTransferDst,
	        },
	        {
	                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
	                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
	        },
	        std::format("dump_app_image staging {}", view));

	auto cmd_buffers = vk.device.allocateCommandBuffers({.commandPool = *cmd_pool, .commandBufferCount = 1});
	vk::raii::CommandBuffer cmd{std::move(cmd_buffers[0])};
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	vk::ImageSubresourceRange range{
	        .aspectMask = vk::ImageAspectFlagBits::eColor,
	        .baseMipLevel = 0,
	        .levelCount = 1,
	        .baseArrayLayer = array_index,
	        .layerCount = 1,
	};
	cmd.pipelineBarrier(
	        vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eShaderRead,
	                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
	                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
	                .image = image,
	                .subresourceRange = range,
	        });
	cmd.copyImageToBuffer(
	        image, vk::ImageLayout::eTransferSrcOptimal, vk::Buffer(staging),
	        vk::BufferImageCopy{
	                .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor, .mipLevel = 0, .baseArrayLayer = array_index, .layerCount = 1},
	                .imageExtent = {extent.width, extent.height, 1},
	        });
	cmd.pipelineBarrier(
	        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
	        vk::ImageMemoryBarrier{
	                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
	                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
	                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
	                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	                .image = image,
	                .subresourceRange = range,
	        });
	cmd.end();

	vk::raii::Fence fence(vk.device, vk::FenceCreateInfo{});
	{
		vk::CommandBufferSubmitInfo cmd_info{.commandBuffer = *cmd};
		std::unique_lock lock{vk.queue.mutex};
		vk.queue.queue.submit2(vk::SubmitInfo2{.commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd_info}, *fence);
	}
	if (vk.device.waitForFences(*fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		U_LOG_E("dump_app_image_once: timeout waiting for GPU, view=%d", view);
		return;
	}
	staging.invalidate();

	auto path = std::format("/data/data/org.meumeu.wivrn.server/files/dump_app_image_view{}.rgba", view);
	std::ofstream f(path, std::ios::binary);
	f.write(staging.data<char>(), size);
	U_LOG_E("dump_app_image_once: wrote %s (%ux%u, array_layer=%u)", path.c_str(), extent.width, extent.height, array_index);
}

// Array layer for stream_idx (0=left, 1=right, 2=alpha): 0 on the PowerVR
// single-layer workaround, else stream_idx -- see compositor.h's struct image.
uint32_t image_layer(const wivrn::vk_bundle & vk, uint8_t stream_idx)
{
	return vk.multi_layer_stream_images ? stream_idx : 0;
}

std::array<vk::Format, 3> image_formats(int bit_depth)
{
	switch (bit_depth)
	{
		case 8:
			return {
			        vk::Format::eR8Unorm,
			        vk::Format::eR8G8Unorm,
			        vk::Format::eG8B8R82Plane420Unorm,
			};
		case 10:
			return {
			        vk::Format::eR16Unorm,
			        vk::Format::eR16G16Unorm,
			        vk::Format::eG10X6B10X6R10X62Plane420Unorm3Pack16,
			};
	}
	throw std::runtime_error(std::format("Unsupported bit depth {}", bit_depth));
}

// Backing allocation for one or more stream images: array_layers=1 (PowerVR
// workaround, one call per stream) or =3 (shared left+right+alpha elsewhere).
image_allocation make_stream_storage(
        wivrn::vk_bundle & vk,
        std::span<const vk::Format, 3> formats,
        vk::Extent3D extent,
        vk::ImageCreateFlags extra_flags,
        vk::ImageUsageFlags extra_usage,
        uint32_t array_layers,
        const char * name)
{
	// Needed at creation time so video_encoder_mediacodec.cpp can optionally
	// read back via vkCopyImageToMemory() instead of vkCmdCopyImageToBuffer().
	vk::ImageUsageFlags host_transfer_usage = vk.host_image_copy ? vk::ImageUsageFlagBits::eHostTransfer : vk::ImageUsageFlags{};

	vk::StructureChain image_info{
	        vk::ImageCreateInfo{
	                .flags = vk::ImageCreateFlagBits::eExtendedUsage | vk::ImageCreateFlagBits::eMutableFormat | extra_flags,
	                .imageType = vk::ImageType::e2D,
	                .format = formats.back(),
	                .extent = extent,
	                .mipLevels = 1,
	                .arrayLayers = array_layers,
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc | host_transfer_usage | extra_usage,
	        },
	        vk::ImageFormatListCreateInfo{
	                .viewFormatCount = uint32_t(formats.size()),
	                .pViewFormats = formats.data(),
	        },
	};

	return image_allocation{
	        vk.device,
	        image_info.get(),
	        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
	        name,
	};
}

// Y/CbCr views into array_layer of vk_image (0, or 0/1/2 for left/right/alpha
// depending on the PowerVR workaround -- see make_stream_storage above).
wivrn::compositor::stream_image make_stream_view(
        wivrn::vk_bundle & vk,
        vk::Image vk_image,
        std::span<const vk::Format, 3> formats,
        uint32_t array_layer)
{
	vk::ImageViewUsageCreateInfo usage{
	        .usage = vk::ImageUsageFlagBits::eStorage,
	};
	return wivrn::compositor::stream_image{
	        .image = vk_image,
	        .view_y{
	                vk.device,
	                {
	                        .pNext = &usage,
	                        .image = vk_image,
	                        .viewType = vk::ImageViewType::e2DArray,
	                        .format = formats[0],
	                        .subresourceRange = {
	                                .aspectMask = vk::ImageAspectFlagBits::ePlane0,
	                                .levelCount = 1,
	                                .baseArrayLayer = array_layer,
	                                .layerCount = 1,
	                        },
	                },
	        },
	        .view_cbcr{
	                vk.device,
	                {
	                        .pNext = &usage,
	                        .image = vk_image,
	                        .viewType = vk::ImageViewType::e2DArray,
	                        .format = formats[1],
	                        .subresourceRange = {
	                                .aspectMask = vk::ImageAspectFlagBits::ePlane1,
	                                .levelCount = 1,
	                                .baseArrayLayer = array_layer,
	                                .layerCount = 1,
	                        },
	                },
	        }};
}

std::array<wivrn::compositor::image, 2> make_images(wivrn::vk_bundle & vk, vk::CommandPool command_pool, std::span<wivrn::encoder_settings> encoders)
{
	auto formats = image_formats(encoders[0].bit_depth);

	vk::ImageCreateFlags extra_flags{};
	vk::ImageUsageFlags extra_usage{};
#if WIVRN_USE_VULKAN_ENCODE
	if (
	        std::get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>(vk.feat).videoMaintenance1 and
	        std::ranges::contains(
	                encoders,
	                wivrn::encoder_vulkan,
	                &wivrn::encoder_settings::encoder_name))
	{
		extra_flags |= vk::ImageCreateFlagBits::eVideoProfileIndependentKHR;
		extra_usage |= vk::ImageUsageFlagBits::eVideoEncodeSrcKHR;
	}
#endif

	vk::Extent3D extent{.width = encoders[0].width, .height = encoders[0].height, .depth = 1};

	auto make_slot = [&](int i) -> wivrn::compositor::image {
		if (vk.multi_layer_stream_images)
		{
			image_allocation combined = make_stream_storage(vk, formats, extent, extra_flags, extra_usage, 3, std::format("compositor YCbCr image {}", i).c_str());
			vk::Image handle = combined;

			std::vector<image_allocation> storage;
			storage.push_back(std::move(combined));

			return wivrn::compositor::image{
			        .storage = std::move(storage),
			        .extent = extent,
			        .content = {
			                make_stream_view(vk, handle, formats, 0),
			                make_stream_view(vk, handle, formats, 1),
			        },
			        .alpha = make_stream_view(vk, handle, formats, 2),
			};
		}

		image_allocation left = make_stream_storage(vk, formats, extent, extra_flags, extra_usage, 1, std::format("compositor YCbCr image {} left", i).c_str());
		image_allocation right = make_stream_storage(vk, formats, extent, extra_flags, extra_usage, 1, std::format("compositor YCbCr image {} right", i).c_str());
		image_allocation alpha_storage = make_stream_storage(vk, formats, extent, extra_flags, extra_usage, 1, std::format("compositor YCbCr image {} alpha", i).c_str());

		vk::Image left_handle = left, right_handle = right, alpha_handle = alpha_storage;

		std::vector<image_allocation> storage;
		storage.reserve(3);
		storage.push_back(std::move(left));
		storage.push_back(std::move(right));
		storage.push_back(std::move(alpha_storage));

		return wivrn::compositor::image{
		        .storage = std::move(storage),
		        .extent = extent,
		        .content = {
		                make_stream_view(vk, left_handle, formats, 0),
		                make_stream_view(vk, right_handle, formats, 0),
		        },
		        .alpha = make_stream_view(vk, alpha_handle, formats, 0),
		};
	};

	return {make_slot(0), make_slot(1)};
}

vk::raii::Semaphore make_semaphore(wivrn::vk_bundle & vk)
{
	vk::raii::Semaphore res{
	        vk.device,
	        vk::StructureChain{
	                vk::SemaphoreCreateInfo{},
	                vk::SemaphoreTypeCreateInfo{
	                        .semaphoreType = vk::SemaphoreType::eTimeline,
	                },
	        }
	                .get(),
	};
	vk.name(res, "compositor semaphore");
	return res;
}

vk::Extent3D render_extent(const wivrn::from_headset::headset_info_packet & info)
{
	return {
	        .width = info.render_eye_width,
	        .height = info.render_eye_height,
	        .depth = 1,
	};
}

} // namespace

namespace wivrn
{

void compositor::timings::add(float us)
{
	int index = this->index;
	this->index = (this->index + 1) % values.size();
	values[index] = us;
}

xrt_result_t compositor::predict_frame(int64_t * out_frame_id,
                                       int64_t * out_wake_time_ns,
                                       int64_t * out_predicted_gpu_time_ns,
                                       int64_t * out_predicted_display_time_ns,
                                       int64_t * out_predicted_display_period_ns)
{
	int64_t frame_id = -1;
	int64_t wake_up_time_ns = 0;
	int64_t present_slop_ns = 0;
	int64_t desired_present_time_ns = 0;
	int64_t predicted_display_time_ns = 0;
	pacer.predict(
	        frame_id,
	        wake_up_time_ns,
	        desired_present_time_ns,
	        present_slop_ns,
	        predicted_display_time_ns);

	frame.waited.id = frame_id;
	frame.waited.desired_present_time_ns = desired_present_time_ns;
	frame.waited.present_slop_ns = present_slop_ns;
	frame.waited.predicted_display_time_ns = predicted_display_time_ns;

	*out_frame_id = frame_id;
	*out_wake_time_ns = wake_up_time_ns;
	*out_predicted_gpu_time_ns = desired_present_time_ns; // Not quite right but close enough.
	*out_predicted_display_time_ns = predicted_display_time_ns;
	*out_predicted_display_period_ns = pacer.get_frame_duration();
	return XRT_SUCCESS;
}

xrt_result_t compositor::mark_frame(int64_t frame_id,
                                    xrt_compositor_frame_point point,
                                    int64_t when_ns)
{
	switch (point)
	{
		case XRT_COMPOSITOR_FRAME_POINT_WOKE:
			trace::instant_feedback("wake_up", when_ns, frame_id);
			return XRT_SUCCESS;
		default:
			assert(false);
	}
	return XRT_ERROR_VULKAN;
}

xrt_result_t compositor::layer_commit(xrt_graphics_sync_handle_t sync_handle)
{
	u_graphics_sync_unref(&sync_handle);

	// Move waited frame to rendering frame, clear waited.
	comp_frame_move_and_clear_locked(&frame.rendering, &frame.waited);

	U_LOG_IFL_D(log_level, "frame %ld commit %d layers", frame.rendering.id, layer_accum.layer_count);

	if (encode_request >= 0 // encoders have not picked up the previous frame
	    or not session.connected() or not session.get_offset())
	{
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}

	int i = acquire_image();
	if (i < 0)
	{
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->StartFrameCapture(NULL, NULL);
#endif

	auto & view_info = images[i].view_info;
	images[i].frame_index = frame.rendering.id;
	view_info = {
	        .display_time = session.get_offset().to_headset(frame.rendering.predicted_display_time_ns),
	        .alpha = layer_accum.data.env_blend_mode == XRT_BLEND_MODE_ALPHA_BLEND,
	};

	trace::instant_feedback("begin", frame.rendering.id, os_monotonic_get_ns());

	cmd_pool.reset();
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	cmd.resetQueryPool(*query_pool, 0, 3);
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *query_pool, 0);

	bool flip_y = false;
	std::array<vk::ImageView, 2> src;
	std::array<xrt_rect, 2> src_rect;
	std::array<xrt_fov, 2> src_fov;

	// Capacity 5 = 1 squasher barrier + 3 per-stream barriers + margin.
	// inplace_vector doesn't grow -- it throws when full, not silently reallocates.
	beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 5> image_barriers;

	// Check if we can pass a layer directly to foveation
	if (layer_accum.layer_count == 1 and
	    (layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION or
	     layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION_DEPTH))
	{
		const auto & layer = layer_accum.layers[0];
		for (int view = 0; view < 2; ++view)
		{
			const auto & data = (layer.data.type == XRT_LAYER_PROJECTION ? layer.data.proj.v : layer.data.depth.v)[view];
			auto & img = get_layer_image(layer, view, data.sub.image_index);

			dump_app_image_once(vk, cmd_pool, layer, view, data.sub.image_index, data.sub.array_index, view);

			if (debug_get_num_option_log_view_pose())
				U_LOG_E("view=%d image_index=%u array_index=%u pose_pos=(%f,%f,%f) pose_orient=(%f,%f,%f,%f)",
				        view,
				        data.sub.image_index,
				        data.sub.array_index,
				        data.pose.position.x, data.pose.position.y, data.pose.position.z,
				        data.pose.orientation.x, data.pose.orientation.y, data.pose.orientation.z, data.pose.orientation.w);

			src[view] = get_image_view(
			        &img,
			        layer.data.flags,
			        data.sub.array_index);
			src_rect[view] = data.sub.rect;
			src_fov[view] = data.fov;
			flip_y = layer.data.flip_y;
			view_info.pose[view] = xrt_cast(data.pose);
			view_info.fov[view] = xrt_cast(data.fov);
		}
	}
	else
	{
		// no fast-path, squash layers
		std::array<xrt_pose, 2> poses;
		const auto extent = images[0].extent;
		std::tie(poses, src_fov, src_rect) = squasher.do_layers(
		        vk.device,
		        cmd,
		        session.get_hmd(),
		        pacer.get_frame_duration(),
		        frame.rendering,
		        layer_accum,
		        xrt_rect{.extent{.w = int(extent.width), .h = int(extent.height)}});

		src = squasher.get_views();

		for (int view = 0; view < 2; ++view)
		{
			view_info.pose[view] = xrt_cast(poses[view]);
			view_info.fov[view] = xrt_cast(src_fov[view]);
		}

		image_barriers.push_back(
		        vk::ImageMemoryBarrier2{
		                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		                .oldLayout = vk::ImageLayout::eGeneral,
		                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		                .image = squasher.get_image(),
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .layerCount = 2,
		                },
		        });
	}

	// One barrier per stream image/layer -- see compositor.h's struct image.
	struct
	{
		vk::Image image;
		uint32_t layer;
	} stream_targets[3] = {
	        {images[i].content[0].image, 0},
	        {images[i].content[1].image, image_layer(vk, 1)},
	        {images[i].alpha.image, image_layer(vk, 2)},
	};
	for (auto & target: stream_targets)
	{
		image_barriers.push_back(
		        vk::ImageMemoryBarrier2{
		                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		                .oldLayout = vk::ImageLayout::eUndefined,
		                .newLayout = vk::ImageLayout::eGeneral,
		                .image = target.image,
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .baseArrayLayer = target.layer,
		                        .layerCount = 1,
		                },
		        });
	}

	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
	        .pImageMemoryBarriers = image_barriers.data(),
	});
	image_barriers.clear();

	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 1);

	if (session.get_info().eye_gaze)
	{
		auto now = os_monotonic_get_ns();
		session.add_tracking_request(device_id::EYE_GAZE, frame.rendering.desired_present_time_ns, now, now);
	}
	view_info.foveation = foveation.foveate(
	        vk.device,
	        cmd,
	        {*images[i].content[0].view_y, *images[i].content[1].view_y},
	        {*images[i].content[0].view_cbcr, *images[i].content[1].view_cbcr},
	        *images[i].alpha.view_y,
	        *images[i].alpha.view_cbcr,
	        flip_y,
	        src,
	        src_rect,
	        src_fov,
	        view_info.alpha);

	// See compositor.h's struct image for the dedicated-image-vs-shared-layer split.
	auto stream_vk_image = [&](uint8_t stream_idx) -> vk::Image {
		return stream_idx < 2 ? vk::Image(images[i].content[stream_idx].image) : vk::Image(images[i].alpha.image);
	};

	for (auto & encoder: encoders)
	{
		if (encoder->stream_idx == 2 and not view_info.alpha)
			continue;
		else if (auto only = debug_get_num_option_only_stream(); only >= 0 and encoder->stream_idx != only)
			continue;
		else if (encoder->need_transfer or encoder->target_queue == vk.queue.family_index)
		{
			image_barriers.push_back(
			        vk::ImageMemoryBarrier2{
			                .srcStageMask = vk::PipelineStageFlagBits2KHR::eComputeShader,
			                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
			                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
			                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
			                // For a queue-family ownership transfer in EXCLUSIVE
			                // sharing mode, the release barrier's old/new layout
			                // must match the encoder-side acquire. newLayout is
			                // the encoder's target_layout; per the VkImageMemoryBarrier2
			                // spec the layout transition is executed exactly once
			                // between the queues, so this single QFOT barrier covers
			                // both the queue-family transfer and the layout
			                // transition the encoder needs.
			                .oldLayout = vk::ImageLayout::eGeneral,
			                .newLayout = encoder->target_layout,
			                .srcQueueFamilyIndex = vk.queue.family_index,
			                .dstQueueFamilyIndex = encoder->target_queue,
			                .image = stream_vk_image(encoder->stream_idx),
			                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
			                                     .baseMipLevel = 0,
			                                     .levelCount = 1,
			                                     .baseArrayLayer = image_layer(vk, encoder->stream_idx),
			                                     .layerCount = 1},
			        });
		}
	}

	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
	        .pImageMemoryBarriers = image_barriers.data(),
	});
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 2);

	cmd.end();

	const vk::SemaphoreSubmitInfo sem_info{
	        .semaphore = *sem,
	        .value = ++sem_value,
	        .stageMask = vk::PipelineStageFlagBits2::eComputeShader,
	};

	{
		vk::CommandBufferSubmitInfo cmd_info{
		        .commandBuffer = cmd,
		};
		std::unique_lock lock{vk.queue.mutex};
		vk.queue.queue.submit2(vk::SubmitInfo2{
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cmd_info,
		        .signalSemaphoreInfoCount = 1,
		        .pSignalSemaphoreInfos = &sem_info,
		});
	}

	pacer.mark_timing_point(COMP_TARGET_TIMING_POINT_SUBMIT_END, frame.rendering.id, os_monotonic_get_ns());
	auto info = pacer.present_to_info(frame.rendering.desired_present_time_ns);

	bool first_present = true;
	for (auto & encoder: encoders)
	{
		if (encoder->stream_idx == 2 and not view_info.alpha)
			continue;
		if (auto only = debug_get_num_option_only_stream(); only >= 0 and encoder->stream_idx != only)
			continue;
		if (auto us = debug_get_num_option_stagger_us(); us > 0 and not first_present)
			std::this_thread::sleep_for(std::chrono::microseconds(us));
		first_present = false;
		encoder->present_image(
		        stream_vk_image(encoder->stream_idx),
		        sem_info,
		        info.frame_id);
	}

	auto j = encode_request.exchange(i);
	encode_request.notify_all();
	assert(j == -1);

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->EndFrameCapture(NULL, NULL);
#endif

	comp_frame_clear_locked(&frame.rendering);

	if (vk.device.waitSemaphores(vk::SemaphoreWaitInfo{
	                                     .semaphoreCount = 1,
	                                     .pSemaphores = &*sem,
	                                     .pValues = &sem_info.value,
	                             },
	                             U_TIME_1S_IN_NS) == vk::Result::eTimeout)
	{
		U_LOG_IFL_W(log_level, "compositor timeout");
	}
	else
	{
		auto [res, ts] = query_pool.getResults<uint64_t>(
		        0,
		        3,
		        3 * sizeof(uint64_t),
		        sizeof(uint64_t),
		        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);

		if (res == vk::Result::eSuccess)
		{
			static const auto period = vk.physical_device.getProperties().limits.timestampPeriod;
			squasher_times.add((ts[1] - ts[0]) * period / 1e3);
			foveation_times.add((ts[2] - ts[1]) * period / 1e3);
			squasher_gpu_time_log.sample(int64_t((ts[1] - ts[0]) * period));
			foveation_gpu_time_log.sample(int64_t((ts[2] - ts[1]) * period));
		}
	}

	// Now is a good point to garbage collect.
	comp_swapchain_shared_garbage_collect(&cscs);

	return XRT_SUCCESS;
}

xrt_result_t compositor::get_display_refresh_rate(float * hz)
{
	auto settings = session.get_settings();
	// there should not be rounding errors, hz must be one of the available refresh rates
	*hz = frame_rate * settings->fps_divider;
	assert(std::ranges::contains(session.get_info().available_refresh_rates, *hz));
	return XRT_SUCCESS;
}

xrt_result_t compositor::request_display_refresh_rate(float hz)
{
	requested_refresh_rate = hz;
	U_LOG_I("request refresh rate: %fHz", hz);
	if (hz > 0)
	{
		try
		{
			session.send_control(to_headset::refresh_rate_change{.hz = hz});
		}
		catch (std::exception & e)
		{
			U_LOG_W("refresh rate change failed: %s", e.what());
		}
	}
	return XRT_SUCCESS;
}

xrt_result_t compositor::get_view_config(
        xrt_compositor_native * self,
        xrt_view_type view_type,
        xrt_view_config * out_view_config)
{
	const auto & session = reinterpret_cast<compositor *>(self)->session;
	const auto extent = render_extent(session.get_info());
	switch (view_type)
	{
		case XRT_VIEW_TYPE_MONO:
			return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
		case XRT_VIEW_TYPE_STEREO:
			*out_view_config = {
			        .view_type = XRT_VIEW_TYPE_STEREO,
			        .view_count = 2,
			        .views = {}};
			for (auto & view: std::span(out_view_config->views, out_view_config->view_count))
			{
				view = {
				        .recommended = {
				                .width_pixels = extent.width,
				                .height_pixels = extent.height,
				                .sample_count = 1,
				        },
				        .max = {
				                .width_pixels = extent.width * 2u,
				                .height_pixels = extent.height * 2u,
				                .sample_count = 1,
				        },
				};
			}
			return XRT_SUCCESS;
		case XRT_VIEW_TYPE_QUAD:
			return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
	}
	return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
}

int compositor::acquire_image()
{
	for (auto [i, image]: std::ranges::enumerate_view(images))
	{
		if (not image.busy.exchange(true))
			return i;
	}
	return -1;
}

void compositor::encoder_work(std::stop_token tok)
{
	while (not tok.stop_requested())
	{
		auto req = encode_request.exchange(-1);
		if (req < 0)
		{
			encode_request.wait(req);
			wivrn::trace::cpu_instant(wivrn::trace::cpu_track::compositor, "encoder_work wake", 0, 0);
			continue;
		}

		assert(req < images.size());
		auto & image = images[req];

		wivrn::trace::scope trace_iter(wivrn::trace::cpu_track::compositor, 0, image.frame_index, "encoder_work iter");

		// Concurrent per-stream encode: AMediaCodec's per-call JNI/Binder
		// overhead means sequential calls starve whichever stream goes second,
		// every frame. wivrn_connection::send_control/send_stream gained a
		// mutex for this since concurrent encode() calls can now race on the
		// same socket.
		{
			auto do_encode = [&](video_encoder * e) {
				try
				{
					e->encode(session, image.view_info, image.frame_index);
				}
				catch (std::exception & ex)
				{
					U_LOG_W("encode error: %s", ex.what());
				}
			};

			beman::inplace_vector::inplace_vector<std::jthread, 3> workers;
			bool first_encode = true;
			for (auto & encoder: encoders)
			{
				if (auto only = debug_get_num_option_only_stream(); only >= 0 and encoder->stream_idx != only)
					continue;
				if (encoder->stream_idx < 2 or image.view_info.alpha)
				{
					if (auto us = debug_get_num_option_stagger_us(); us > 0 and not first_encode)
						std::this_thread::sleep_for(std::chrono::microseconds(us));
					first_encode = false;
					if (debug_get_num_option_serialize_encode())
						do_encode(encoder.get());
					else
						workers.emplace_back([&, e = encoder.get()] { do_encode(e); });
				}
			}
			// workers' jthreads join here as it goes out of scope, before
			// this image is marked free for reuse.
		}
		image.busy = false;
	}
}

void compositor::send_video_stream_description()

{
	to_headset::video_stream_description desc{
	        .width = uint16_t(images[0].extent.width),
	        .height = uint16_t(images[0].extent.height),
	        .frame_rate = settings[0].fps,
	};
	get_display_refresh_rate(&desc.refresh_rate);
	static_assert(std::tuple_size_v<decltype(settings)> == std::tuple_size_v<decltype(desc.codec)>);
	std::ranges::transform(settings, desc.codec.begin(), &encoder_settings::codec);
	session.send_control(std::move(desc));
}

compositor::compositor(wivrn_session & session) :
        comp_base{
                .base = {
                        .base = {
                                .info{},
                                .begin_session = method_pointer<&compositor::begin_session>,
                                .end_session = method_pointer<&compositor::end_session>,
                                .predict_frame = method_pointer<&compositor::predict_frame>,
                                .mark_frame = method_pointer<&compositor::mark_frame>,
                                .begin_frame = method_pointer<&compositor::begin_frame>,
                                .discard_frame = method_pointer<&compositor::discard_frame>,
                                .layer_commit = method_pointer<&compositor::layer_commit>,
                                .get_display_refresh_rate = method_pointer<&compositor::get_display_refresh_rate>,
                                .request_display_refresh_rate = method_pointer<&compositor::request_display_refresh_rate>,
                                .destroy = method_pointer<&compositor::destroy>,
                        },
                },
        },
        log_level(debug_get_log_option_log()),
        session(session),
        cmd_pool(vk.device, vk::CommandPoolCreateInfo{
                                    .flags = vk::CommandPoolCreateFlagBits::eTransient,
                                    .queueFamilyIndex = vk.queue.family_index,
                            }),
        query_pool(vk.device, vk::QueryPoolCreateInfo{
                                      .queryType = vk::QueryType::eTimestamp,
                                      .queryCount = 3,
                              }),
        settings(get_encoder_settings(vk, session)),
        images{make_images(vk, cmd_pool, settings)},
        cmd{std::move(vk.device.allocateCommandBuffers({.commandPool = *cmd_pool, .commandBufferCount = 1})[0])},
        sem{make_semaphore(vk)},
        frame_rate(settings[0].fps),
        pacer(U_TIME_1S_IN_NS / frame_rate),
        squasher(vk, render_extent(session.get_info())),
        foveation(vk, images[0].extent)
{
	comp_base * c_base = this;
	// Ensure we can safely cast pointers
	assert(intptr_t(&base) == intptr_t(this));
	auto res = vk_init_from_given(
	        &c_base->vk,
	        (PFN_vkGetInstanceProcAddr)vk.instance.getProcAddr("vkGetInstanceProcAddr"),
	        *vk.instance,
	        *vk.physical_device,
	        *vk.device,
	        vk.queue.family_index,
	        0,
	        vk.has_device_ext(VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME),
	        vk.has_device_ext(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME),
	        std::get<vk::PhysicalDeviceVulkan12Features>(vk.feat).timelineSemaphore,
	        true, // in Vulkan 1.2
	        vk.has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	        log_level);
	vk::detail::resultCheck(vk::Result(res), "vk_init_from_given");

	c_base->vk.version = vk_bundle::api_version;
	// vk_init_from_given can't enable calibrated timestamps; do it here.
#ifdef VK_EXT_calibrated_timestamps
	c_base->vk.has_EXT_calibrated_timestamps = vk.has_device_ext(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#endif

	// Share monado's vk_bundle so gpu_timestamp_pool reuses its calibration cache.
	wivrn::trace::set_calibration_source(&c_base->vk);

	// vk_init_from_given assumes a graphics queue was provided
	c_base->vk.graphics_queue = nullptr;

	// Monado submits to the main queue from IPC client threads under
	// main_queue->mutex: our own submissions must take the same lock.
	vk::detail::resultCheck(vk::Result(vk_init_mutex(&c_base->vk)), "vk_init_mutex");
	if (c_base->vk.main_queue)
		vk.queue.mutex.share(c_base->vk.main_queue->mutex.mutex);

	{
		comp_vulkan_formats formats{};
		comp_vulkan_formats_check(&c_base->vk, &formats);
		comp_vulkan_formats_copy_to_info(&formats, &base.base.info);
		comp_vulkan_formats_log(log_level, &formats);
	}

	comp_base_init(this);

	// Tie the lifetimes of swapchains to Vulkan.
	xrt_result_t xret = comp_swapchain_shared_init(&cscs, &c_base->vk);
	if (xret != XRT_SUCCESS)
		throw std::runtime_error("comp_swapchain_shared_init failed");

	print_encoders(settings);
	for (auto [i, settings]: std::ranges::enumerate_view(settings))
		encoders[i] = video_encoder::create(vk, settings, i);
	send_video_stream_description();

	u_var_add_root(this, "Compositor", false);
	u_var_add_f32_timing(this, &squasher_times.var, "layers processing");
	u_var_add_f32_timing(this, &foveation_times.var, "foveation");

	// Start the thread after everything is initialized
	encoder_thread = std::jthread{[&](std::stop_token t) { encoder_work(t); }};
}

compositor::~compositor()
{
	u_var_remove_root(this);
	encoder_thread.request_stop();
	encode_request = -2;
	encode_request.notify_all();
	comp_base * c_base = this;
	comp_swapchain_shared_garbage_collect(&cscs);
	comp_swapchain_shared_destroy(&cscs, &c_base->vk);
	comp_base_fini(this);
}

xrt_system_compositor_info compositor::sys_info() const
{
	const auto & info = session.get_info();
	auto [prop, dev_id] = vk.physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
	xrt_system_compositor_info res{
	        .view_type_count = 1,
	        .view_types = {
	                XRT_VIEW_TYPE_STEREO,
	        },
	        .max_layers = squasher.max_layers(prop.properties),
	        .supported_blend_modes = {
	                XRT_BLEND_MODE_OPAQUE,
	                XRT_BLEND_MODE_ALPHA_BLEND,
	        },
	        .supported_blend_mode_count = uint8_t(1 + info.passthrough),
	        .refresh_rate_count = std::min<uint32_t>(info.available_refresh_rates.size(), std::size(res.refresh_rates_hz)),
	        .supports_fov_mutable = true,
	};

	std::ranges::copy(dev_id.deviceUUID, res.compositor_vk_deviceUUID.data);
	std::ranges::copy(dev_id.deviceUUID, res.client_vk_deviceUUID.data);
	std::ranges::copy(std::span(info.available_refresh_rates).subspan(0, res.refresh_rate_count), res.refresh_rates_hz);
	return res;
}

void compositor::set_bitrate(uint32_t bitrate)
{
	for (auto & encoder: encoders)
		encoder->set_bitrate(bitrate);
}

void compositor::set_framerate(float hz)
{
	if (frame_rate.exchange(hz) == hz)
		return;
	U_LOG_IFL_D(log_level, "Framerate change from %.0f to %.0f", frame_rate.load(), hz);
	pacer.set_frame_duration(U_TIME_1S_IN_NS / hz);
	for (auto & encoder: encoders)
		encoder->set_framerate(hz);
}

void compositor::update_tracking(const from_headset::tracking & tracking)
{
	foveation.update_tracking(tracking);
}

void compositor::update_foveation_center_override(const from_headset::override_foveation_center & center)
{
	foveation.update_foveation_center_override(center);
}

void compositor::resume()
{
	for (auto & encoder: encoders)
		encoder->reset();
	send_video_stream_description();
}

void compositor::on_feedback(const from_headset::feedback & feedback, const clock_offset & o)
{
	uint8_t stream = feedback.stream_index;
	if (stream >= encoders.size())
		return;
	encoders[stream]->on_feedback(feedback);
	if (not o)
		return;
	pacer.on_feedback(feedback, o);
}

} // namespace wivrn
