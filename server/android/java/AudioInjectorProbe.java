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

import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Method;

// Standalone mic-forwarding investigation probe (docs/ANDROID_PORT.md) --
// NOT part of the production streaming path, isolated diagnostic only.
//
// AudioMixingRule/AudioMix/AudioPolicy are all @SystemApi -- confirmed
// absent from the public SDK entirely (javap against android-34/android.jar
// reports "class not found" for all three), so this calls them via
// reflection rather than direct imports. Reads the MIX_ROLE_INJECTOR/
// RULE_MATCH_UID/ROUTE_FLAG_LOOP_BACK constant *values* via reflection too
// (Field.get), rather than hardcoding integers guessed from AOSP source, so
// this stays correct even if a particular OEM build's values ever differ.
//
// Expected outcome: registerAudioPolicy() requires MODIFY_AUDIO_ROUTING
// (signature|privileged) -- confirmed via
// `adb shell cat /system/etc/permissions/platform.xml` that shell's own
// UID-level grant is INTERNET only, but `com.android.shell`'s actual
// installed-package grants (dumpsys package com.android.shell) DO include
// MODIFY_AUDIO_ROUTING -- so this is expected to fail here (a normal app),
// and the point of running it is to capture the *exact* failure mode
// (exception type/message, or a non-zero return code) before deciding
// whether a Shizuku user-service (running with that same shell identity)
// is the right next step.
public class AudioInjectorProbe
{
	private static final String TAG = "WivrnAudioInjectProbe";

	public static void run(Context context, int targetUid, android.media.projection.MediaProjection projection)
	{
		try
		{
			Class<?> ruleClass = Class.forName("android.media.audiopolicy.AudioMixingRule");
			Class<?> ruleBuilderClass = Class.forName("android.media.audiopolicy.AudioMixingRule$Builder");
			Class<?> mixClass = Class.forName("android.media.audiopolicy.AudioMix");
			Class<?> mixBuilderClass = Class.forName("android.media.audiopolicy.AudioMix$Builder");
			Class<?> policyClass = Class.forName("android.media.audiopolicy.AudioPolicy");
			Class<?> policyBuilderClass = Class.forName("android.media.audiopolicy.AudioPolicy$Builder");

			int mixRoleInjector = ruleClass.getField("MIX_ROLE_INJECTOR").getInt(null);
			int ruleMatchUid = ruleClass.getField("RULE_MATCH_UID").getInt(null);
			// policyReadyToUse()'s MediaProjection exemption only applies
			// when isLoopbackRenderPolicy() is true -- confirmed via AOSP
			// source this specifically means LOOP_BACK | RENDER combined,
			// not ROUTE_FLAG_LOOP_BACK alone (which is all
			// createAudioTrackSource()'s own separate check requires -- the
			// two checks are independent). The named ROUTE_FLAG_LOOP_BACK_RENDER
			// constant itself doesn't exist on this device's framework
			// (NoSuchFieldException, confirmed live) even though present in
			// current AOSP source -- computed from the two individual flags
			// instead, since that's all the constant ever was.
			int routeFlagLoopBack = mixClass.getField("ROUTE_FLAG_LOOP_BACK").getInt(null)
			        | mixClass.getField("ROUTE_FLAG_RENDER").getInt(null);
			Log.i(TAG, "Constants: MIX_ROLE_INJECTOR=" + mixRoleInjector
			        + " RULE_MATCH_UID=" + ruleMatchUid
			        + " ROUTE_FLAG_LOOP_BACK_RENDER=" + routeFlagLoopBack
			        + " targetUid=" + targetUid
			        + " projection=" + projection);

			Object ruleBuilder = ruleBuilderClass.getConstructor().newInstance();
			ruleBuilderClass.getMethod("setTargetMixRole", int.class).invoke(ruleBuilder, mixRoleInjector);
			ruleBuilderClass.getMethod("addMixRule", int.class, Object.class).invoke(ruleBuilder, ruleMatchUid, targetUid);
			Object rule = ruleBuilderClass.getMethod("build").invoke(ruleBuilder);
			Log.i(TAG, "AudioMixingRule built");

			AudioFormat format = new AudioFormat.Builder()
			        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
			        .setSampleRate(48000)
			        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
			        .build();

			Object mixBuilder = mixBuilderClass.getConstructor(ruleClass).newInstance(rule);
			mixBuilderClass.getMethod("setFormat", AudioFormat.class).invoke(mixBuilder, format);
			mixBuilderClass.getMethod("setRouteFlags", int.class).invoke(mixBuilder, routeFlagLoopBack);
			Object mix = mixBuilderClass.getMethod("build").invoke(mixBuilder);
			Log.i(TAG, "AudioMix built");

			Object policyBuilder = policyBuilderClass.getConstructor(Context.class).newInstance(context);
			policyBuilderClass.getMethod("addMix", mixClass).invoke(policyBuilder, mix);
			policyBuilderClass.getMethod("setLooper", Looper.class).invoke(policyBuilder, Looper.getMainLooper());
			if (projection != null)
				policyBuilderClass.getMethod("setMediaProjection", android.media.projection.MediaProjection.class)
				        .invoke(policyBuilder, projection);
			Object policy = policyBuilderClass.getMethod("build").invoke(policyBuilder);
			Log.i(TAG, "AudioPolicy built, attempting registerAudioPolicy()");

			AudioManager am = context.getSystemService(AudioManager.class);
			Method registerMethod = AudioManager.class.getMethod("registerAudioPolicy", policyClass);
			int result = (Integer) registerMethod.invoke(am, policy);
			Log.i(TAG, "registerAudioPolicy() returned " + result);

			if (result != 0)
			{
				Log.e(TAG, "registerAudioPolicy failed with non-zero result: " + result);
				return;
			}

			Method createTrackMethod = policyClass.getMethod("createAudioTrackSource", mixClass);
			AudioTrack track = (AudioTrack) createTrackMethod.invoke(policy, mix);
			if (track == null)
			{
				Log.e(TAG, "createAudioTrackSource() returned null despite registerAudioPolicy() succeeding");
				return;
			}

			Log.i(TAG, "Got injector AudioTrack -- writing a 440Hz test tone for ~5s");
			track.play();
			short[] buffer = new short[4800];
			double phase = 0;
			for (int rep = 0; rep < 50; rep++)
			{
				for (int i = 0; i < buffer.length; i++)
				{
					buffer[i] = (short) (Math.sin(phase) * Short.MAX_VALUE * 0.5);
					phase += 2 * Math.PI * 440 / 48000;
				}
				track.write(buffer, 0, buffer.length);
			}
			track.stop();
			track.release();
			Log.i(TAG, "Done -- if a separate AudioRecord-based test app targeting the same UID heard the tone, injection works end to end");
		}
		catch (Throwable t)
		{
			// Deliberately catches everything -- ClassNotFoundException/
			// NoSuchMethodException would mean the reflection shape itself
			// is wrong for this device's framework build; SecurityException
			// is the expected outcome (missing MODIFY_AUDIO_ROUTING). Either
			// way, the exact type+message is the actual deliverable here.
			Log.e(TAG, "AudioInjectorProbe failed: " + t.getClass().getName() + ": " + t.getMessage(), t);
		}
	}
}
