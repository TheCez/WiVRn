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

#include <optional>
#include <string>
#include <vulkan/vulkan.h>

namespace wivrn
{

// A custom Vulkan driver imported by the user (e.g. a Turnip build), in
// adrenotools' own ADPKG terms: `dir` is the extraction directory,
// `library_name` the main driver .so's filename within it (ADPKG's
// meta.json "libraryName" field).
struct custom_vulkan_driver
{
	std::string dir;
	std::string library_name;
};

// Called once by the JNI entry point before any real server/vk_bundle work
// begins, to record the user's driver choice and the app's own
// ApplicationInfo.nativeLibraryDir (a Java-only API adrenotools requires).
// `driver` unset means "use the system driver".
void configure_vulkan_loader(std::string native_lib_dir, std::optional<custom_vulkan_driver> driver);

// Resolves PFN_vkGetInstanceProcAddr per the last configure_vulkan_loader()
// call. Never returns null: falls back to the system libvulkan.so (with a
// warning) if a custom driver was configured but failed to load, so a bad
// selection can never prevent the server from starting.
PFN_vkGetInstanceProcAddr resolve_vk_get_instance_proc_addr();

} // namespace wivrn
