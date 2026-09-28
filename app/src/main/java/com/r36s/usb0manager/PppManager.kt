package com.r36s.usb0manager

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File


data class PppProfile(
    val name: String = "Default",
    val simId: String = "",
    val device: String = "",
    val baud: Int = 115200,
    val apn: String = "",
    val username: String = "",
    val password: String = "",
    val dial: String = "ATD*99***1#",
    val auth: String = "auto",
    val customAt: String = ""
) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("name", name)
        put("simId", simId)
        put("device", device)
        put("baud", baud)
        put("apn", apn)
        put("username", username)
        put("password", password)
        put("dial", dial)
        put("auth", auth)
        put("customAt", customAt)
    }

    companion object {
        fun fromJson(o: JSONObject): PppProfile = PppProfile(
            name = o.optString("name", "Default"),
            simId = o.optString("simId", ""),
            device = o.optString("device", ""),
            baud = o.optInt("baud", 115200).coerceIn(1200, 921600),
            apn = o.optString("apn", ""),
            username = o.optString("username", ""),
            password = o.optString("password", ""),
            dial = o.optString("dial", "ATD*99***1#"),
            auth = o.optString("auth", "auto").lowercase(),
            customAt = o.optString("customAt", "")
        )
    }
}

object PppManager {
    private const val PREFS = "settings"
    private const val PROFILES = "ppp_profiles"
    private const val ACTIVE = "ppp_active_profile"
    const val IGNORE_USB_IDS = "ppp_ignore_usb_ids"
    const val LOGGING = "ppp_logging"
    const val AUTO_RECONNECT = "ppp_auto_reconnect"

    fun profiles(context: Context): List<PppProfile> {
        val raw = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(PROFILES, "[]") ?: "[]"
        return try {
            val a = JSONArray(raw)
            buildList {
                for (i in 0 until a.length()) {
                    runCatching { PppProfile.fromJson(a.getJSONObject(i)) }
                        .onSuccess { add(it) }
                }
            }.ifEmpty { listOf(PppProfile()) }
        } catch (_: Exception) {
            listOf(PppProfile())
        }
    }

    fun active(context: Context): PppProfile {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val activeName = prefs.getString(ACTIVE, "Default") ?: "Default"
        val sim = prefs.getString("ppp_active_sim", "") ?: ""
        val all = profiles(context)
        if (sim.isNotBlank()) {
            all.firstOrNull { it.simId == sim }?.let { return it }
        }
        return all.firstOrNull { it.name == activeName } ?: all.first()
    }

    fun setActive(context: Context, name: String) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(ACTIVE, name).apply()
    }

    fun save(context: Context, profile: PppProfile) {
        val current = profiles(context).filterNot { it.name == profile.name }
        val next = current + profile
        val array = JSONArray()
        next.forEach { array.put(it.toJson()) }
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putString(PROFILES, array.toString())
            .putString(ACTIVE, profile.name)
            .apply()
    }

    fun delete(context: Context, name: String) {
        val next = profiles(context).filterNot { it.name == name }
        val array = JSONArray()
        next.forEach { array.put(it.toJson()) }
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(PROFILES, array.toString()).apply()
    }

    fun ignoredUsbIds(context: Context): String =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(IGNORE_USB_IDS, "")?.trim().orEmpty()

    fun loggingEnabled(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(LOGGING, true)

    fun writeRuntimeProfile(context: Context, profile: PppProfile): File {
        val dir = File(context.filesDir, "ppp")
        dir.mkdirs()
        val ignored = ignoredUsbIds(context)
        val logging = if (loggingEnabled(context)) "1" else "0"
        val file = File(dir, "active.conf")
        val text = buildString {
            appendLine("NAME=${safe(profile.name)}")
            appendLine("SIM_ID=${safe(profile.simId)}")
            appendLine("DEVICE=${safe(profile.device)}")
            appendLine("BAUD=${profile.baud}")
            appendLine("APN=${safe(profile.apn)}")
            appendLine("USERNAME=${safe(profile.username)}")
            appendLine("PASSWORD=${safe(profile.password)}")
            appendLine("DIAL=${safe(profile.dial)}")
            appendLine("AUTH=${safe(profile.auth)}")
            appendLine("IGNORE_USB_IDS=${safe(ignored)}")
            appendLine("PPP_LOGGING=$logging")
            appendLine("CUSTOM_AT_BEGIN")
            append(profile.customAt.replace("\r", ""))
            if (!profile.customAt.endsWith("\n")) append('\n')
            appendLine("CUSTOM_AT_END")
        }
        file.writeText(text)
        file.setReadable(false, false)
        file.setWritable(false, false)
        file.setReadable(true, true)
        file.setWritable(true, true)

        // PPP Widget 3 compatibility: keep the custom modem initialisation
        // commands in a separate file as well.
        val init = File(dir, "modem_init.txt")
        init.writeText(profile.customAt.replace("\r", ""))
        init.setReadable(false, false)
        init.setWritable(false, false)
        init.setReadable(true, true)
        init.setWritable(true, true)
        return file
    }

    fun syncSimId(context: Context, output: String) {
        val sim = output.lineSequence()
            .firstOrNull { it.startsWith("SIM_ID=") }
            ?.substringAfter('=')?.trim()
            ?.takeIf { it.isNotBlank() && it != "UNKNOWN" }
            ?: return
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString("ppp_active_sim", sim).apply()
    }

    private fun safe(value: String): String =
        value.replace("\n", " ").replace("\r", " ")
}
