/*
 * WiVRn VR streaming
 * Copyright (C) 2024  galister <galister@librevr.org>
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

#include "foveation.h"

#include "driver/xrt_cast.h"
#include "utils/enumerate_polyfill.h"
#include "utils/wivrn_vk_bundle.h"
#include "vk/specialization_constants.h"
#include "wivrn_packets.h"

#include "xrt/xrt_defines.h"
#include "xrt/xrt_limits.h"
#include "util/u_debug.h"
#include "util/u_logging.h"

// Diagnostic: forces the foveation center to dead-ahead for both eyes
// (compression math untouched -- "no compression" outright crashes fill_ubo()'s
// count>0 assert), to isolate a gaze-dependent per-eye asymmetry.
// `adb shell setprop debug.xrt.WIVRN_DISABLE_FOVEATION 1` (or the env var on desktop).
DEBUG_GET_ONCE_NUM_OPTION(disable_foveation, "WIVRN_DISABLE_FOVEATION", 0)

// See its own call site's comment, below.
DEBUG_GET_ONCE_NUM_OPTION(log_foveation_ubo, "WIVRN_LOG_FOVEATION_UBO", 0)

#include <array>
#include <cmath>
#include <format>
#include <ranges>
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vulkan_structs.hpp>
#include <openxr/openxr.h>

#define RENDER_FOVEATION_BUFFER_DIMENSIONS (4096 + 1)

namespace
{
struct ubo_data
{
	uint32_t x[XRT_MAX_VIEWS * RENDER_FOVEATION_BUFFER_DIMENSIONS];
	uint32_t y[XRT_MAX_VIEWS * RENDER_FOVEATION_BUFFER_DIMENSIONS];
};

vk::raii::Sampler make_sampler(wivrn::vk_bundle & vk)
{
	vk::raii::Sampler res(
	        vk.device,
	        vk::SamplerCreateInfo{
	                .magFilter = vk::Filter::eLinear,
	                .minFilter = vk::Filter::eLinear,
	                .mipmapMode = vk::SamplerMipmapMode::eLinear,
	                .addressModeU = vk::SamplerAddressMode::eClampToBorder,
	                .addressModeV = vk::SamplerAddressMode::eClampToBorder,
	                .addressModeW = vk::SamplerAddressMode::eClampToBorder,
	                .borderColor = vk::BorderColor::eFloatOpaqueBlack,
	        });
	vk.name(res, "foveation sampler");
	return res;
}

vk::raii::DescriptorSetLayout make_ds_layout(wivrn::vk_bundle & vk)
{
	std::array bindings{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 2,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eStorageBuffer,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 2,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 3,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 4,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 5,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	};
	vk::raii::DescriptorSetLayout res{
	        vk.device,
	        vk::DescriptorSetLayoutCreateInfo{
	                .bindingCount = bindings.size(),
	                .pBindings = bindings.data(),
	        },
	};
	vk.name(*res, "foveation descriptor set layout");
	return res;
}

vk::raii::PipelineLayout make_layout(wivrn::vk_bundle & vk, vk::DescriptorSetLayout ds_layout)
{
	// Which eye's ubo/source-array slot this dispatch targets -- see foveation.comp.
	vk::PushConstantRange push_constant{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .offset = 0,
	        .size = sizeof(int32_t),
	};
	vk::raii::PipelineLayout res(vk.device,
	                             vk::PipelineLayoutCreateInfo{
	                                     .setLayoutCount = 1,
	                                     .pSetLayouts = &ds_layout,
	                                     .pushConstantRangeCount = 1,
	                                     .pPushConstantRanges = &push_constant,
	                             });
	vk.name(*res, "foveation pipeline layout");
	return res;
}

std::array<vk::raii::Pipeline, 2> make_pipelines(wivrn::vk_bundle & vk, vk::PipelineLayout layout, int32_t alpha_width)
{
	auto shader = vk.load_shader("foveation");
	auto spc = make_specialization_constants(alpha_width);
	std::array res{
	        vk::raii::Pipeline{
	                vk.device,
	                nullptr,
	                vk::ComputePipelineCreateInfo{
	                        .stage = {
	                                .stage = vk::ShaderStageFlagBits::eCompute,
	                                .module = *shader,
	                                .pName = "main",
	                        },
	                        .layout = layout,
	                },
	        },
	        vk::raii::Pipeline{
	                vk.device,
	                nullptr,
	                vk::ComputePipelineCreateInfo{
	                        .stage = {
	                                .stage = vk::ShaderStageFlagBits::eCompute,
	                                .module = *shader,
	                                .pName = "main",
	                                .pSpecializationInfo = spc,
	                        },
	                        .layout = layout,
	                },
	        },
	};
	vk.name(*res[0], "foveation pipeline");
	vk.name(*res[1], "foveation+alpha pipeline");
	return res;
}

vk::raii::DescriptorPool make_ds_pool(wivrn::vk_bundle & vk)
{
	// One descriptor set per eye: a set's contents can't be safely rewritten
	// between two dispatches already recorded into the same not-yet-submitted
	// command buffer (the GPU reads current contents at execution time).
	std::array pool_sizes{
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eCombinedImageSampler,
	                .descriptorCount = 2 * 2,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 4 * 2,
	        },
	        vk::DescriptorPoolSize{
	                .type = vk::DescriptorType::eStorageBuffer,
	                .descriptorCount = 1 * 2,
	        },
	};
	vk::raii::DescriptorPool res{
	        vk.device,
	        vk::DescriptorPoolCreateInfo{
	                .maxSets = 5,
	                .poolSizeCount = pool_sizes.size(),
	                .pPoolSizes = pool_sizes.data(),
	        },
	};
	vk.name(*res, "foveation descriptor pool");
	return res;
}
uint32_t divide_and_round_up(uint32_t a, uint32_t b)
{
	return (a + (b - 1)) / b;
}

} // namespace

// a, b: parameters computed by solve_foveation
// λ: pixel ratio between full size and foveated image (in range ]0,1[)
// c: coordinates in -1,1 range of the full size image where pixel ratio must be 1:1
// x: coordinates in -1,1 range of the foveated image
// result: full size image coordinates in -1,1 range
static double defoveate(double a, double b, double λ, double c, double x)
{
	// In order to save encoding, transmit and decoding time, only a portion of the image is encoded in full resolution.
	// on each axis, foveated coordinates are defined by the following formula.
	return λ / a * tan(a * x + b) + c;
	// a and b are defined such as:
	// edges of the image are not moved
	// f(-1) = -1
	// f( 1) =  1
	// the function also enforces pixel ratio 1:1 at fovea
	// df⁻¹(x)/dx = 1/scale for x = c

	// We then ensure that source and destination pixel grids match by
	// rounding to integer pixel ratios: 1:1, 1:2 etc.
	// Finally, pixel spans are sorted so that we only have increasing
	// ratios going out from the center.
}

static std::tuple<float, float> solve_foveation(float λ, float c)
{
	// Compute a and b for the foveation function such that:
	//   foveate(a, b, scale, c, -1) = -1   (eq. 1)
	//   foveate(a, b, scale, c,  1) =  1   (eq. 2)
	//
	// Use eq. 2 to express a as function of b, then replace in eq. 1
	// equation that needs to be null is:
	auto b = [λ, c](double a) { return atan(a * (1 - c) / λ) - a; };
	auto eq = [λ, c](double a) { return atan(a * (1 - c) / λ) + atan(a * (1 + c) / λ) - 2 * a; }; // (eq. 3)

	// function starts positive, reaches a maximum then decreases to -∞
	double a0 = 0;
	// Find a negative value by computing eq(2^n)
	double a1 = 1;
	while (eq(a1) > 0)
		a1 *= 2;

	// last computed values for f(a0) and f(a1)
	std::optional<double> f_a0;
	double f_a1 = eq(a1);

	int n = 0;
	double a = 0;
	while (std::abs(a1 - a0) > 0.0000001 && n++ < 100)
	{
		if (not f_a0)
		{
			// use binary search
			a = 0.5 * (a0 + a1);
			double val = eq(a);
			if (val > 0)
			{
				a0 = a;
				f_a0 = val;
			}
			else
			{
				a1 = a;
				f_a1 = val;
			}
		}
		else
		{
			// f(a1) is always defined
			// when f(a0) is defined, use secant method
			a = a1 - f_a1 * (a1 - a0) / (f_a1 - *f_a0);
			a0 = a1;
			a1 = a;
			f_a0 = f_a1;
			f_a1 = eq(a);
		}
	}

	return {a, b(a)};
}

static bool is_zero_quat(xrt_quat q)
{
	return q.x == 0 and q.y == 0 and q.z == 0 and q.w == 0;
}

static xrt_vec2 yaw_pitch(xrt_quat q)
{
	if (is_zero_quat(q))
		return xrt_vec2{};

	float sine_theta = std::clamp(-2.0f * (q.y * q.z - q.w * q.x), -1.0f, 1.0f);

	float pitch = std::asin(sine_theta);

	if (std::abs(sine_theta) > 0.99999f)
	{
		float scale = std::copysign(2.0, sine_theta);
		return {scale * std::atan2(-q.z, q.w), pitch};
	}

	return {
	        std::atan2(2.0f * (q.x * q.z + q.w * q.y),
	                   q.w * q.w - q.x * q.x - q.y * q.y + q.z * q.z),
	        pitch};
}

static float angles_to_center(float e, float l, float r)
{
	e = tan(e);
	l = tan(l);
	r = tan(r);
	float res = std::clamp((e - l) / (r - l) * 2 - 1, -1.f, 1.f);
	// If the center isn't in the FoV, fallback to middle of image
	if (std::isnan(res))
		return 0;
	return res;
}

static float convergence_angle(float distance, float eye_x, float gaze_yaw)
{
	float target_x = distance * std::sin(gaze_yaw);
	float target_z = distance * std::cos(gaze_yaw);

	float dx = target_x - eye_x;
	float dz = target_z;

	return std::atan2(dx, dz);
}

static void fill_param_2d(
        float c,
        size_t foveated_dim,
        size_t source_dim,
        std::vector<uint16_t> & out)
{
	float scale = float(foveated_dim) / source_dim;
	auto [a, b] = solve_foveation(scale, c);

	uint16_t last = 0;
	std::vector<uint16_t> left; // index 0: 1:1 ratio, then 2:1 etc.
	std::vector<uint16_t> right;
	for (size_t i = 1; i < foveated_dim; ++i)
	{
		double u = (i * 2.) / foveated_dim - 1;
		auto f = defoveate(a, b, scale, c, u);
		uint16_t n = std::clamp<uint16_t>((f * 0.5 + 0.5) * source_dim + 0.5, 0, source_dim);
		assert(n > last);
		size_t count = n - last;
		auto & vec = u < c ? left : right;
		if (count > vec.size())
			vec.resize(count);
		vec[count - 1]++;
		last = n;
	}
	assert(last < source_dim);
	size_t count = source_dim - last;
	if (count > right.size())
		right.resize(count);
	right[count - 1]++;

	count = std::max(left.size(), right.size());
	out.clear();
	out.resize(count - left.size());
	out.insert(out.end(), left.rbegin(), left.rend());
	if (not right.empty())
		out.back() += right.front();
	if (right.size() > 1)
		out.insert(out.end(), right.begin() + 1, right.end());
	out.resize(count * 2 - 1);
}

namespace wivrn
{

void foveation::compute_params()
{
	auto e = yaw_pitch(gaze);

	if (manual_foveation.enabled)
		e.y = manual_foveation.pitch;

	for (size_t i = 0; i < 2; ++i)
	{
		const auto & fov = last.fovs[i];

		bool neutral = debug_get_num_option_disable_foveation();

		size_t extent_w = std::abs(last.src[i].extent.w);
		if (foveated_size.width < extent_w)
		{
			auto distance = manual_foveation.enabled ? manual_foveation.distance : convergence_distance;
			auto angle_x = neutral ? 0.0 : convergence_angle(distance, eye_x[i], -e.x);
			auto center = angles_to_center(angle_x, fov.angle_left, fov.angle_right);
			fill_param_2d(center, foveated_size.width, extent_w, params[i].x);
		}
		else
			params[i].x = {uint16_t(extent_w)};

		size_t extent_h = std::abs(last.src[i].extent.h);
		if (foveated_size.height < extent_h)
		{
			auto angle_y = neutral ? 0.0 : -e.y;
			if (not neutral and is_zero_quat(gaze) and not manual_foveation.enabled)
			{
				// Natural gaze is not straight forward, adjust the angle
				angle_y += angle_offset;
			}
			auto center = angles_to_center(-angle_y, fov.angle_up, fov.angle_down);
			fill_param_2d(center, foveated_size.height, extent_h, params[i].y);
		}
		else
			params[i].y = {uint16_t(extent_h)};
	}
}

foveation::foveation(wivrn::vk_bundle & bundle, vk::Extent3D foveated_size) :
        foveated_size(foveated_size),
        // normal sight line is between 10° and 15° below horizontal
        // https://apps.dtic.mil/sti/tr/pdf/AD0758339.pdf pages 393-394
        // testing shows 10° looks better
        angle_offset(10 * M_PI / 180),
        convergence_distance(1 /* meter*/),
        gpu_buffer(
                bundle.device,
                {
                        .size = sizeof(ubo_data),
                        .usage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
                },
                VmaAllocationCreateInfo{
                        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                },
                "foveation storage buffer"),
        sampler(make_sampler(bundle)),
        ds_layout(make_ds_layout(bundle)),
        layout(make_layout(bundle, ds_layout)),
        pipeline(make_pipelines(bundle, layout, foveated_size.width / 2)),
        descriptor_pool(make_ds_pool(bundle))
{
	std::array<vk::DescriptorSetLayout, 2> layouts{*ds_layout, *ds_layout};
	auto sets = bundle.device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
	        .descriptorPool = descriptor_pool,
	        .descriptorSetCount = uint32_t(layouts.size()),
	        .pSetLayouts = layouts.data(),
	});
	for (size_t i = 0; i < descriptor_sets.size(); ++i)
	{
		descriptor_sets[i] = sets[i].release();
		bundle.name(descriptor_sets[i], std::format("foveation descriptor set {}", i));
	}
}

