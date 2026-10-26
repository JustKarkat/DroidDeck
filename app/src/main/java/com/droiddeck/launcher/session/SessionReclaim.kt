package com.droiddeck.launcher.session

import android.app.ActivityManager
import android.content.Context
import android.util.Log

/**
 * The Android TV home screen, recommendations and NVIDIA services keep running
 * (and restarting) while a session is up. On the 3 GB Shield TV that costs a
 * whole A57 core: measured tvlauncher at 94% CPU and tvrecommendations at 42%
 * during a session, which starved the guest and the compositor
 * (`render_scene` 115 ms -> 8 ms once they were stopped).
 *
 * Ask Android to drop their background processes when a session starts.
 */
object SessionReclaim {
    private const val TAG = "SessionReclaim"

    private val PACKAGES = arrayOf(
        "com.google.android.tvlauncher",
        "com.google.android.tvrecommendations",
        "com.google.android.apps.mediashell",
        "com.google.android.katniss",
        "com.nvidia.tegrazone3",
    )

    fun stop(context: Context) {
        if (!SessionPrefs.reclaimMemory(context)) return
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager ?: return
        val info = ActivityManager.MemoryInfo()
        am.getMemoryInfo(info)
        if (info.totalMem > 4L * 1024 * 1024 * 1024) return
        var stopped = 0
        for (pkg in PACKAGES) {
            try {
                am.killBackgroundProcesses(pkg)
                stopped++
            } catch (t: Throwable) {
                Log.w(TAG, "could not drop $pkg", t)
            }
        }
        Log.i(TAG, "asked Android to drop $stopped background TV processes")
    }
}
