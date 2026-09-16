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
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.view.Menu;
import android.view.MenuItem;
import android.widget.TextView;

import java.util.Set;

// Deliberately minimal: no app picker/launcher here -- the OpenXR app
// (e.g. a Unity build) is launched directly on the phone by the user, and
// negotiates with Monado via the OpenXR runtime broker on its own, exactly
// like it already does today. All this UI needs to do is start the server
// and show whether a headset is connected.
//
// Three real states, driven by two separate native signals (see
// WivrnServerService's own comment): "No devices connected" (no headset),
// "{name} connected" (headset's network connection is up, but no local
// OpenXR app is using it), "Streaming to {name}" (both) -- shared text
// logic with the notification via WivrnServerService.statusText so they
// can't drift apart.
public class MainActivity extends Activity implements WivrnServerService.ConnectionListener
{
	private TextView status;

	@Override
	protected void onCreate(Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);

		Intent intent = new Intent(this, WivrnServerService.class);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
			startForegroundService(intent);
		else
			startService(intent);

		status = new TextView(this);
		status.setTextSize(20);
		status.setPadding(48, 96, 48, 48);
		status.setText("No devices connected");
		setContentView(status);

		WivrnServerService.setConnectionListener(this);
	}

	@Override
	protected void onDestroy()
	{
		WivrnServerService.setConnectionListener(null);
		super.onDestroy();
	}

	// Called on the main thread (see WivrnServerService's Handler).
	@Override
	public void onStatusChanged(String headsetName, Set<Integer> connectedClients)
	{
		status.setText(WivrnServerService.statusText(headsetName, connectedClients));
	}

	// Only entry point into SettingsActivity (Turnip/adrenotools custom
	// Vulkan driver picker) -- a plain options menu, since nothing else in
	// this minimal UI needs one yet.
	@Override
	public boolean onCreateOptionsMenu(Menu menu)
	{
		menu.add("Vulkan driver settings");
		return true;
	}

	@Override
	public boolean onOptionsItemSelected(MenuItem item)
	{
		startActivity(new Intent(this, SettingsActivity.class));
		return true;
	}
}
