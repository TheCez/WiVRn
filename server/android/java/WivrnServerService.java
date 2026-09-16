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
// Connection status is two genuinely separate native signals, combined here
// into the three states the UI actually shows:
//   - onHeadsetConnected/onHeadsetDisconnected: the real, network-level
//     "a headset's TCP connection is up" signal, fired directly from
//     run_server() in wivrn_server_jni.cpp (see its comment) with the
//     client-reported device name (from_headset::headset_info_packet's
//     system_name). This is what most people mean by "connected".
//   - onClientConnected/onClientDisconnected: Monado's own
//     ipc_server_callbacks (bridged via android_ipc_server_cb), which fire
//     for a *local OpenXR application* on the phone using this runtime over
//     IPC -- nothing does that yet (no OpenXR runtime broker registration),
//     so in practice these don't fire today, but the plumbing (and the
//     distinct "streaming" state once they do) is already correct.
// "No devices connected" (no headset) / "{name} connected" (headset, no app
// streaming through it yet) / "Streaming to {name}" (both) -- see
// MainActivity for exactly how these combine.
public class WivrnServerService extends Service
{
	private static final String CHANNEL_ID = "wivrn_server";
	private static final int NOTIFICATION_ID = 1;

	static
	{
		System.loadLibrary("wivrn-server");
	}

	// nativeLibDir MUST be getApplicationInfo().nativeLibraryDir exactly --
	// adrenotools requires it verbatim to load its own hook libraries (see
	// server/utils/vulkan_loader.cpp). customDriverDir/customDriverLibraryName
	// are both null for "use the system driver" (the default -- see
	// DriverSettings/SettingsActivity for the only place either is ever
	// non-null).
	private native void nativeStart(String nativeLibDir, String customDriverDir, String customDriverLibraryName, boolean sysmemCompat);

	private native void nativeStop();

	// Called from MonadoIpcService (a different Service, see its own
	// comment) when a local OpenXR app hands off a new IPC client fd.
	// Static: MonadoIpcService has no WivrnServerService instance to call
	// through (and doesn't need one -- referencing this class is enough to
	// trigger the loadLibrary above via normal Java class-init rules).
	static native int nativeAddIpcClient(int fd);

	private boolean started = false;

	private final Handler mainHandler = new Handler(Looper.getMainLooper());
	private final Set<Integer> connectedClients = new HashSet<>();
	private String headsetName = null; // null == no headset connected

	public interface ConnectionListener
	{
		void onStatusChanged(String headsetName, Set<Integer> connectedClients);
	}

	private static ConnectionListener listener;

	public static void setConnectionListener(ConnectionListener l)
	{
		listener = l;
	}

	private void notifyChanged()
	{
		updateNotification();
		if (listener != null)
			listener.onStatusChanged(headsetName, connectedClients);
	}

	// Called from native (server thread, via JNIEnv obtained through
	// AttachCurrentThread -- see wivrn_server_jni.cpp's call_service_method_*).
	public void onHeadsetConnected(String name)
	{
		mainHandler.post(() -> {
			headsetName = name;
			notifyChanged();
		});
	}

	public void onHeadsetDisconnected()
	{
		mainHandler.post(() -> {
			headsetName = null;
			connectedClients.clear(); // a new session starts clean
			notifyChanged();
		});
	}

	public void onClientConnected(int clientId)
	{
		mainHandler.post(() -> {
			connectedClients.add(clientId);
			notifyChanged();
		});
	}

	public void onClientDisconnected(int clientId)
	{
		mainHandler.post(() -> {
			connectedClients.remove(clientId);
			notifyChanged();
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
			DriverSettings driver = DriverSettings.load(this);
			nativeStart(getApplicationInfo().nativeLibraryDir, driver.dir, driver.libraryName, driver.sysmemCompat);
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

		return new Notification.Builder(this, CHANNEL_ID)
		        .setContentTitle("WiVRn server")
		        .setContentText(statusText(headsetName, connectedClients))
		        .setSmallIcon(android.R.drawable.ic_media_play)
		        .build();
	}

	// Shared with MainActivity so the notification and the in-app UI never
	// say something different.
	public static String statusText(String headsetName, Set<Integer> connectedClients)
	{
		if (headsetName == null)
			return "No devices connected";
		if (connectedClients.isEmpty())
			return headsetName + " connected";
		return "Streaming to " + headsetName;
	}
}
