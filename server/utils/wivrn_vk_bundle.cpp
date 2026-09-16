/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
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
#include "wivrn_vk_bundle.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "utils/enumerate_polyfill.h"
#include "vulkan_loader.h"
#include "wivrn-server_shaders.h"
#include "wivrn_config.h"

#include <format>
#include <ranges>
#include <set>

DEBUG_GET_ONCE_NUM_OPTION(force_gpu_index, "XRT_COMPOSITOR_FORCE_GPU_INDEX", -1)

// Diagnostic override for vk_bundle::multi_layer_stream_images's own
// vendorID-based denylist (docs/ANDROID_PORT.md's Milestone 6): -1 (default)
// = the normal vendorID check; 0 = force the PowerVR-workaround single-layer
// path regardless of vendor; 1 = force the shared-3-layer fast path
// regardless of vendor. Added live while diagnosing a real stereo
// duplication/ghosting bug seen on a Snapdragon tablet's Mesa/Turnip
// (freedreno) driver -- the first non-PowerVR, non-AMD-Xclipse driver this
// fast path had ever actually run on. Forcing 0 here reproduced the exact
// same bug, ruling this path OUT as the cause (see Milestone 8's own
// entry) -- the real cause turned out to be VRChat's own login-screen
// rendering on this runtime, confirmed by a clean, symmetric stereo image
// from this project's own reference Unity app on the same server/driver.
// Left in permanently: a real, reusable diagnostic for the next time this
// fast path needs to be A/B'd against a genuinely new GPU/driver.
// `adb shell setprop debug.xrt.WIVRN_MULTI_LAYER_STREAM_IMAGES 0` (or 1).
DEBUG_GET_ONCE_NUM_OPTION(multi_layer_stream_images_override, "WIVRN_MULTI_LAYER_STREAM_IMAGES", -1)

// Default 3: left and right eye + alpha
DEBUG_GET_ONCE_NUM_OPTION(max_vulkan_encoders, "WIVRN_MAX_VULKAN_ENCODERS", 3)

// Corruption-investigation diagnostic (docs/ANDROID_PORT.md's perf branch):
// explicitly request VK_LAYER_KHRONOS_validation for THIS instance only,
// rather than Android's system-wide "enable GPU debug layers" developer
// option / adb settings (enable_gpu_debug_layers/gpu_debug_app/
// gpu_debug_layers). That mechanism does not work at all on this device/OS
// build -- confirmed live, the Vulkan loader's own debug trace never once
// searches /data/local/tmp/vulkan/debug (the documented location for it)
// regardless of those settings -- and additionally injects the layer into
// every Vulkan instance the whole process creates when it does anything,
// including the app's own Activity/View system (libhwui's Vulkan-backed UI
// renderer), which crashed before this instance was ever reached.
//
// What actually works: the layer .so is bundled directly into this
// (debug-only) APK's own native library directory via server-app/
// build.gradle's debug jniLibs source set -- one of the paths the loader
// *does* search for every app, confirmed live ("searching for layers in
// '.../lib/arm64'" in its own trace). With that in place, this explicit
// request is enough on its own; no adb settings or pushed files needed.
// `adb shell setprop debug.xrt.WIVRN_VK_VALIDATION 1` (no rebuild needed
// once the debug APK is installed). Validation output arrives through the
// existing message_callback below (severity Warning/Error already maps to
// U_LOG_WARN/ERROR, so it's visible even without raising XRT_LOG).
DEBUG_GET_ONCE_NUM_OPTION(vk_validation, "WIVRN_VK_VALIDATION", 0)

