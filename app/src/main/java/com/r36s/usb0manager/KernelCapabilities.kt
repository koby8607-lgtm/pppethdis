package com.r36s.usb0manager

import android.content.Context

object KernelCapabilities {
    private fun lines(context: Context): List<String> =
        context.assets.open("r36s/kernel-capabilities.txt").bufferedReader().readLines().filter { it.isNotBlank() }

    private fun state(line: String): String? {
        val text = line.substringAfter(": ", line)
        return when {
            text.startsWith("# CONFIG_") && text.endsWith(" is not set") -> "n"
            text.startsWith("CONFIG_") && '=' in text -> text.substringAfter('=')
            else -> null
        }
    }

    fun summary(context: Context): String {
        val all = lines(context)
        val enabled = all.count { state(it)?.let { v -> v == "y" || v == "m" } == true }
        return "Supplied R36S kernel profile: $enabled/${all.size} relevant USB/network/PPP options enabled."
    }

    fun report(context: Context): String = buildString {
        append("=== R36S KERNEL CAPABILITIES (SUPPLIED DEFCONFIG) ===\n")
        lines(context).forEach { append(it).append('\n') }
        append("\nInterpretation:\n")
        append("The bundled profile is a reference for the original R36S build; the live device is checked at runtime by the helper.\n")
        append("PPP support is detected from the running kernel (/sys/module and /proc/modules) before a PPP session is attempted.\n")
        append("USB serial/ACM and PPP async/sync-TTY support must exist in the running kernel for classic serial-modem PPP.\n")
    }
}
