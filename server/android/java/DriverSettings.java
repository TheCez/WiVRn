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

package org.meumeu.wivrn.server;

import android.content.Context;
import android.content.SharedPreferences;

// Persisted custom Vulkan driver choice (Turnip/adrenotools -- see
// server/utils/vulkan_loader.cpp and SettingsActivity.java). First
// SharedPreferences usage in this app: no other Java-side persistence
// exists yet, so this is a plain, minimal wrapper rather than conforming to
// some pre-existing pattern (there isn't one).
//
// "dir" and "libraryName" (adrenotools' own customDriverDir/customDriverName
// terms) are the two fields nativeStart() actually needs; "displayName" and
// "driverVersion" are only for SettingsActivity's own UI, straight from the
// imported ADPKG's meta.json ("name"/"driverVersion" fields).
class DriverSettings
{
	private static final String PREFS_NAME = "driver_settings";
	private static final String KEY_DIR = "custom_driver_dir";
	private static final String KEY_LIBRARY_NAME = "custom_driver_library_name";
	private static final String KEY_DISPLAY_NAME = "custom_driver_display_name";
	private static final String KEY_DRIVER_VERSION = "custom_driver_version";

	final String dir; // null == system default driver
	final String libraryName;
	final String displayName;
	final String driverVersion;

	private DriverSettings(String dir, String libraryName, String displayName, String driverVersion)
	{
		this.dir = dir;
		this.libraryName = libraryName;
		this.displayName = displayName;
		this.driverVersion = driverVersion;
	}

	static DriverSettings load(Context context)
	{
		SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
		return new DriverSettings(
		        prefs.getString(KEY_DIR, null),
		        prefs.getString(KEY_LIBRARY_NAME, null),
		        prefs.getString(KEY_DISPLAY_NAME, null),
		        prefs.getString(KEY_DRIVER_VERSION, null));
	}

	static void save(Context context, String dir, String libraryName, String displayName, String driverVersion)
	{
		context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
		        .edit()
		        .putString(KEY_DIR, dir)
		        .putString(KEY_LIBRARY_NAME, libraryName)
		        .putString(KEY_DISPLAY_NAME, displayName)
		        .putString(KEY_DRIVER_VERSION, driverVersion)
		        .apply();
	}

	static void clear(Context context)
	{
		context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit().clear().apply();
	}

	boolean isCustom()
	{
		return dir != null && libraryName != null;
	}
}
