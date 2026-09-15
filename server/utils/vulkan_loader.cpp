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

#include "vulkan_loader.h"

#include "util/u_logging.h"

#include <adrenotools/driver.h>

#include <dlfcn.h>
#include <stdexcept>
#include <utility>

// Milestone (docs/ANDROID_PORT.md): a Samsung Galaxy Tab (Snapdragon 778G /
// Adreno 642L) confirmed live that this device's Vulkan driver only reports
// apiVersion 1.1 and does not even list VK_KHR_synchronization2 as an
// extension -- a real, hard driver limitation (this compositor's
// synchronization2 requirement is load-bearing, not a check that can be
// relaxed). GameNative/Winlator-style apps solve exactly this class of
// problem on Adreno by loading Turnip (Mesa's open Adreno Vulkan driver,
// which does implement synchronization2) via adrenotools
// (https://github.com/bylaws/libadrenotools), a rootless driver-swap
// library, instead of the vendor's own driver. This is that same mechanism.
//
// No equivalent exists for other GPU vendors (Mali, PowerVR, AMD Xclipse)
// today -- confirmed live before building this (see docs/ANDROID_PORT.md) --
// so `driver` is simply unset on every device that isn't using a custom
// Adreno driver, and this whole file reduces to a plain dlopen of the
// system libvulkan.so, identical to what vulkan-hpp's own internal
// DynamicLoader already did before this existed.

namespace
{
std::string g_native_lib_dir;
std::optional<wivrn::custom_vulkan_driver> g_driver;
} // namespace

namespace wivrn
{

void configure_vulkan_loader(std::string native_lib_dir, std::optional<custom_vulkan_driver> driver)
{
	g_native_lib_dir = std::move(native_lib_dir);
	g_driver = std::move(driver);
}

PFN_vkGetInstanceProcAddr resolve_vk_get_instance_proc_addr()
{
	const auto & driver = g_driver;
	if (driver)
	{
		// adrenotools' own required directory-separator convention:
		// customDriverDir + customDriverName must concatenate directly
		// into a real path (see its driver.cpp: stat((customDriverDir +
		// customDriverName).c_str(), ...)), so the caller-supplied dir
		// must already end in '/'.
		void * handle = adrenotools_open_libvulkan(
		        RTLD_NOW,
		        ADRENOTOOLS_DRIVER_CUSTOM,
		        nullptr, // tmpLibDir: nullptr is correct/required on API >= 29 (memfd), which this project's minSdk (33) always satisfies
		        g_native_lib_dir.c_str(),
		        driver->dir.c_str(),
		        driver->library_name.c_str(),
		        nullptr, // fileRedirectDir: ADRENOTOOLS_DRIVER_FILE_REDIRECT not requested
		        nullptr  // userMappingHandle: ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT not requested
		);

		if (handle)
		{
			auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
			        dlsym(handle, "vkGetInstanceProcAddr"));
			if (get_instance_proc_addr)
			{
				U_LOG_I("Loaded custom Vulkan driver '%s' from '%s'",
				        driver->library_name.c_str(),
				        driver->dir.c_str());
				return get_instance_proc_addr;
			}
			U_LOG_E("adrenotools_open_libvulkan succeeded but dlsym(vkGetInstanceProcAddr) failed: %s", dlerror());
		}
		else
		{
			U_LOG_E("adrenotools_open_libvulkan failed to load custom Vulkan driver '%s' from '%s' -- falling back to the system driver",
			        driver->library_name.c_str(),
			        driver->dir.c_str());
		}
		// Fall through to the plain system driver below: a bad custom
		// driver selection must never prevent the server from starting.
	}

	void * handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
	if (not handle)
		throw std::runtime_error(std::string("dlopen(\"libvulkan.so\") failed: ") + dlerror());

	auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
	        dlsym(handle, "vkGetInstanceProcAddr"));
	if (not get_instance_proc_addr)
		throw std::runtime_error(std::string("dlsym(vkGetInstanceProcAddr) on libvulkan.so failed: ") + dlerror());

	return get_instance_proc_addr;
}

} // namespace wivrn
