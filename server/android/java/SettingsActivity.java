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

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// Turnip/adrenotools custom Vulkan driver settings (see
// server/utils/vulkan_loader.cpp). Not launched at app startup -- reachable
// from MainActivity's options menu. Genuinely optional: the app works
// exactly as before if the user never opens this screen (DriverSettings
// defaults to "system driver", matching every device except the one Adreno
// tablet that ever needed this).
//
// Imports the same ADPKG-format driver package GameNative/Winlator use
// (https://github.com/bylaws/libadrenotools/blob/master/tools/ADPKG.md): a
// .zip containing a meta.json (schemaVersion/name/libraryName/driverVersion/
// ...) plus the driver's .so file(s). Picked via the standard
// ACTION_OPEN_DOCUMENT file picker -- no new dependency (no AndroidX
// documentfile/activity-result, no zip library): java.util.zip is JDK
// stdlib, org.json is built into the Android SDK.
public class SettingsActivity extends Activity
{
	private static final int REQUEST_PICK_DRIVER_ZIP = 1;

	private TextView status;

	@Override
	protected void onCreate(Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);

		LinearLayout layout = new LinearLayout(this);
		layout.setOrientation(LinearLayout.VERTICAL);
		layout.setPadding(48, 96, 48, 48);

		status = new TextView(this);
		status.setTextSize(18);
		layout.addView(status);

		Button importButton = new Button(this);
		importButton.setText("Import driver...");
		importButton.setOnClickListener(v -> pickDriverZip());
		layout.addView(importButton);

		Button resetButton = new Button(this);
		resetButton.setText("Reset to system default");
		resetButton.setOnClickListener(v -> resetToSystemDefault());
		layout.addView(resetButton);

		// Turnip's own TU_DEBUG=sysmem workaround for a real GMEM-path
		// stereo duplication/ghosting bug confirmed live (at least one
		// app, VRChat, triggers it; this project's own reference app does
		// not) -- see DriverSettings.KEY_SYSMEM_COMPAT's own comment for
		// why this is a separate, independently-toggleable setting rather
		// than something forced on for every custom driver.
		CheckBox sysmemCheckbox = new CheckBox(this);
		sysmemCheckbox.setText("Compatibility mode (fixes some apps' stereo rendering, may reduce performance)");
		sysmemCheckbox.setChecked(DriverSettings.load(this).sysmemCompat);
		sysmemCheckbox.setOnCheckedChangeListener((buttonView, isChecked) -> {
			DriverSettings.setSysmemCompat(this, isChecked);
			Toast.makeText(this, "Restart the server for this to take effect", Toast.LENGTH_SHORT).show();
		});
		layout.addView(sysmemCheckbox);

		setContentView(layout);
		updateStatus();
	}

	private void updateStatus()
	{
		DriverSettings driver = DriverSettings.load(this);
		if (driver.isCustom())
			status.setText("Current driver: " + driver.displayName + " (" + driver.driverVersion + ")\n\nRestart the server for a driver change to take effect.");
		else
			status.setText("Current driver: System default\n\nRestart the server for a driver change to take effect.");
	}

	private void pickDriverZip()
	{
		Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
		intent.addCategory(Intent.CATEGORY_OPENABLE);
		intent.setType("application/zip");
		startActivityForResult(intent, REQUEST_PICK_DRIVER_ZIP);
	}

	private void resetToSystemDefault()
	{
		DriverSettings.clear(this);
		updateStatus();
		Toast.makeText(this, "Reset to system default driver", Toast.LENGTH_SHORT).show();
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data)
	{
		super.onActivityResult(requestCode, resultCode, data);
		if (requestCode != REQUEST_PICK_DRIVER_ZIP || resultCode != RESULT_OK || data == null)
			return;

		Uri uri = data.getData();
		if (uri == null)
			return;

		try
		{
			importDriverZip(uri);
			updateStatus();
		}
		catch (Exception e)
		{
			new AlertDialog.Builder(this)
			        .setTitle("Failed to import driver")
			        .setMessage(e.getMessage())
			        .setPositiveButton("OK", null)
			        .show();
		}
	}

	// Extracts every file in the zip into a fixed directory under this
	// app's own internal data dir (getFilesDir()) -- adrenotools requires
	// this NOT be on sdcard (dlopen() refuses a world-writable path, see
	// driver.h's own docs), and getFilesDir() is exactly the "not sdcard"
	// directory this project's native code already writes config.json
	// into (wivrn_server_jni.cpp). Cleared first so a re-import never
	// leaves a previous driver's stale files mixed in with the new one.
	private void importDriverZip(Uri uri) throws IOException, JSONException
	{
		File driverDir = new File(getFilesDir(), "custom_driver");
		deleteRecursive(driverDir);
		if (!driverDir.mkdirs())
			throw new IOException("Could not create " + driverDir);

		JSONObject meta = null;

		try (InputStream in = getContentResolver().openInputStream(uri))
		{
			if (in == null)
				throw new IOException("Could not open the selected file");

			try (ZipInputStream zip = new ZipInputStream(in))
			{
				ZipEntry entry;
				while ((entry = zip.getNextEntry()) != null)
				{
					if (entry.isDirectory())
						continue;

					// ADPKG contents are flat (meta.json + .so files, no
					// subdirectories) -- discard any path components a
					// malformed/malicious zip might still have, so
					// extraction can never write outside driverDir.
					String name = new File(entry.getName()).getName();
					if (name.isEmpty())
						continue;

					if (name.equals("meta.json"))
					{
						meta = new JSONObject(readFully(zip));
					}
					else
					{
						File out = new File(driverDir, name);
						try (OutputStream os = new FileOutputStream(out))
						{
							byte[] buffer = new byte[64 * 1024];
							int read;
							while ((read = zip.read(buffer)) != -1)
								os.write(buffer, 0, read);
						}
					}
				}
			}
		}

		if (meta == null)
			throw new IOException("Not a valid driver package (no meta.json found)");

		String libraryName = meta.optString("libraryName", null);
		if (libraryName == null || !new File(driverDir, libraryName).isFile())
			throw new IOException("meta.json's libraryName ('" + libraryName + "') was not found in the package");

		DriverSettings.save(
		        this,
		        driverDir.getAbsolutePath() + "/",
		        libraryName,
		        meta.optString("name", libraryName),
		        meta.optString("driverVersion", "unknown version"));
	}

	private static String readFully(InputStream in) throws IOException
	{
		java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
		byte[] buffer = new byte[8192];
		int read;
		while ((read = in.read(buffer)) != -1)
			out.write(buffer, 0, read);
		return out.toString("UTF-8");
	}

	private static void deleteRecursive(File f)
	{
		File[] children = f.listFiles();
		if (children != null)
			for (File child : children)
				deleteRecursive(child);
		f.delete();
	}
}
