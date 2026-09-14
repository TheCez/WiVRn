// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to bootstrap the Monado IPC connection.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_android
 */
// Verbatim copy from Monado (src/xrt/ipc/android/src/main/aidl/...) -- not our
// own contribution, kept identical (including its own license header) so the
// AIDL-generated stub stays wire-compatible with Client.java, which every
// OpenXR app's loader (built from the same vendored Monado, see
// XRT_ANDROID_PACKAGE in server/CMakeLists.txt) already loads unmodified.
// See docs/ANDROID_PORT.md's OpenXR runtime broker section for the full story.

package org.freedesktop.monado.ipc;

import android.os.ParcelFileDescriptor;
import android.view.Surface;

interface IMonado {
    /*!
     * Pass one side of the socket pair to the service to set up the IPC.
     */
    void connect(in ParcelFileDescriptor parcelFileDescriptor);

    /*!
     * Provide the surface we inject into the activity, back to the service.
     */
    void passAppSurface(in Surface surface);

    /*!
     * Asking service whether it has the capbility to draw over other apps or not.
     */
    boolean canDrawOverOtherApps();
}
