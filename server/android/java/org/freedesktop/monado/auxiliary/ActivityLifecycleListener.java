// Copyright 2022, Qualcomm Innovation Center, Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Class that listens to activity lifecycle change event.
 * @author Jarvis Huang
 * @ingroup aux_android_java
 */
// Mechanical Kotlin->Java port of Monado's own ActivityLifecycleListener.kt
// (same file, same behavior, same native method signatures) -- not modified
// logic, just ported so this module doesn't need to add a Kotlin toolchain
// for two small files (see SystemUiController.java, the other one). This is
// the exact class the OpenXR loader's Java-side glue (st_oxr's Android
// support, part of libopenxr_wivrn.so) tries to load from this app's own
// APK via loadClassFromApk() -- its absence is what caused
// XR_ERROR_RUNTIME_UNAVAILABLE the first time a real OpenXR app tried to use
// this runtime. See docs/ANDROID_PORT.md's OpenXR runtime broker section.

package org.freedesktop.monado.auxiliary;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.util.Log;

import androidx.annotation.Keep;

/** Monitor activity lifecycle of application. */
@Keep
public class ActivityLifecycleListener implements Application.ActivityLifecycleCallbacks
{
	private static final String TAG = "ActivityLifecycleListener";

	private final long nativePtr;

	public ActivityLifecycleListener(long nativePtr)
	{
		this.nativePtr = nativePtr;
	}

	/** Register callback with {@link Application} from given {@code activity} object. */
	public void registerCallback(Activity activity)
	{
		activity.getApplication().registerActivityLifecycleCallbacks(this);
	}

	/** Unregister callback with {@link Application} from given {@code activity} object. */
	public void unregisterCallback(Activity activity)
	{
		activity.getApplication().unregisterActivityLifecycleCallbacks(this);
	}

	@Override
	public void onActivityCreated(Activity activity, Bundle savedInstanceState)
	{
		Log.i(TAG, activity + " onActivityCreated");
		nativeOnActivityCreated(nativePtr, activity);
	}

	@Override
	public void onActivityStarted(Activity activity)
	{
		Log.i(TAG, activity + " onActivityStarted");
		nativeOnActivityStarted(nativePtr, activity);
	}

	@Override
	public void onActivityResumed(Activity activity)
	{
		Log.i(TAG, activity + " onActivityResumed");
		nativeOnActivityResumed(nativePtr, activity);
	}

	@Override
	public void onActivityPaused(Activity activity)
	{
		Log.i(TAG, activity + " onActivityPaused");
		nativeOnActivityPaused(nativePtr, activity);
	}

	@Override
	public void onActivityStopped(Activity activity)
	{
		Log.i(TAG, activity + " onActivityStopped");
		nativeOnActivityStopped(nativePtr, activity);
	}

	@Override
	public void onActivitySaveInstanceState(Activity activity, Bundle outState)
	{
		Log.i(TAG, activity + " onActivitySaveInstanceState");
		nativeOnActivitySaveInstanceState(nativePtr, activity);
	}

	@Override
	public void onActivityDestroyed(Activity activity)
	{
		Log.i(TAG, activity + " onActivityDestroyed");
		nativeOnActivityDestroyed(nativePtr, activity);
	}

	private native void nativeOnActivityCreated(long nativePtr, Activity activity);

	private native void nativeOnActivityStarted(long nativePtr, Activity activity);

	private native void nativeOnActivityResumed(long nativePtr, Activity activity);

	private native void nativeOnActivityPaused(long nativePtr, Activity activity);

	private native void nativeOnActivityStopped(long nativePtr, Activity activity);

	private native void nativeOnActivitySaveInstanceState(long nativePtr, Activity activity);

	private native void nativeOnActivityDestroyed(long nativePtr, Activity activity);
}
