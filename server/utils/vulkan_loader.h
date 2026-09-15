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
// adrenotools' own ADPKG terms: `dir` is the directory the driver's .so
// (and any of its own dependency .so's) was extracted into, `library_name`
// is the main driver .so's filename within that directory (ADPKG's
// meta.json "libraryName" field). See wivrn_vk_bundle.cpp's own comment on
// why this only exists/matters on Android.
struct custom_vulkan_driver
{
	std::string dir;
	std::string library_name;
};

// Called once by the JNI entry point (wivrn_server_jni.cpp's nativeStart(),
// before any real server/compositor/vk_bundle work begins) to record the
// user's driver choice (persisted by the Java-side settings UI) and the
// app's own ApplicationInfo.nativeLibraryDir -- a Java-only API adrenotools
// requires exactly (see driver.h's own docs) and that native code has no
// other way to obtain. `driver` unset means "use the system driver" (the
// default, and the only option on every device this project has running
// today except the one Adreno tablet that needs this at all).
void configure_vulkan_loader(std::string native_lib_dir, std::optional<custom_vulkan_driver> driver);

// Resolves the PFN_vkGetInstanceProcAddr to bootstrap vk::raii::Context
// from, using whatever configure_vulkan_loader() was last called with:
// adrenotools_open_libvulkan()'s isolated, hook-injected libvulkan.so when
// a custom driver is configured, or a plain system libvulkan.so otherwise
// (also the fallback if configure_vulkan_loader() was never called at all,
// e.g. on desktop, where this whole mechanism doesn't apply) -- see
// vulkan_loader.cpp's own comment for the full mechanism and why this
// exists at all (Turnip/adrenotools custom Vulkan driver support).
//
// Never returns null: falls back to the plain system libvulkan.so (logging
// a warning) if a custom driver was configured but loading it failed for
// any reason, so a bad custom driver selection can never prevent the
// server from starting.
PFN_vkGetInstanceProcAddr resolve_vk_get_instance_proc_addr();

} // namespace wivrn
