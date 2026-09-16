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

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.util.Log;
import android.view.Surface;

import org.freedesktop.monado.ipc.IMonado;

import java.io.IOException;

// A bound Service exposing Monado's IMonado AIDL interface, bound by
// Client.java -- the same class every OpenXR app on this device loads from
// its own copy of vendored Monado (XRT_ANDROID_PACKAGE) -- to hand this
// app's already-running wivrn-server a new local OpenXR client connection.
//
// Deliberately NOT the same Service as WivrnServerService (long-lived,
// tied to the streaming session): this one only needs to exist long enough
// to hand off one fd per connecting app, mirroring (not copying) Monado's
// own Client.java/MonadoImpl.java split -- no Hilt/watchdog/surface-passing
// machinery, since the compositor never renders to a local Surface.
public class MonadoIpcService extends Service
{
	private static final String TAG = "MonadoIpcService";

	private final IMonado.Stub binder = new IMonado.Stub()
	{
		@Override
		public void connect(ParcelFileDescriptor parcelFileDescriptor) throws RemoteException
		{
			int fd = parcelFileDescriptor.getFd();
			Log.i(TAG, "connect: given fd " + fd);
			// Mirrors Monado's own MonadoImpl.connect() exactly: the native
			// side (ipc_server_mainloop_add_fd) takes what it needs from
			// this fd number synchronously, before this call returns --
			// closing our ParcelFileDescriptor right after is correct, not
			// a race, same as upstream.
			int result = WivrnServerService.nativeAddIpcClient(fd);
			try
			{
				parcelFileDescriptor.close();
			}
			catch (IOException e)
			{
				Log.e(TAG, "connect: close FileDescriptor failed", e);
			}
			if (result != 0)
			{
				Log.e(TAG, "Failed to hand off client fd to the running server (result " + result + ")");
				throw new IllegalStateException("server not available");
			}
		}

		@Override
		public void passAppSurface(Surface surface)
		{
			// Unused: WiVRn's compositor renders into the video encoder's
			// input, never a local on-screen Surface -- this only gets
			// called at all if canDrawOverOtherApps() below returns false.
		}

		@Override
		public boolean canDrawOverOtherApps()
		{
			// Tells Client.java this runtime has its own separate display
			// output (network streaming to the headset) and doesn't need the
			// calling app to hand it a Surface to render into.
			return true;
		}
	};

	@Override
	public IBinder onBind(Intent intent)
	{
		return binder;
	}
}
