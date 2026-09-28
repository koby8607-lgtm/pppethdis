package com.r36s.usb0manager

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import androidx.core.app.NotificationCompat
import java.io.File

class MonitorService : Service() {
    private val handler = Handler(Looper.getMainLooper())
    private var last = ""
    private var lastRecoveryAt = 0L
    private var lastPppRecoveryAt = 0L
    private val recoverCooldownMs = 30_000L

    private val tick = object : Runnable {
        override fun run() {
            val state = runStatus()
            if (state.isNotBlank() && state != last) {
                appendEvent(state)
                updateNotification(state)
                if (shouldRecover(state) && System.currentTimeMillis() - lastRecoveryAt >= recoverCooldownMs) {
                    lastRecoveryAt = System.currentTimeMillis()
                    val command = if (allowAggressive() && state.contains("NATIVE_INTERFACE=NO")) "recover-aggressive" else "recover"
                    val result = runHelper(command)
                    appendEvent("AUTO_RECOVER[$command] $result")
                    if (getSharedPreferences("settings", MODE_PRIVATE).getBoolean("auto_driver_fallback", false) &&
                        (result.contains("INET=FAIL") || result.contains("ROUTE=FAIL") || result.contains("NO_INTERFACE"))) {
                        val fallback = runHelper("driver-fallback")
                        appendEvent("AUTO_DRIVER_FALLBACK $fallback")
                    }
                    updateNotification("USB0 recovery attempted")
                }
                last = state
            }

            if (allowPppAutoReconnect() && System.currentTimeMillis() - lastPppRecoveryAt >= recoverCooldownMs) {
                val ppp = runHelper("ppp-status")
                if (ppp.contains("PPPD=DOWN") &&
                    !ppp.contains("PPP_INTERFACE=ppp0") &&
                    !ppp.contains("MODEM_TTY=NONE") &&
                    ppp.contains("MODEM_TTY=")) {
                    lastPppRecoveryAt = System.currentTimeMillis()
                    val result = runHelper("ppp-reconnect")
                    appendEvent("AUTO_PPP_RECONNECT $result")
                    updateNotification(if (result.contains("PPP_CONNECTED=1")) "PPP connected" else "PPP reconnect attempted")
                }
            }

            handler.postDelayed(this, 5000)
        }
    }

    override fun onCreate() {
        super.onCreate()
        HelperManager.install(this)
        createChannel()
        startForeground(7, notification("USB0 monitor starting"))
        handler.post(tick)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun allowAggressive() =
        getSharedPreferences("settings", MODE_PRIVATE).getBoolean("auto_rndis", false)

    private fun allowPppAutoReconnect() =
        getSharedPreferences("settings", MODE_PRIVATE).getBoolean(PppManager.AUTO_RECONNECT, false)

    private fun shouldRecover(state: String): Boolean {
        val prefs = getSharedPreferences("settings", MODE_PRIVATE)
        val autoGadget = prefs.getBoolean("auto_rndis_gadget", true)
        if (!prefs.getBoolean("auto_recover", false) &&
            !(allowAggressive() && state.contains("NATIVE_INTERFACE=NO")) &&
            !(autoGadget && state.contains("ROLE=GADGET"))) return false
        val gadgetFunctionBroken = state.contains("ROLE=GADGET") &&
            !Regex("USB_FUNCTION=[^ ]*rndis", RegexOption.IGNORE_CASE).containsMatchIn(state)
        return state.contains("NATIVE_INTERFACE=NO") ||
            gadgetFunctionBroken ||
            state.contains("LINK=down") ||
            state.contains("CARRIER=0") ||
            state.contains("IP=-") ||
            state.contains("GW=-") ||
            state.contains("ROUTE=FAIL") ||
            state.contains("GWTEST=FAIL")
    }

    private fun runStatus(): String = runHelper("status").let { out ->
        if (out.isBlank()) "empty" else out.lines().filter {
            it.startsWith("ROLE=") || it.startsWith("USB_FUNCTION=") ||
                it.startsWith("INTERFACE=") || it.startsWith("NATIVE_INTERFACE=") ||
                it.startsWith("LINK=") || it.startsWith("CARRIER=") ||
                it.startsWith("IP=") || it.startsWith("GW=") ||
                it.startsWith("TABLE=") || it.startsWith("ROUTE=") ||
                it.startsWith("GWTEST=") || it.startsWith("INET=") ||
                it.startsWith("DNS=") || it.startsWith("MTU=") ||
                it.startsWith("RP_FILTER=")
        }.joinToString(" ") }

    private fun runHelper(command: String): String {
        return try {
            val tools = HelperManager.install(this)
            val q = "'${tools.helper.absolutePath.replace("'", "'\\''")}' $command"
            var lastError = "no su"
            for (su in listOf("/system/xbin/su", "/system/bin/su", "/vendor/bin/su", "su")) {
                try {
                    val process = Runtime.getRuntime().exec(arrayOf(su, "-c", q))
                    val out = process.inputStream.bufferedReader().readText()
                    val err = process.errorStream.bufferedReader().readText()
                    process.waitFor()
                    if (process.exitValue() == 0 || out.isNotBlank()) {
                        return out + if (err.isNotBlank()) "\n[stderr]\n$err" else ""
                    }
                    lastError = err.ifBlank { "su exit=${process.exitValue()}" }
                } catch (e: Exception) {
                    lastError = e.message ?: "su failed"
                }
            }
            "root-error=$lastError"
        } catch (e: Exception) {
            "root-error=${e.message}"
        }
    }

    private fun appendEvent(state: String) {
        File(filesDir, "events.log").appendText("${System.currentTimeMillis()} $state\n")
        val f = File(filesDir, "events.log")
        if (f.length() > 256 * 1024) {
            val lines = f.readLines().takeLast(1000)
            f.writeText(lines.joinToString("\n") + "\n")
        }
    }

    private fun createChannel() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel("usb0", "USB0 monitoring", NotificationManager.IMPORTANCE_LOW)
        )
    }

    private fun notification(text: String): Notification =
        NotificationCompat.Builder(this, "usb0")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentTitle("USB0 Manager")
            .setContentText(text.take(120))
            .setOngoing(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()

    private fun updateNotification(state: String) {
        getSystemService(NotificationManager::class.java).notify(7, notification(state))
    }
}
