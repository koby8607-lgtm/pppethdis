package com.r36s.usb0manager

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Binder
import android.os.Process
import java.util.concurrent.Executors

class PppControlReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val uid = Binder.getCallingUid()
        if (uid != Process.ROOT_UID && uid != Process.SHELL_UID && uid != context.applicationInfo.uid) return

        val action = intent.action ?: return
        if (action != ACTION_CONNECT && action != ACTION_DISCONNECT) return

        val pending = goAsync()
        Executors.newSingleThreadExecutor().execute {
            try {
                val profile = PppManager.active(context)
                val command = when (action) {
                    ACTION_CONNECT -> {
                        val file = PppManager.writeRuntimeProfile(context, profile)
                        "PPP_NETWORK_REPAIR=${if (context.getSharedPreferences(\"settings\", Context.MODE_PRIVATE).getBoolean(\"ppp_network_repair\", true)) 1 else 0} ppp-connect '${file.absolutePath.replace("'", "'\\''")}'"
                    }
                    else -> "ppp-disconnect"
                }
                runRoot(context, command)
            } finally {
                pending.finish()
            }
        }
    }

    private fun runRoot(context: Context, command: String): String {
        val helper = HelperManager.install(context).helper
        val q = "'${helper.absolutePath.replace("'", "'\\''")}' $command"
        var last = "no su"
        for (su in listOf("/system/xbin/su", "/system/bin/su", "/vendor/bin/su", "su")) {
            try {
                val p = Runtime.getRuntime().exec(arrayOf(su, "-c", q))
                val out = p.inputStream.bufferedReader().readText()
                val err = p.errorStream.bufferedReader().readText()
                p.waitFor()
                if (p.exitValue() == 0 || out.isNotBlank()) return out
                last = err.ifBlank { "su exit=${p.exitValue()}" }
            } catch (e: Exception) {
                last = e.message ?: "su failed"
            }
        }
        return "ROOT_ERROR=$last"
    }

    companion object {
        const val ACTION_CONNECT = "de.draisberghof.pppwidget3.ACTION_CONNECT"
        const val ACTION_DISCONNECT = "de.draisberghof.pppwidget3.ACTION_DISCONNECT"
    }
}
