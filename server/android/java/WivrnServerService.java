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
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.projection.MediaProjection;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

import java.util.HashSet;
import java.util.Set;
import java.util.UUID;

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
	private static final String DISCOVERY_SERVICE_TYPE = "_wivrn._tcp.";
	private static final int DISCOVERY_PORT = 9757;
	private static final String DISCOVERY_COOKIE_PREFERENCES = "wivrn_discovery";
	private static final String DISCOVERY_COOKIE_KEY = "server_cookie";

	static
	{
		System.loadLibrary("wivrn-server");
	}

	// Set in onCreate/cleared in onDestroy, purely so
	// upgradeForMediaProjection() below (a static call from MainActivity,
	// same process) can reach startForeground() on the one actual running
	// instance -- Service instances aren't otherwise reachable statically.
	private static WivrnServerService instance;

	// MediaProjectionManager.getMediaProjection() itself throws
	// SecurityException ("Media projections require a foreground service of
	// type ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION") unless this
	// type is already active *before* that call -- confirmed live, this is
	// earlier than it looks like it should be from the API surface alone.
	// MainActivity's onActivityResult calls this first, then
	// getMediaProjection(), then setMediaProjection() below.
	public static void upgradeForMediaProjection()
	{
		if (instance != null)
			instance.startForeground(NOTIFICATION_ID, instance.buildNotification(),
			        ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE | ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
	}

	// nativeLibDir MUST be getApplicationInfo().nativeLibraryDir exactly --
	// adrenotools requires it verbatim to load its own hook libraries (see
	// server/utils/vulkan_loader.cpp). customDriverDir/customDriverLibraryName
	// are both null for "use the system driver" (the default -- see
	// DriverSettings/SettingsActivity for the only place either is ever
	// non-null).
	private native void nativeStart(String nativeLibDir, String customDriverDir, String customDriverLibraryName, boolean sysmemCompat);

	private native void nativeStop();

	// The client rejects a discovered server before connection unless this is
	// exactly its own protocol hash. Keep it native so the mDNS TXT record and
	// the actual server always come from the same build.
	private static native String nativeProtocolVersion();

	// Called from MonadoIpcService (a different Service, see its own
	// comment) when a local OpenXR app hands off a new IPC client fd.
	// Static: MonadoIpcService has no WivrnServerService instance to call
	// through (and doesn't need one -- referencing this class is enough to
	// trigger the loadLibrary above via normal Java class-init rules).
	static native int nativeAddIpcClient(int fd);

	// Called from native (server thread, inside wivrn_session's constructor)
	// once a headset reports its speaker format
	// (from_headset::headset_info_packet), and again on disconnect. The JNI
	// call itself is synchronous (CallVoidMethod blocks the caller), and
	// that caller is the server thread mid-construction of the session --
	// it must return promptly or the compositor never gets to start, the
	// same reason every other native callback in this class
	// (onHeadsetConnected etc.) only ever posts to mainHandler instead of
	// doing real work inline. AudioRecord.Builder().build() with an
	// AudioPlaybackCaptureConfiguration does real binder work and is not
	// fast/bounded enough to run inline here -- confirmed live: doing so
	// stalled session startup entirely (headset connects, but the
	// compositor never starts, so nothing ever streams and the local
	// OpenXR app times out and falls back to flat 2D).
	public void onAudioStreamStart(int sampleRate, int channels)
	{
		// A plain background thread, not mainHandler: stopAudioCapture()
		// below does audioThread.join(), which would block the UI thread
		// (risking an ANR) if it ever ran there instead.
		new Thread(() -> startAudioCapture(sampleRate, channels), "wivrn-audio-start").start();
	}

	public void onAudioStreamStop()
	{
		new Thread(this::stopAudioCapture, "wivrn-audio-stop").start();
	}

	// Fed one buffer at a time from audioThread's AudioRecord.read() loop
	// below into android_audio_device::on_audio_data()
	// (server/android/wivrn_server_jni.cpp).
	private native void nativeAudioData(byte[] data, int size);

	// Set by MainActivity once the user grants the MediaProjection consent
	// dialog (requested automatically at app start -- there is no audio-only
	// variant of that system prompt, see docs/building.md). Same process as
	// MainActivity (no android:process= in the manifest), so the object
	// reference itself is usable directly, no Binder/Parcel involved.
	private static MediaProjection mediaProjection;

	public static void setMediaProjection(MediaProjection projection)
	{
		mediaProjection = projection;
		// Mandatory since API 29 (enforced harder on 34): using a
		// MediaProjection without a registered callback throws
		// IllegalStateException the first time it's actually used for
		// capture. onStop() firing just means the grant expired/was
		// revoked -- audioThreadRunning already guards nativeAudioData
		// calls after stopAudioCapture(), so nothing else to do here.
		if (projection != null)
			projection.registerCallback(new MediaProjection.Callback()
			{
				@Override
				public void onStop()
				{
					mediaProjection = null;
				}
			}, null);
	}

	private Thread audioThread;
	private volatile boolean audioThreadRunning = false;

	// AudioPlaybackCaptureConfiguration is the Android replacement for the
	// desktop backend's PipeWire virtual-sink capture (audio_pipewire.cpp) --
	// captures whatever the local OpenXR app (or any other app tagged
	// USAGE_GAME/USAGE_MEDIA) is currently outputting. Requires the
	// MediaProjection token from the consent dialog and the RECORD_AUDIO
	// runtime permission (both handled in MainActivity before this can ever
	// be called, since headset info -- and therefore this call -- only
	// arrives after a session is already running).
	private void startAudioCapture(int sampleRate, int channels)
	{
		if (mediaProjection == null)
		{
			Log.w("WivrnAudio", "No MediaProjection granted, cannot capture audio");
			return;
		}

		AudioPlaybackCaptureConfiguration config =
		        new AudioPlaybackCaptureConfiguration.Builder(mediaProjection)
		                .addMatchingUsage(android.media.AudioAttributes.USAGE_GAME)
		                .addMatchingUsage(android.media.AudioAttributes.USAGE_MEDIA)
		                .addMatchingUsage(android.media.AudioAttributes.USAGE_UNKNOWN)
		                .build();

		int channelMask = channels >= 2 ? AudioFormat.CHANNEL_IN_STEREO : AudioFormat.CHANNEL_IN_MONO;
		AudioFormat format = new AudioFormat.Builder()
		        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
		        .setSampleRate(sampleRate)
		        .setChannelMask(channelMask)
		        .build();

		int minBufferSize = AudioRecord.getMinBufferSize(sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT);
		if (minBufferSize <= 0)
		{
			Log.e("WivrnAudio", "AudioRecord.getMinBufferSize failed: " + minBufferSize);
			return;
		}

		AudioRecord record;
		try
		{
			record = new AudioRecord.Builder()
			        .setAudioFormat(format)
			        .setAudioPlaybackCaptureConfig(config)
			        .setBufferSizeInBytes(minBufferSize * 2)
			        .build();
		}
		catch (Exception e)
		{
			// Most likely RECORD_AUDIO not granted, or the MediaProjection
			// token already spent (Android 14 forbids re-registering the
			// same one) -- either way, no audio rather than a crash.
			Log.e("WivrnAudio", "Failed to create AudioRecord for playback capture", e);
			return;
		}

		record.startRecording();

		// Mute the phone's own physical output once capture is confirmed
		// running -- AudioPlaybackCaptureConfiguration taps the mix
		// upstream of stream volume/mute, so this silences the tab's
		// speaker without affecting what nativeAudioData sends to the
		// headset. STREAM_MUSIC is what USAGE_GAME/USAGE_MEDIA map to for
		// volume control purposes. Restored in stopAudioCapture.
		AudioManager audioManager = getSystemService(AudioManager.class);
		audioManager.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_MUTE, 0);

		audioThreadRunning = true;
		audioThread = new Thread(() -> {
			byte[] buffer = new byte[minBufferSize];
			while (audioThreadRunning)
			{
				int read = record.read(buffer, 0, buffer.length);
				if (read > 0)
					nativeAudioData(buffer, read);
			}
			record.stop();
			record.release();
		}, "wivrn-audio-capture");
		audioThread.start();
	}

	private void stopAudioCapture()
	{
		audioThreadRunning = false;
		if (audioThread != null)
		{
			try
			{
				audioThread.join();
			}
			catch (InterruptedException e)
			{
				Thread.currentThread().interrupt();
			}
			audioThread = null;

			AudioManager audioManager = getSystemService(AudioManager.class);
			audioManager.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_UNMUTE, 0);
		}
	}

	private boolean started = false;
	private boolean discoveryRegistered = false;
	private boolean discoveryRegistrationRequested = false;
	private NsdManager.RegistrationListener discoveryRegistrationListener;

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
	public void onCreate()
	{
		super.onCreate();
		instance = this;
		discoveryRegistrationListener = new NsdManager.RegistrationListener()
		{
			@Override
			public void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode)
			{
				discoveryRegistered = false;
				discoveryRegistrationRequested = false;
				Log.e("WivrnDiscovery", "mDNS registration failed: " + errorCode);
			}

			@Override
			public void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode)
			{
				Log.w("WivrnDiscovery", "mDNS unregistration failed: " + errorCode);
			}

			@Override
			public void onServiceRegistered(NsdServiceInfo serviceInfo)
			{
				discoveryRegistered = true;
				Log.i("WivrnDiscovery", "Advertising " + serviceInfo.getServiceName());
			}

			@Override
			public void onServiceUnregistered(NsdServiceInfo serviceInfo)
			{
				discoveryRegistered = false;
				discoveryRegistrationRequested = false;
			}
		};
	}

	private String discoveryCookie()
	{
		SharedPreferences preferences = getSharedPreferences(DISCOVERY_COOKIE_PREFERENCES, MODE_PRIVATE);
		String cookie = preferences.getString(DISCOVERY_COOKIE_KEY, null);
		if (cookie == null)
		{
			cookie = UUID.randomUUID().toString().replace("-", "");
			preferences.edit().putString(DISCOVERY_COOKIE_KEY, cookie).apply();
		}
		return cookie;
	}

	private String discoveryServiceName()
	{
		String model = Build.MODEL == null || Build.MODEL.isEmpty() ? "Android" : Build.MODEL;
		String name = model + " WiVRn";
		// DNS-SD instance labels are limited to 63 octets. Keep the stable,
		// user-visible suffix when a vendor supplies an unusually long model.
		return name.length() <= 63 ? name : name.substring(0, 57) + " WiVRn";
	}

	private void registerDiscovery()
	{
		if (discoveryRegistrationRequested)
			return;

		String protocol = nativeProtocolVersion();
		if (protocol == null || protocol.isEmpty())
		{
			Log.e("WivrnDiscovery", "Cannot advertise without a protocol version");
			return;
		}

		NsdServiceInfo serviceInfo = new NsdServiceInfo();
		// Android resolves conflicts by changing the registered name and reports
		// the final name in onServiceRegistered(), exactly like Avahi does.
		serviceInfo.setServiceName(discoveryServiceName());
		serviceInfo.setServiceType(DISCOVERY_SERVICE_TYPE);
		serviceInfo.setPort(DISCOVERY_PORT);
		serviceInfo.setAttribute("protocol", protocol);
		serviceInfo.setAttribute("version", BuildConfig.VERSION_NAME);
		serviceInfo.setAttribute("cookie", discoveryCookie());

		try
		{
			discoveryRegistrationRequested = true;
			getSystemService(NsdManager.class).registerService(
			        serviceInfo, NsdManager.PROTOCOL_DNS_SD, discoveryRegistrationListener);
		}
		catch (RuntimeException e)
		{
			discoveryRegistrationRequested = false;
			// Discovery must never stop a manually-addressed headset connection.
			Log.e("WivrnDiscovery", "Could not register mDNS service", e);
		}
	}

	private void unregisterDiscovery()
	{
		if (!discoveryRegistrationRequested)
			return;
		try
		{
			getSystemService(NsdManager.class).unregisterService(discoveryRegistrationListener);
		}
		catch (RuntimeException e)
		{
			Log.w("WivrnDiscovery", "Could not unregister mDNS service", e);
		}
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId)
	{
		// Explicitly CONNECTED_DEVICE only here, not the manifest's default
		// (which would be the full connectedDevice|mediaProjection set) --
		// Android validates a startForeground() call against every type it
		// specifies, and mediaProjection's check requires an already-granted
		// projection. There isn't one yet this early (this runs before
		// MainActivity's consent dialog is even answered), so asking for it
		// here throws SecurityException and crashes the service on launch.
		// mediaProjection gets added with a second startForeground() call,
		// once one actually exists -- see startAudioCapture below.
		startForeground(NOTIFICATION_ID, buildNotification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);

		if (!started)
		{
			DriverSettings driver = DriverSettings.load(this);
			nativeStart(getApplicationInfo().nativeLibraryDir, driver.dir, driver.libraryName, driver.sysmemCompat);
			started = true;
			registerDiscovery();
		}

		// If Android kills this process to reclaim memory, don't
		// automatically restart the streaming session behind the user's
		// back -- let them relaunch explicitly.
		return START_NOT_STICKY;
	}

	@Override
	public void onDestroy()
	{
		unregisterDiscovery();
		if (started)
		{
			nativeStop();
			started = false;
		}
		instance = null;
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