void foveation::update_tracking(const from_headset::tracking & tracking)
{
	std::lock_guard lock(mutex);

	const uint8_t orientation_ok = from_headset::pose_flags::orientation_valid | from_headset::pose_flags::orientation_tracked;

	if (tracking.view_flags & XR_VIEW_STATE_POSITION_VALID_BIT)
	{
		eye_x[0] = tracking.views[0].pose.position.x;
		eye_x[1] = tracking.views[1].pose.position.x;
	}

	for (const auto & pose: tracking.device_poses)
	{
		if (pose.device != device_id::EYE_GAZE)
			continue;

		if ((pose.flags & orientation_ok) != orientation_ok)
			return;

		gaze = xrt_cast(pose.pose.orientation);
		return;
	}
}

void foveation::update_foveation_center_override(const from_headset::override_foveation_center & center)
{
	std::lock_guard lock(mutex);
	manual_foveation = center;
}

static void fill_ubo(
        std::span<uint32_t> ubo,
        const std::vector<uint16_t> & params,
        bool flip,
        size_t offset,
        size_t size,
        [[maybe_unused]] int count)
{
	assert(params.size() % 2 == 1);
	const int n_ratio = (params.size() - 1) / 2;
	ubo[0] = offset;
	if (flip)
		ubo[0] += size;
	for (auto [i, n]: std::ranges::enumerate_view(params))
	{
		const int n_source = std::abs(n_ratio - int(i)) + 1;
		for (size_t j = 0; j < n; ++j)
		{
			assert(count > 0);
			if (flip)
				ubo[1] = ubo[0] - n_source;
			else
				ubo[1] = ubo[0] + n_source;
			ubo = ubo.subspan(1);
			--count;
		}
	}
	if (not ubo.empty())
		std::ranges::fill(ubo, ubo[0]);
}

