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

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

// Foreground Service wrapper around wivrn-server's JNI entry point
// (server/android/wivrn_server_jni.cpp, built when WIVRN_ANDROID_JNI=ON).
// Runs the compositor/IPC server continuously for the lifetime of the
// service, on a background thread native side -- see that file's own
// comment for the specific simplification this implies versus desktop's
// on-demand-per-connection compositor start.
//
// NOT YET WIRED INTO A BUILDABLE APK: this class exists and is written to
// compile, but there is no Gradle module/AndroidManifest.xml that packages
// it yet -- see docs/ANDROID_PORT.md for the concrete next step.
public class WivrnServerService extends Service
{
	private static final String CHANNEL_ID = "wivrn_server";
	private static final int NOTIFICATION_ID = 1;

	static
	{
		System.loadLibrary("wivrn-server");
	}

	private native void nativeStart();

	private native void nativeStop();

	private boolean started = false;

	@Override
	public IBinder onBind(Intent intent)
	{
		return null; // not a bound service
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId)
	{
		startForeground(NOTIFICATION_ID, buildNotification());

		if (!started)
		{
			nativeStart();
			started = true;
		}

		// If Android kills this process to reclaim memory, don't
		// automatically restart the streaming session behind the user's
		// back -- let them relaunch explicitly.
		return START_NOT_STICKY;
	}

	@Override
	public void onDestroy()
	{
		if (started)
		{
			nativeStop();
			started = false;
		}
		super.onDestroy();
	}

	private Notification buildNotification()
	{
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
		{
			NotificationManager manager = getSystemService(NotificationManager.class);
			NotificationChannel channel = new NotificationChannel(
			        CHANNEL_ID,
			        "WiVRn server",
			        NotificationManager.IMPORTANCE_LOW);
			manager.createNotificationChannel(channel);
		}

		return new Notification.Builder(this, CHANNEL_ID)
		        .setContentTitle("WiVRn server running")
		        .setContentText("Streaming to headset")
		        .setSmallIcon(android.R.drawable.ic_media_play)
		        .build();
	}
}
