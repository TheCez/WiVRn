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
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

import java.util.HashSet;
import java.util.Set;

// Foreground Service wrapper around wivrn-server's JNI entry point
// (server/android/wivrn_server_jni.cpp, built when WIVRN_ANDROID_JNI=ON).
// Runs the compositor/IPC server continuously for the lifetime of the
// service, on a background thread native side -- see that file's own
// comment for the specific simplification this implies versus desktop's
// on-demand-per-connection compositor start.
//
// Connection status: onClientConnected/onClientDisconnected below are
// called from native (android_ipc_server_cb in wivrn_server_jni.cpp,
// bridging Monado's own ipc_server_callbacks) whenever a headset's TCP
// connection to the compositor opens/closes. Right now that's the only
// granularity available -- there's no separate "streaming vs. just
// connected" native hook yet, and no headset name/model, just a numeric
// client id -- so the UI (MainActivity) shows one honest "connected"
// state rather than inventing detail this doesn't actually have.
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

	private final Handler mainHandler = new Handler(Looper.getMainLooper());
	private final Set<Integer> connectedClients = new HashSet<>();

	public interface ConnectionListener
	{
		void onConnectedClientsChanged(Set<Integer> clientIds);
	}

	private static ConnectionListener listener;

	public static void setConnectionListener(ConnectionListener l)
	{
		listener = l;
	}

	// Called from native (server thread, via JNIEnv obtained through
	// AttachCurrentThread -- see wivrn_server_jni.cpp's call_service_method).
	public void onClientConnected(int clientId)
	{
		mainHandler.post(() -> {
			connectedClients.add(clientId);
			updateNotification();
			if (listener != null)
				listener.onConnectedClientsChanged(connectedClients);
		});
	}

	public void onClientDisconnected(int clientId)
	{
		mainHandler.post(() -> {
			connectedClients.remove(clientId);
			updateNotification();
			if (listener != null)
				listener.onConnectedClientsChanged(connectedClients);
		});
	}

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

	private void updateNotification()
	{
		NotificationManager manager = getSystemService(NotificationManager.class);
		manager.notify(NOTIFICATION_ID, buildNotification());
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

		String status = connectedClients.isEmpty()
		        ? "No devices connected"
		        : "Streaming (" + connectedClients.size() + " connected)";

		return new Notification.Builder(this, CHANNEL_ID)
		        .setContentTitle("WiVRn server")
		        .setContentText(status)
		        .setSmallIcon(android.R.drawable.ic_media_play)
		        .build();
	}
}