template <typename T>
static bool operator==(const T & a, const T & b)
{
	static_assert(std::has_unique_object_representations_v<T>);
	return std::memcmp(&a, &b, sizeof(T)) == 0;
}
static bool operator==(const xrt_quat & a, const xrt_quat & b)
{
	return a.x == b.x and a.y == b.y and a.z == b.z and a.w == b.w;
}
static bool operator==(const xrt_fov & a, const xrt_fov & b)
{
	return a.angle_left == b.angle_left and a.angle_right == b.angle_right and a.angle_up == b.angle_up and a.angle_down == b.angle_down;
}
void foveation::update_ubo(
        vk::raii::CommandBuffer & cmd,
        bool flip_y,
        std::array<xrt_rect, 2> src_rect,
        std::array<xrt_fov, 2> src_fov)
{
	// Check if the last value is still valid
	std::lock_guard lock(mutex);
	if (last.flip_y == flip_y and
	    last.src[0] == src_rect[0] and
	    last.src[1] == src_rect[1] and
	    last.fovs[0] == src_fov[0] and
	    last.fovs[1] == src_fov[1] and
	    (last.gaze == gaze or manual_foveation.enabled) and // Ignore the gaze if foveation center is overridden
	    std::abs(last.eye_x[0] - eye_x[0]) < 0.0005 and
	    std::abs(last.eye_x[1] - eye_x[1]) < 0.0005 and
	    last.manual_foveation.enabled == manual_foveation.enabled and
	    std::abs(last.manual_foveation.pitch - manual_foveation.pitch) < 0.0005 and
	    std::abs(last.manual_foveation.distance - manual_foveation.distance) < 0.0005)
		return;

	last = {
	        .gaze = gaze,
	        .flip_y = flip_y,
	        .src = {src_rect[0], src_rect[1]},
	        .fovs = {src_fov[0], src_fov[1]},
	        .eye_x = {eye_x[0], eye_x[1]},
	        .manual_foveation = manual_foveation,
	};

	compute_params();

	ubo_data ubo;
	for (size_t view = 0; view < 2; ++view)
	{
		bool flip = false;
		size_t offset, extent;

		if (src_rect[view].extent.w < 0)
		{
			flip = true;
			offset = src_rect[view].offset.w + src_rect[view].extent.w;
			extent = -src_rect[view].extent.w;
		}
		else
		{
			offset = src_rect[view].offset.w;
			extent = src_rect[view].extent.w;
		}
		fill_ubo(std::span(ubo.x + view * RENDER_FOVEATION_BUFFER_DIMENSIONS, RENDER_FOVEATION_BUFFER_DIMENSIONS),
		         params[view].x,
		         flip,
		         offset,
		         extent,
		         foveated_size.width);

		if (src_rect[view].extent.h < 0)
		{
			flip = not flip_y;
			offset = src_rect[view].offset.h + src_rect[view].extent.h;
			extent = -src_rect[view].extent.h;
		}
		else
		{
			flip = flip_y;
			offset = src_rect[view].offset.h;
			extent = src_rect[view].extent.h;
		}
		fill_ubo(std::span(ubo.y + view * RENDER_FOVEATION_BUFFER_DIMENSIONS, RENDER_FOVEATION_BUFFER_DIMENSIONS),
		         params[view].y,
		         flip,
		         offset,
		         extent,
		         foveated_size.height);
	}
	// Diagnostic: logs per-eye source rect and the UBO's first/last x-index
	// table entries, to catch an unsigned underflow at the edge buckets.
	// `adb shell setprop debug.xrt.WIVRN_LOG_FOVEATION_UBO 1`.
	if (debug_get_num_option_log_foveation_ubo())
	{
	for (size_t view = 0; view < 2; ++view)
	{
		auto xspan = std::span(ubo.x + view * RENDER_FOVEATION_BUFFER_DIMENSIONS, RENDER_FOVEATION_BUFFER_DIMENSIONS);
		U_LOG_E("DIAG3 view=%zu src_rect offset=(%d,%d) extent=(%d,%d) foveated_size=(%u,%u) params.x.size=%zu params.y.size=%zu",
		        view,
		        src_rect[view].offset.w, src_rect[view].offset.h,
		        src_rect[view].extent.w, src_rect[view].extent.h,
		        foveated_size.width, foveated_size.height,
		        params[view].x.size(), params[view].y.size());
		U_LOG_E("DIAG3 view=%zu ubo.x[0..4]=%u,%u,%u,%u,%u ubo.x[last-4..last]=%u,%u,%u,%u,%u",
		        view,
		        xspan[0], xspan[1], xspan[2], xspan[3], xspan[4],
		        xspan[RENDER_FOVEATION_BUFFER_DIMENSIONS - 5], xspan[RENDER_FOVEATION_BUFFER_DIMENSIONS - 4],
		        xspan[RENDER_FOVEATION_BUFFER_DIMENSIONS - 3], xspan[RENDER_FOVEATION_BUFFER_DIMENSIONS - 2],
		        xspan[RENDER_FOVEATION_BUFFER_DIMENSIONS - 1]);
	}
	}

	vmaCopyMemoryToAllocation(vk_allocator::instance(), &ubo, gpu_buffer, 0, sizeof(ubo));
	std::memcpy(gpu_buffer.data<ubo_data>(), &ubo, sizeof(ubo));
}