namespace
{

VkBool32 message_callback(
        vk::DebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        vk::DebugUtilsMessageTypeFlagsEXT messageTypes,
        const vk::DebugUtilsMessengerCallbackDataEXT * pCallbackData,
        void * pUserData)
{
	u_logging_level level = U_LOGGING_ERROR;
	switch (vk::DebugUtilsMessageSeverityFlagBitsEXT(messageSeverity))
	{
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
			level = U_LOGGING_DEBUG;
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
			level = U_LOGGING_INFO;
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
			level = U_LOGGING_WARN;
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
			level = U_LOGGING_ERROR;
			break;
	}
	U_LOG(level, "%s", pCallbackData->pMessage);
	return false;
}
struct strless
{
	bool operator()(const char * a, const char * b) const
	{
		return strcmp(a, b) < 0;
	}
};

int device_type_priority(vk::PhysicalDeviceType device_type)
{
	switch (device_type)
	{
		case vk::PhysicalDeviceType::eDiscreteGpu:
			return 4;
		case vk::PhysicalDeviceType::eIntegratedGpu:
			return 3;
		case vk::PhysicalDeviceType::eVirtualGpu:
			return 2;
		case vk::PhysicalDeviceType::eCpu:
			return 1;
		case vk::PhysicalDeviceType::eOther:
			return 0;
	}
	assert(false);
	return 0;
}

uint32_t select_queue(const std::vector<vk::QueueFamilyProperties> & queues, vk::QueueFlags flags)
{
	uint32_t res = vk::QueueFamilyIgnored;
	auto queue_flag_cost = [](vk::QueueFlags flags) {
		// number of bits set
		return std::popcount(VkQueueFlags(flags));
	};
	for (auto [i, prop]: std::ranges::enumerate_view(queues))
	{
		if ((prop.queueFlags & flags) == flags)
		{
			if (res == vk::QueueFamilyIgnored or
			    queue_flag_cost(prop.queueFlags) < queue_flag_cost(queues[res].queueFlags))
				res = i;
		}
	}
	return res;
}

int get_queue_index(const std::vector<vk::QueueFamilyProperties> & queues, std::vector<vk::DeviceQueueCreateInfo> & infos, uint32_t family_index)
{
	if (family_index == vk::QueueFamilyIgnored)
		return -1;
	for (auto & info: infos)
	{
		if (info.queueFamilyIndex == family_index)
		{
			auto count = queues.at(family_index).queueCount;
			if (info.queueCount == count)
			{
				U_LOG_D("Insufficient vulkan queues for family %d", family_index);
				return -1;
			}
			return info.queueCount++;
		}
	}
	// worst case: compute, transfer and 3x encode are all of the same family
	static std::array<float, 5> prios{1.0, 1.0, 1.0, 1.0, 1.0};
	infos.push_back(vk::DeviceQueueCreateInfo{
	        .queueFamilyIndex = family_index,
	        .queueCount = 1,
	        .pQueuePriorities = prios.data(),
	});
	return 0;
}
} // namespace

wivrn::vk_bundle::vk_bundle() :
#if VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL
        // Desktop: unchanged from before this existed, vulkan-hpp's own
        // internal DynamicLoader (dlopen's the system libvulkan.so itself).
        vk_ctx(),
#else
        // Android: resolve vkGetInstanceProcAddr ourselves, so it can be
        // redirected to a custom driver (Turnip/adrenotools) instead of the
        // system one -- see vulkan_loader.cpp's own comment. Falls back to
        // the exact same system-libvulkan.so behavior as the desktop path
        // above when no custom driver is configured.
        vk_ctx(resolve_vk_get_instance_proc_addr()),
