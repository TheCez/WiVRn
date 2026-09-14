// Copyright 2021, Qualcomm Innovation Center, Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Class to handle system ui visibility
 * @author Jarvis Huang
 * @ingroup aux_android_java
 */
// Mechanical Kotlin->Java port of Monado's own SystemUiController.kt (same
// file, same behavior) -- see ActivityLifecycleListener.java's own comment
// for why (avoids adding a Kotlin toolchain to this module for two small
// files). Client.java (the AIDL-bootstrap class every OpenXR app on this
// device loads from this app's own APK, verbatim from upstream Monado --
// see docs/ANDROID_PORT.md) imports this directly.

package org.freedesktop.monado.auxiliary;

import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import androidx.annotation.RequiresApi;

/** Helper class that handles system ui visibility. */
public class SystemUiController
{
	private interface Impl
	{
		void hide();
	}

	private static abstract class BaseImpl implements Impl
	{
		final View view;
		private final Handler uiHandler = new Handler(Looper.getMainLooper());

		BaseImpl(View view)
		{
			this.view = view;
		}

		void runOnUiThread(Runnable runnable)
		{
			uiHandler.post(runnable);
		}
	}

	@SuppressWarnings("deprecation")
	private static class SystemUiVisibilityImpl extends BaseImpl
	{
		private static final int FLAG_FULL_SCREEN_IMMERSIVE_STICKY =
		        // Give us a stable view of content insets, be able to do
		        // fullscreen and hide navigation, and want sticky immersive.
		        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
		                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
		                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
		                | View.SYSTEM_UI_FLAG_FULLSCREEN
		                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
		                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;

		SystemUiVisibilityImpl(View view)
		{
			super(view);
			runOnUiThread(() -> view.setOnSystemUiVisibilityChangeListener(visibility -> {
				// If not fullscreen, fix it.
				if ((visibility & View.SYSTEM_UI_FLAG_FULLSCREEN) == 0)
					hide();
			}));
		}

		@Override
		public void hide()
		{
			runOnUiThread(() -> view.setSystemUiVisibility(FLAG_FULL_SCREEN_IMMERSIVE_STICKY));
		}
	}

	@RequiresApi(api = Build.VERSION_CODES.R)
	private static class WindowInsetsControllerImpl extends BaseImpl
	{
		WindowInsetsControllerImpl(View view)
		{
			super(view);
			runOnUiThread(() -> {
				WindowInsetsController controller = view.getWindowInsetsController();
				if (controller != null)
				{
					controller.addOnControllableInsetsChangedListener((c, typeMask) -> {
						if ((typeMask & WindowInsets.Type.displayCutout()) == 1
						    || (typeMask & WindowInsets.Type.statusBars()) == 1
						    || (typeMask & WindowInsets.Type.navigationBars()) == 1)
							hide();
					});
				}
			});
		}

		@Override
		public void hide()
		{
			runOnUiThread(() -> {
				WindowInsetsController controller = view.getWindowInsetsController();
				if (controller == null)
					return;
				controller.hide(
				        WindowInsets.Type.displayCutout()
				        | WindowInsets.Type.statusBars()
				        | WindowInsets.Type.navigationBars());
				controller.setSystemBarsBehavior(
				        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
			});
		}
	}

	private final Impl impl;

	public SystemUiController(View view)
	{
		impl = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
		               ? new WindowInsetsControllerImpl(view)
		               : new SystemUiVisibilityImpl(view);
	}

	/** Hide system ui and make fullscreen. */
	public void hide()
	{
		impl.hide();
	}
}
