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

package org.meumeu.wivrn.server;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.widget.TextView;

import java.util.Set;

// Deliberately minimal: no app picker/launcher here -- the OpenXR app
// (e.g. a Unity build) is launched directly on the phone by the user, and
// negotiates with Monado via the OpenXR runtime broker on its own, exactly
// like it already does today. All this UI needs to do is start the server
// and show whether a headset is connected.
//
// "No devices connected" / "Streaming (N connected)" is the full extent of
// status available right now (see WivrnServerService's onClientConnected/
// onClientDisconnected) -- there's no native hook yet for anything more
// specific (headset name/model, or a distinct "paired but not yet
// streaming frames" state), so this doesn't pretend to show information it
// doesn't have.
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
	public void onConnectedClientsChanged(Set<Integer> clientIds)
	{
		status.setText(clientIds.isEmpty()
		        ? "No devices connected"
		        : "Streaming (" + clientIds.size() + " connected)");
	}
}