#endif
        instance(nullptr),
        physical_device(nullptr),
        device(nullptr),
        debug(nullptr)
{
	// Create instance
	vk::ApplicationInfo app_info{
	        .pApplicationName = "WiVRn server",
	        .pEngineName = "WiVRn",
	        .apiVersion = api_version,
	};
	{
		// Required extensions
		instance_extensions = {
		        // promoted to 1.1, but we need the KHR name for Monado
		        VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
		        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
		        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
		        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		};
		// Optional extensions
		std::set<const char *, strless> opt_instance_extensions = {
		        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
		};
		for (auto & ext: vk_ctx.enumerateInstanceExtensionProperties())
		{
			if (auto it = opt_instance_extensions.find(ext.extensionName); it != opt_instance_extensions.end())
				instance_extensions.push_back(*it);
		}

		std::vector<const char *> instance_layers;
		if (debug_get_num_option_vk_validation())
		{
			bool available = false;
			for (auto & layer: vk_ctx.enumerateInstanceLayerProperties())
				if (std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation")
					available = true;
			if (available)
				instance_layers.push_back("VK_LAYER_KHRONOS_validation");
			else
				U_LOG_W("WIVRN_VK_VALIDATION set but VK_LAYER_KHRONOS_validation is not available");
		}

		// Always try this one (not gated behind a debug option like
		// validation above): Khronos' own portable synchronization2
		// implementation, bundled on Android alongside adrenotools (see
		// server-app/build.gradle). On a driver that already has native
		// synchronization2 (Pixel, S22) the layer is a no-op passthrough
		// by default (it only actively emulates when the driver lacks the
		// extension, or when VK_SYNCHRONIZATION2_FORCE_ENABLE is set,
		// which we don't set) -- safe to always request. See
		// docs/ANDROID_PORT.md's Milestone 10 follow-up for why this
		// matters on the Adreno tablet specifically.
		{
			bool available = false;
			for (auto & layer: vk_ctx.enumerateInstanceLayerProperties())
				if (std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_synchronization2")
					available = true;
			if (available)
				instance_layers.push_back("VK_LAYER_KHRONOS_synchronization2");
		}

		instance = vk::raii::Instance(
		        vk_ctx,
		        vk::InstanceCreateInfo{
		                .pApplicationInfo = &app_info,
		                .enabledLayerCount = uint32_t(instance_layers.size()),
		                .ppEnabledLayerNames = instance_layers.data(),
		                .enabledExtensionCount = uint32_t(instance_extensions.size()),
		                .ppEnabledExtensionNames = instance_extensions.data(),
		        });
	}

	if (has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
		debug = vk::raii::DebugUtilsMessengerEXT(
		        instance,
		        vk::DebugUtilsMessengerCreateInfoEXT{
		                .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
		                .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
		                .pfnUserCallback = message_callback,
		        });

	// Select physical device

	{
		auto phys_devices = instance.enumeratePhysicalDevices();
		if (phys_devices.empty())
			throw std::runtime_error("No Vulkan device");
		auto index = debug_get_num_option_force_gpu_index();
		if (index < 0)
		{
			std::vector<vk::PhysicalDeviceType> types;
			for (const auto & dev: phys_devices)
				types.push_back(dev.getProperties().deviceType);
			index = 0;
			// Select the first device of the highest priority
			for (auto [i, t]: std::ranges::enumerate_view(types))
			{
				if (device_type_priority(t) > device_type_priority(types[index]))
					index = i;
			}
		}
		else if (index > phys_devices.size())
			throw std::runtime_error(std::format("Invalid GPU index {}, must be in range 0..{}", index, phys_devices.size() - 1));
		physical_device = std::move(phys_devices[index]);
	}

	// Select queue families
	auto queues = physical_device.getQueueFamilyProperties();
	uint32_t encode_queue_family_index;
	{
		queue.family_index = select_queue(queues, vk::QueueFlagBits::eCompute);
		transfer_queue.family_index = select_queue(queues, vk::QueueFlagBits::eTransfer);
#if WIVRN_USE_VULKAN_ENCODE
		encode_queue_family_index = select_queue(queues, vk::QueueFlagBits::eVideoEncodeKHR);
#endif
		// Technically allowed to have a device with only encode or decode capabilities
		if (queue.family_index == vk::QueueFamilyIgnored)
			throw std::runtime_error("GPU does not support vulkan compute");
	}

	// Create logical device
	{
		// Required extensions
		device_extensions = {
		        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
		        VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
		        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
		        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
		        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		};
		// Optional extensions
		std::set<const char *, strless> opt_device_extensions = {
		        // For Monado
		        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
		        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
		        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
		        VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
		        VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
		        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
		        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
		        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
// For Monado's Android swapchain-import path (vk_create_image_from_native,
// XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER on Android): a local OpenXR
// app importing its own AHardwareBuffer-backed swapchain images into the
// compositor needs this to resolve the external-memory handle type; without
// it, vk_create_image_from_native crashed (null function pointer, offset
// +1180) the moment a real local app tried to create one -- nothing before
// broker registration ever exercised this path, so WiVRn's own device
// extension list never needed it. Also the mechanism a genuinely zero-copy
// video_encoder_mediacodec.cpp would use (see its own comment) -- not a
// coincidence, same underlying AHardwareBuffer<->Vulkan interop either way.
#ifdef VK_ANDROID_external_memory_android_hardware_buffer
		        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
		        // Required by the spec whenever
		        // VK_ANDROID_external_memory_android_hardware_buffer is
		        // enabled (queue family ownership transfers to/from a
		        // foreign entity, e.g. the hardware encoder/decoder, for
		        // an AHB-imported image) -- was missing here, caught live
		        // by Vulkan validation (vkCreateDevice
		        // VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-01387).
		        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
#endif
// For FFMPEG
#ifdef VK_EXT_external_memory_dma_buf
		        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
#endif
#ifdef VK_EXT_image_drm_format_modifier
		        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
#endif

// For vulkan video encode
#ifdef VK_KHR_video_queue
		        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
#endif
#ifdef VK_KHR_video_encode_queue
		        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
#endif
#ifdef VK_KHR_video_maintenance1
		        VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
#endif
#ifdef VK_KHR_video_encode_h264
		        VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
#endif
#ifdef VK_KHR_video_encode_h265
		        VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME,
#endif
#ifdef VK_KHR_video_encode_intra_refresh
		        VK_KHR_VIDEO_ENCODE_INTRA_REFRESH_EXTENSION_NAME,
#endif
#ifdef VK_KHR_maintenance9
		        VK_KHR_MAINTENANCE_9_EXTENSION_NAME,
#endif
#ifdef VK_KHR_unified_image_layouts
		        VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME,
#endif
// Milestone 5 (docs/ANDROID_PORT.md's perf branch): host_image_copy,
// enabled below, lets video_encoder_mediacodec.cpp read back the
// compositor's image without a queue submission at all.
#ifdef VK_EXT_host_image_copy
		        VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME,
#endif
// For perfetto GPU timestamp tracing
#ifdef VK_EXT_calibrated_timestamps
		        VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
#endif
		};
		for (auto & ext: physical_device.enumerateDeviceExtensionProperties())
		{
			if (auto it = opt_device_extensions.find(ext.extensionName); it != opt_device_extensions.end())
				device_extensions.push_back(*it);
		}


		float prio = 1.0;

		std::vector<vk::DeviceQueueCreateInfo> queues_info;
		int queue_index = get_queue_index(queues, queues_info, queue.family_index);
		U_LOG_D("queue index: %d", queue_index);
		int transfer_queue_index = get_queue_index(queues, queues_info, transfer_queue.family_index);
		U_LOG_D("transfer queue index: %d", transfer_queue_index);
#if WIVRN_USE_VULKAN_ENCODE
		std::vector<int> encode_queue_indices;
		for (int i = 0; i < debug_get_num_option_max_vulkan_encoders(); ++i)
		{
			int encode_queue_index = get_queue_index(queues, queues_info, encode_queue_family_index);
			if (encode_queue_index < 0)
				break;
			U_LOG_D("encode queue index: %d", encode_queue_index);
			encode_queue_indices.push_back(encode_queue_index);
		}
#ifdef VK_KHR_video_maintenance1
		if (has_device_ext(VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME))
			std::get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>(feat).videoMaintenance1 =
			        std::get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>(physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>()).videoMaintenance1;
#endif
#endif
#ifdef VK_KHR_maintenance9
		if (has_device_ext(VK_KHR_MAINTENANCE_9_EXTENSION_NAME))
		{
			std::get<vk::PhysicalDeviceMaintenance9FeaturesKHR>(feat).maintenance9 =
			        std::get<vk::PhysicalDeviceMaintenance9FeaturesKHR>(physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceMaintenance9FeaturesKHR>()).maintenance9;
		}
#endif

		// Enable features
		auto [phys_feat, phys_feat12, phys_feat13] = physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>();

		std::get<vk::PhysicalDeviceVulkan12Features>(feat).descriptorBindingPartiallyBound = phys_feat12.descriptorBindingPartiallyBound;
		std::get<vk::PhysicalDeviceVulkan12Features>(feat).timelineSemaphore = phys_feat12.timelineSemaphore;

		bool synchronization2 = phys_feat13.synchronization2;
		std::get<vk::PhysicalDeviceVulkan13Features>(feat).synchronization2 = synchronization2;

#ifdef VK_KHR_synchronization2
		// A device below apiVersion 1.3 (like this Adreno tablet's stock
		// driver, which reports 1.1) never populates
		// PhysicalDeviceVulkan13Features at all -- if it (or a layer such
		// as VK_LAYER_KHRONOS_synchronization2, see above) only offers the
		// discrete VK_KHR_synchronization2 extension, it has to be queried
		// through its own struct instead. Queried separately, not chained
		// together with PhysicalDeviceVulkan13Features above: having both
		// describe the same feature in one pNext chain is invalid
		// (VUID-VkPhysicalDeviceFeatures2-pNext-06532 and its
		// vkCreateDevice equivalent).
		if (not synchronization2)
		{
			synchronization2 = std::get<vk::PhysicalDeviceSynchronization2FeaturesKHR>(physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceSynchronization2FeaturesKHR>()).synchronization2;
			std::get<vk::PhysicalDeviceSynchronization2FeaturesKHR>(feat).synchronization2 = synchronization2;
			// Only one of the two structs may be present in the
			// vkCreateDevice pNext chain -- keep whichever we actually
			// need linked (Vulkan13Features carries nothing else this
			// file reads, see wivrn_vk_bundle.h's comment).
			feat.unlink<vk::PhysicalDeviceVulkan13Features>();
		}
		else
		{
			feat.unlink<vk::PhysicalDeviceSynchronization2FeaturesKHR>();
		}
#endif

		if (not synchronization2)
			throw std::runtime_error("GPU does not support Vulkan synchronization2 feature");
		if (not phys_feat12.timelineSemaphore)
			throw std::runtime_error("GPU does not support Vulkan timeline semaphores");

#ifdef VK_KHR_video_encode_intra_refresh
		if (has_device_ext(VK_KHR_VIDEO_ENCODE_INTRA_REFRESH_EXTENSION_NAME))
			std::get<vk::PhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR>(feat).videoEncodeIntraRefresh = std::get<vk::PhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR>(physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR>()).videoEncodeIntraRefresh;
#endif
#ifdef VK_KHR_unified_image_layouts
		if (has_device_ext(VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME))
		{
			const auto available = std::get<vk::PhysicalDeviceUnifiedImageLayoutsFeaturesKHR>(physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceUnifiedImageLayoutsFeaturesKHR>());
			auto & enabled = std::get<vk::PhysicalDeviceUnifiedImageLayoutsFeaturesKHR>(feat);
			enabled.unifiedImageLayouts = available.unifiedImageLayouts;
			enabled.unifiedImageLayoutsVideo = available.unifiedImageLayoutsVideo;
			U_LOG_D("GPU unified layout support: %d (video: %d)", enabled.unifiedImageLayouts, enabled.unifiedImageLayoutsVideo);
		}
#endif
#ifdef VK_EXT_host_image_copy
		if (has_device_ext(VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME))
		{
			const auto available = std::get<vk::PhysicalDeviceHostImageCopyFeaturesEXT>(physical_device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceHostImageCopyFeaturesEXT>());
			std::get<vk::PhysicalDeviceHostImageCopyFeaturesEXT>(feat).hostImageCopy = available.hostImageCopy;
			host_image_copy = bool(available.hostImageCopy);
			U_LOG_D("GPU host image copy support: %d", host_image_copy);
		}
#endif

		device = vk::raii::Device(
		        physical_device,
		        vk::DeviceCreateInfo{
		                .pNext = &feat.get(),
		                .queueCreateInfoCount = uint32_t(queues_info.size()),
		                .pQueueCreateInfos = queues_info.data(),
		                .enabledExtensionCount = uint32_t(device_extensions.size()),
		                .ppEnabledExtensionNames = device_extensions.data(),
		        });

		queue.queue = device.getQueue(queue.family_index, queue_index);
		name(queue.queue, "compute queue");
		if (transfer_queue_index >= 0)
		{
			transfer_queue.queue = device.getQueue(transfer_queue.family_index, transfer_queue_index);
			name(transfer_queue.queue, "transfer queue");
		}
#if WIVRN_USE_VULKAN_ENCODE
		for (auto encode_queue_index: encode_queue_indices)
		{
			auto & queue = encode_queues.emplace_back();
			queue.queue = device.getQueue(encode_queue_family_index, encode_queue_index);
			queue.family_index = encode_queue_family_index;
			name(queue.queue, "encode queue");
		}
		U_LOG_D("Using %d vulkan encode queue(s)", int(encode_queues.size()));
#endif
	}

	allocator.emplace(VmaAllocatorCreateInfo{
	                          .physicalDevice = *physical_device,
	                          .device = *device,
	                          .instance = *instance,
	                          .vulkanApiVersion = app_info.apiVersion,
	                  },
	                  *debug != VK_NULL_HANDLE);

	auto prop = physical_device.getProperties();

	// PowerVR/Imagination Technologies' registered Vulkan vendorID -- see
	// multi_layer_stream_images's own comment (wivrn_vk_bundle.h).
	constexpr uint32_t vendor_id_powervr = 0x1010;
	multi_layer_stream_images = (prop.vendorID != vendor_id_powervr);

	if (auto override = debug_get_num_option_multi_layer_stream_images_override(); override >= 0)
		multi_layer_stream_images = (override != 0);

	U_LOG_I("Vulkan instance created:\n"
	        "\tGPU: %s\n"
	        "\tqueue families: %d %d %d (main, encode, transfer)\n"
	        "\tmulti_layer_stream_images: %s\n",
	        prop.deviceName.data(),
	        int32_t(queue.family_index),
	        int32_t(encode_queue_family_index),
	        int32_t(transfer_queue.family_index),
	        multi_layer_stream_images ? "true" : "false");

}

uint32_t wivrn::vk_bundle::get_memory_type(uint32_t type_bits, vk::MemoryPropertyFlags memory_props)
{
	auto mem_prop = physical_device.getMemoryProperties();

	for (uint32_t i = 0; i < mem_prop.memoryTypeCount; ++i)
	{
		if ((type_bits >> i) & 1)
		{
			if ((mem_prop.memoryTypes[i].propertyFlags & memory_props) ==
			    memory_props)
				return i;
		}
	}
	throw std::runtime_error("Failed to get memory type");
}

bool wivrn::vk_bundle::has_instance_ext(const char * ext) const
{
	for (auto e: instance_extensions)
	{
		if (strcmp(e, ext) == 0)
			return true;
	}
	return false;
}

bool wivrn::vk_bundle::has_device_ext(const char * ext) const
{
	for (auto e: device_extensions)
	{
		if (strcmp(e, ext) == 0)
			return true;
	}
	return false;
}

bool wivrn::vk_bundle::optimal_transfer(uint32_t from, uint32_t to) const
{
	if (from == vk::QueueFamilyIgnored or to == vk::QueueFamilyIgnored or from == to)
		return false;
#ifdef VK_KHR_maintenance9
	if (std::get<vk::PhysicalDeviceMaintenance9FeaturesKHR>(feat).maintenance9)
	{
		auto props = physical_device.getQueueFamilyProperties2<vk::StructureChain<vk::QueueFamilyProperties2, vk::QueueFamilyOwnershipTransferPropertiesKHR>>();

		auto mask = std::get<1>(props[from]).optimalImageTransferToQueueFamilies;
		return (mask & (1u << to)) != 0;
	}
#endif
	return true;
}

vk::raii::ShaderModule wivrn::vk_bundle::load_shader(const char * name)
{
	std::span spirv{::shaders.at(name)};

	return vk::raii::ShaderModule{
	        device,
	        vk::ShaderModuleCreateInfo{
	                .codeSize = spirv.size_bytes(),
	                .pCode = spirv.data(),
	        },
	};
}

void wivrn::vk_bundle::name(vk::ObjectType type, uint64_t handle, const char * value)
{
	if (not has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
		return;
	device.setDebugUtilsObjectNameEXT(vk::DebugUtilsObjectNameInfoEXT{
	        .objectType = type,
	        .objectHandle = handle,
	        .pObjectName = value,
	});
}
