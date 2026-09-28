package com.r36s.usb0manager

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        val prefs = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        if (prefs.getBoolean("monitor_on", false) || prefs.getBoolean("ppp_auto_reconnect", false)) {
            ContextCompat.startForegroundService(context, Intent(context, MonitorService::class.java))
        }
    }
}