std::array<to_headset::foveation_parameter, 2> foveation::foveate(
        vk::raii::Device & device,
        vk::raii::CommandBuffer & cmd,
        std::array<vk::ImageView, 2> y,
        std::array<vk::ImageView, 2> cbcr,
        vk::ImageView alpha_y,
        vk::ImageView alpha_cbcr,
        bool flip_y,
        std::array<vk::ImageView, 2> src,
        std::array<xrt_rect, 2> src_rect,
        std::array<xrt_fov, 2> src_fov,
        bool alpha)
{
	update_ubo(cmd, flip_y, src_rect, src_fov);

	vk::DescriptorBufferInfo ubo_info{
	        .buffer = gpu_buffer,
	        .range = vk::WholeSize,
	};

	cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline[alpha]);

	// Real GPU driver bug (see compositor.h's struct image): a compute write
	// to array layer >=1 of a multi-planar image is corrupted, so each eye
	// gets its own dedicated single-layer image, dispatched separately
	// (groupCountZ=1; which eye is a push constant, not gl_GlobalInvocationID.z).
	for (int eye = 0; eye < 2; ++eye)
	{
		// src (Monado's swapchain image view) genuinely changes every
		// frame -- real double/triple buffering upstream -- so binding 0
		// always needs rewriting. y/cbcr/alpha_y/alpha_cbcr/ubo, by
		// contrast, only ever take on 2 distinct values each for the
		// lifetime of the session (the compositor's 2 fixed image slots),
		// alternating every other frame: skip rewriting bindings 1-5 when
		// they haven't actually changed since last time.
		std::array src_image_info{
		        vk::DescriptorImageInfo{
		                .sampler = *sampler,
		                .imageView = src[0],
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		        vk::DescriptorImageInfo{
		                .sampler = *sampler,
		                .imageView = src[1],
		                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		        },
		};
		vk::WriteDescriptorSet src_write{
		        .dstSet = descriptor_sets[eye],
		        .dstBinding = 0,
		        .descriptorCount = src_image_info.size(),
		        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
		        .pImageInfo = src_image_info.data(),
		};
		device.updateDescriptorSets(src_write, {});

		bound_views current{.y = y[eye], .cbcr = cbcr[eye], .alpha_y = alpha_y, .alpha_cbcr = alpha_cbcr};
		if (current != last_bound[eye])
		{
			last_bound[eye] = current;

			vk::DescriptorImageInfo y_info{
			        .imageView = y[eye],
			        .imageLayout = vk::ImageLayout::eGeneral,
			};
			vk::DescriptorImageInfo cbcr_info{
			        .imageView = cbcr[eye],
			        .imageLayout = vk::ImageLayout::eGeneral,
			};
			vk::DescriptorImageInfo alpha_y_info{
			        .imageView = alpha_y,
			        .imageLayout = vk::ImageLayout::eGeneral,
			};
			vk::DescriptorImageInfo alpha_cbcr_info{
			        .imageView = alpha_cbcr,
			        .imageLayout = vk::ImageLayout::eGeneral,
			};

			std::array writes = {
			        vk::WriteDescriptorSet{
			                .dstSet = descriptor_sets[eye],
			                .dstBinding = 1,
			                .descriptorCount = 1,
			                .descriptorType = vk::DescriptorType::eStorageBuffer,
			                .pBufferInfo = &ubo_info,
			        },
			        vk::WriteDescriptorSet{
			                .dstSet = descriptor_sets[eye],
			                .dstBinding = 2,
			                .descriptorCount = 1,
			                .descriptorType = vk::DescriptorType::eStorageImage,
			                .pImageInfo = &y_info,
			        },
			        vk::WriteDescriptorSet{
			                .dstSet = descriptor_sets[eye],
			                .dstBinding = 3,
			                .descriptorCount = 1,
			                .descriptorType = vk::DescriptorType::eStorageImage,
			                .pImageInfo = &cbcr_info,
			        },
			        vk::WriteDescriptorSet{
			                .dstSet = descriptor_sets[eye],
			                .dstBinding = 4,
			                .descriptorCount = 1,
			                .descriptorType = vk::DescriptorType::eStorageImage,
			                .pImageInfo = &alpha_y_info,
			        },
			        vk::WriteDescriptorSet{
			                .dstSet = descriptor_sets[eye],
			                .dstBinding = 5,
			                .descriptorCount = 1,
			                .descriptorType = vk::DescriptorType::eStorageImage,
			                .pImageInfo = &alpha_cbcr_info,
			        },
			};
			device.updateDescriptorSets(writes, {});
		}

		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0, descriptor_sets[eye], {});
		cmd.pushConstants<int32_t>(*layout, vk::ShaderStageFlagBits::eCompute, 0, eye);
		cmd.dispatch(divide_and_round_up(foveated_size.width, 8),
		             divide_and_round_up(foveated_size.height, 8),
		             1);
	}

	return params;
}
} // namespace wivrn
