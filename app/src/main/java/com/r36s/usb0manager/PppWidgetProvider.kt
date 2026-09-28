package com.r36s.usb0manager

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.widget.RemoteViews
import java.util.concurrent.Executors

class PppWidgetProvider : AppWidgetProvider() {
    private val executor = Executors.newSingleThreadExecutor()

    override fun onUpdate(context: Context, manager: AppWidgetManager, ids: IntArray) {
        ids.forEach { updateWidget(context, manager, it) }
    }

    override fun onReceive(context: Context, intent: Intent) {
        super.onReceive(context, intent)
        when (intent.action) {
            ACTION_CONNECT, ACTION_DISCONNECT, ACTION_REFRESH -> {
                executor.execute {
                    when (intent.action) {
                        ACTION_CONNECT -> runRoot(context, connectCommand(context))
                        ACTION_DISCONNECT -> runRoot(context, "ppp-disconnect")
                    }
                    refreshAll(context)
                }
            }
        }
    }

    private fun updateWidget(context: Context, manager: AppWidgetManager, id: Int) {
        val views = RemoteViews(context.packageName, R.layout.ppp_widget)
        views.setTextViewText(R.id.ppp_widget_title, "USB0 PPP")
        views.setTextViewText(R.id.ppp_widget_status, "Checking…")
        views.setOnClickPendingIntent(R.id.ppp_widget_connect, action(context, ACTION_CONNECT, id))
        views.setOnClickPendingIntent(R.id.ppp_widget_disconnect, action(context, ACTION_DISCONNECT, id))
        views.setOnClickPendingIntent(R.id.ppp_widget_settings, settings(context, id))
        manager.updateAppWidget(id, views)

        executor.execute {
            val status = runRoot(context, "ppp-status")
            val summary = when {
                status.contains("PPP_INTERFACE=ppp0") && status.contains("PPPD=UP") -> "CONNECTED"
                status.contains("MODEM_TTY=NONE") -> "NO MODEM"
                else -> "DISCONNECTED"
            }
            val live = RemoteViews(context.packageName, R.layout.ppp_widget)
            live.setTextViewText(R.id.ppp_widget_title, "USB0 PPP")
            live.setTextViewText(R.id.ppp_widget_status, summary)
            live.setOnClickPendingIntent(R.id.ppp_widget_connect, action(context, ACTION_CONNECT, id))
            live.setOnClickPendingIntent(R.id.ppp_widget_disconnect, action(context, ACTION_DISCONNECT, id))
            live.setOnClickPendingIntent(R.id.ppp_widget_settings, settings(context, id))
            manager.updateAppWidget(id, live)
        }
    }

    private fun connectCommand(context: Context): String {
        val profile = PppManager.active(context)
        val file = PppManager.writeRuntimeProfile(context, profile)
        val repair = if (context.getSharedPreferences("settings", Context.MODE_PRIVATE)
                .getBoolean("ppp_network_repair", true)) 1 else 0
        return "PPP_NETWORK_REPAIR=$repair ppp-connect '${file.absolutePath.replace("'", "'\\''")}'"
    }

    private fun refreshAll(context: Context) {
        val manager = AppWidgetManager.getInstance(context)
        val component = ComponentName(context, PppWidgetProvider::class.java)
        onUpdate(context, manager, manager.getAppWidgetIds(component))
    }

    private fun action(context: Context, action: String, widgetId: Int): PendingIntent {
        val intent = Intent(context, PppWidgetProvider::class.java).apply {
            this.action = action
            putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, widgetId)
        }
        return PendingIntent.getBroadcast(
            context,
            widgetId xor action.hashCode(),
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
    }

    private fun settings(context: Context, widgetId: Int): PendingIntent {
        val intent = Intent(context, MainActivity::class.java)
        return PendingIntent.getActivity(
            context,
            widgetId + 90000,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
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
        const val ACTION_CONNECT = "com.r36s.usb0manager.widget.CONNECT"
        const val ACTION_DISCONNECT = "com.r36s.usb0manager.widget.DISCONNECT"
        const val ACTION_REFRESH = "com.r36s.usb0manager.widget.REFRESH"
    }
}
