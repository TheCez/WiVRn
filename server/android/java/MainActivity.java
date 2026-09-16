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

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.projection.MediaProjectionManager;
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
	private static final int REQUEST_RECORD_AUDIO = 1;
	private static final int REQUEST_MEDIA_PROJECTION = 2;

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

		requestAudioStreamingPermissions();

		status = new TextView(this);
		status.setTextSize(20);
		status.setPadding(48, 96, 48, 48);
		status.setText("No devices connected");
		setContentView(status);

		WivrnServerService.setConnectionListener(this);

		// Mic-forwarding investigation (docs/ANDROID_PORT.md): AudioPolicy
		// injector probe. It is dormant unless an explicit debug property
		// supplies the target UID, so it never affects a normal server launch.
		// `adb shell setprop debug.wivrn.audio_inject_test <targetUid>`
		// before launching (get the UID via `pm list packages -U`).
		try
		{
			Process p = Runtime.getRuntime().exec(new String[]{"getprop", "debug.wivrn.audio_inject_test"});
			String value = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream())).readLine();
			p.waitFor();
			if (value != null && !value.isEmpty())
				AudioInjectorProbe.run(this, Integer.parseInt(value.trim()), null);
		}
		catch (Exception e)
		{
			android.util.Log.e("WivrnAudioInjectProbe", "Setup failed: " + e);
		}
	}

	// Speaker audio (game -> headset) needs both RECORD_AUDIO (runtime
	// permission, dangerous group) and a MediaProjection consent grant --
	// there is no audio-only variant of that system dialog, it's the same
	// "Start recording or casting?" prompt used for screen capture (see
	// docs/building.md). Requested once here at server start rather than
	// lazily on first headset connect, so the prompt doesn't interrupt an
	// already-streaming session.
	private void requestAudioStreamingPermissions()
	{
		if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
			requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQUEST_RECORD_AUDIO);
		else
			requestMediaProjection();
	}

	@Override
	public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults)
	{
		if (requestCode == REQUEST_RECORD_AUDIO)
		{
			if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED)
				requestMediaProjection();
			// Denied: no speaker audio this session (WivrnServerService's
			// startAudioCapture already handles a null/failed AudioRecord
			// gracefully -- streaming itself is unaffected).
		}
	}

	private void requestMediaProjection()
	{
		MediaProjectionManager manager = getSystemService(MediaProjectionManager.class);
		startActivityForResult(manager.createScreenCaptureIntent(), REQUEST_MEDIA_PROJECTION);
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data)
	{
		super.onActivityResult(requestCode, resultCode, data);
		android.util.Log.i("WivrnAudioInjectProbe", "onActivityResult requestCode=" + requestCode + " resultCode=" + resultCode + " data=" + data);
		if (requestCode == REQUEST_MEDIA_PROJECTION && resultCode == RESULT_OK && data != null)
		{
			// Must happen before getMediaProjection() below -- that call
			// itself throws SecurityException ("Media projections require a
			// foreground service of type
			// ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION") unless
			// the service is already running with that type active. Found
			// live, not obvious from the API surface alone.
			WivrnServerService.upgradeForMediaProjection();
			MediaProjectionManager manager = getSystemService(MediaProjectionManager.class);
			android.media.projection.MediaProjection projection = manager.getMediaProjection(resultCode, data);
			WivrnServerService.setMediaProjection(projection);

			// Mic-forwarding investigation retry (docs/ANDROID_PORT.md): the
			// first attempt (from onCreate, before any MediaProjection
			// existed) got past registerAudioPolicy() (confirmed via dumpsys
			// media.audio_policy -- the mix genuinely registered natively)
			// but createAudioTrackSource() returned null. AOSP source: that
			// method's policyReadyToUse() gate requires MODIFY_AUDIO_ROUTING,
			// CALL_AUDIO_INTERCEPTION, OR a valid MediaProjection -- retrying
			// now that one exists.
			try
			{
				Process p = Runtime.getRuntime().exec(new String[]{"getprop", "debug.wivrn.audio_inject_test"});
				String value = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream())).readLine();
				p.waitFor();
				if (value != null && !value.isEmpty())
					AudioInjectorProbe.run(this, Integer.parseInt(value.trim()), projection);
			}
			catch (Exception e)
			{
				android.util.Log.e("WivrnAudioInjectProbe", "Retry setup failed: " + e);
			}
		}
		// Denied/cancelled: same as a RECORD_AUDIO denial above, streaming
		// still works, just without speaker audio.
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
