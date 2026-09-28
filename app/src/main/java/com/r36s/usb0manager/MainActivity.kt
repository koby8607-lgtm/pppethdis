package com.r36s.usb0manager

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.widget.*
import androidx.core.content.ContextCompat
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

class MainActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var log: TextView
    private lateinit var helper: File
    private val pool = Executors.newSingleThreadExecutor()
    private val handler = Handler(Looper.getMainLooper())
    private val prefs by lazy { getSharedPreferences("settings", MODE_PRIVATE) }
    private var lastState = ""
    private val stamp: String
        get() = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
        helper = HelperManager.install(this).helper

        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8), dp(8), dp(8), dp(8))
        }

        content.addView(TextView(this).apply {
            text = "USB0 / PPP / RNDIS Manager"
            textSize = 16f
            gravity = Gravity.CENTER
            setPadding(4, 2, 4, 6)
        })

        status = TextView(this).apply {
            textSize = 11f
            setPadding(6, 6, 6, 8)
            setTextIsSelectable(true)
        }
        content.addView(status)

        section(content, "AUTO RECOVERY")
        buttonRow(content, "Fix Everything" to "fix-all", "Auto Recover" to "recover")
        buttonRow(content, "RNDIS Gadget" to "rndis-gadget", "Persist RNDIS" to "rndis-persist")
        buttonRow(content, "ConfigFS RNDIS" to "rndis-configfs", "Gadget Reset" to "rndis-reset")
        buttonRow(content, "Gadget DHCP/NAT" to "gadget-tether", "Stop Gadget DHCP" to "gadget-tether-stop")

        section(content, "USB / INTERFACE")
        buttonRow(content, "USB Reset" to "usb-reset-auto", "USB Re-authorize" to "usb-reauthorize")
        buttonRow(content, "Interface UP" to "up", "Interface DOWN" to "down")
        buttonRow(content, "Interface Reset" to "reset", "USB Power Fix" to "usb-power")
        buttonRow(content, "MAC / Gadget MAC" to "mac-fix", "ARP / Gateway MAC" to "arp-fix")

        section(content, "IP / ROUTING")
        buttonRow(content, "DHCP Renew" to "dhcp", "DHCP Retry" to "dhcp-retry")
        buttonRow(content, "Route Repair" to "route", "Connected Route" to "route-connected")
        buttonRow(content, "Policy Repair" to "policy-repair", "Flush USB Routes" to "flush-usb-routes")
        buttonRow(content, "MTU Repair" to "set-mtu", "rp_filter Repair" to "set-rpfilter")
        buttonRow(content, "DNS Repair" to "dns", "Connectivity Test" to "smart-test")

        section(content, "PPP / MODEM")
        buttonRow(content, "PPP Connect" to "ppp-connect", "PPP Disconnect" to "ppp-disconnect")
        buttonRow(content, "PPP Status" to "ppp-status", "Modem Status" to "modem-info")
        buttonRow(content, "PPP Settings" to "ppp-settings", "PPP Log" to "ppp-log")
        buttonRow(content, "PPP Diagnostics" to "ppp-diagnose", "PPP Modem Scan" to "ppp-scan-root")
        buttonRow(content, "Signal / Operator" to "modem-signal", "PPP Protocols" to "ppp-protocols")
        buttonRow(content, "PPP Reconnect" to "ppp-reconnect", "PPP Widget" to "ppp-widget")
        addCheck(content, "Auto reconnect PPP modem", "ppp_auto_reconnect", false)
        addCheck(content, "Repair Android network/DNS after PPP connect", "ppp_network_repair", true)

        section(content, "DRIVERS / USB PROTOCOLS")
        buttonRow(content, "Driver Status" to "driver-info", "Driver Fallback" to "driver-fallback")
        buttonRow(content, "USB Host Scan" to "usb-host-scan", "PPP / Modem Scan" to "ppp-scan")
        buttonRow(content, "USB Role" to "usb-role", "USB Functions" to "usb-functions")
        buttonRow(content, "Kernel Capabilities" to "kernel", "USB Host Tree" to "usb-host-tree")

        section(content, "DIAGNOSTICS / RECOVERY")
        buttonRow(content, "Diagnose" to "diagnose", "USB / RNDIS Diag" to "usbdiag")
        buttonRow(content, "Firewall Diag" to "firewall", "Snapshot" to "snapshot")
        buttonRow(content, "Restore" to "restore", "Root Check" to "root-info")
        buttonRow(content, "Save Report" to "save-report", "Share Report" to "share-report")
        buttonRow(content, "Event Log" to "events", "Role + Functions" to "usb-state")

        section(content, "STATIC IPV4 FALLBACK")
        val ipRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val ip = compactEdit("IP", InputType.TYPE_CLASS_PHONE, 1.15f)
        val prefix = compactEdit("Prefix", InputType.TYPE_CLASS_NUMBER, 0.55f)
        val gateway = compactEdit("Gateway", InputType.TYPE_CLASS_PHONE, 1.15f)
        ipRow.addView(ip); ipRow.addView(prefix); ipRow.addView(gateway)
        content.addView(ipRow)
        content.addView(smallButton("Apply static IPv4") {
            if (ip.text.isNullOrBlank() || prefix.text.isNullOrBlank() || gateway.text.isNullOrBlank()) {
                toast("Enter IP, prefix and gateway")
            } else {
                runAction("static", "static ${ip.text} ${prefix.text} ${gateway.text}")
            }
        })

        section(content, "ROOT / MONITOR")
        content.addView(TextView(this).apply {
            text = "Root: LineageOS su is used first. All networking helpers, including BusyBox, are private to this app."
            textSize = 11f
            setPadding(6, 2, 6, 4)
        })
        val monitor = CheckBox(this).apply {
            text = "Background USB0 monitor"
            textSize = 12f
            isChecked = prefs.getBoolean("monitor_on", false)
            setOnCheckedChangeListener { _, checked -> setMonitor(checked) }
        }
        content.addView(monitor)
        addCheck(content, "Auto repair IP/routes/ARP", "auto_recover", false) { checked ->
            if (checked && !prefs.getBoolean("monitor_on", false)) monitor.isChecked = true
        }
        addCheck(content, "Auto RNDIS USB reset / re-enumeration", "auto_rndis", false)
        addCheck(content, "Auto alternate-driver fallback", "auto_driver_fallback", false)
        addCheck(content, "Auto RNDIS gadget configuration", "auto_rndis_gadget", true)
        addCheck(content, "Auto gadget DHCP/NAT fallback", "auto_gadget_tether", true)

        section(content, "ROOT CONSOLE")
        val consoleRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val consoleInput = EditText(this).apply {
            hint = "status / fix-all / arp-fix / rndis-gadget"
            textSize = 11f
            isSingleLine = true
            layoutParams = LinearLayout.LayoutParams(0, dp(48), 1.0f)
        }
        consoleRow.addView(consoleInput)
        consoleRow.addView(smallButton("Run") {
            val cmd = consoleInput.text.toString().trim()
            if (cmd.isBlank()) toast("Enter a bundled helper command") else runAction("console", cmd)
        })
        content.addView(consoleRow)

        section(content, "LOG")
        log = TextView(this).apply {
            textSize = 10f
            setTextIsSelectable(true)
        }
        content.addView(ScrollView(this).apply { addView(log) }, LinearLayout.LayoutParams(-1, dp(96)))

        val scroll = ScrollView(this).apply {
            isFillViewport = true
            addView(content)
        }
        setContentView(scroll)

        append("USB0 Manager v1.9.0 — PPP Widget 3 style")
        append("Self-contained ARM64 recovery engine + BusyBox + PPPD/chat + modem tools.")
        append(KernelCapabilities.summary(this))
        runAction("startup", "check")

        handler.post(object : Runnable {
            override fun run() {
                refresh()
                handler.postDelayed(this, 4000)
            }
        })
    }

    private fun section(parent: LinearLayout, title: String) {
        parent.addView(TextView(this).apply {
            text = title
            textSize = 10f
            gravity = Gravity.CENTER_VERTICAL
            setPadding(6, dp(5), 6, dp(2))
        })
    }

    private fun buttonRow(parent: LinearLayout, vararg items: Pair<String, String>) {
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        for ((label, command) in items) {
            row.addView(smallButton(label) { runAction(label, command) }, LinearLayout.LayoutParams(0, dp(44), 1f))
        }
        if (items.size == 1) row.addView(Space(this), LinearLayout.LayoutParams(0, dp(44), 1f))
        parent.addView(row)
    }

    private fun smallButton(label: String, onClick: () -> Unit): Button = Button(this).apply {
        text = label
        textSize = 9.5f
        isAllCaps = false
        maxLines = 2
        gravity = Gravity.CENTER
        includeFontPadding = false
        minHeight = dp(42)
        minimumHeight = dp(42)
        setPadding(3, 0, 3, 0)
        setOnClickListener { onClick() }
        layoutParams = LinearLayout.LayoutParams(0, dp(44), 1f).apply { setMargins(2, 1, 2, 1) }
    }

    private fun compactEdit(hintText: String, type: Int, weight: Float): EditText = EditText(this).apply {
        hint = hintText
        inputType = type
        textSize = 11f
        isSingleLine = true
        layoutParams = LinearLayout.LayoutParams(0, dp(46), weight).apply { setMargins(2, 0, 2, 0) }
    }

    private fun addCheck(parent: LinearLayout, label: String, key: String, default: Boolean, listener: ((Boolean) -> Unit)? = null) {
        parent.addView(CheckBox(this).apply {
            text = label
            textSize = 10f
            isChecked = prefs.getBoolean(key, default)
            setOnCheckedChangeListener { _, checked ->
                prefs.edit().putBoolean(key, checked).apply()
                append("[$stamp] $label ${if (checked) "enabled" else "disabled"}")
                listener?.invoke(checked)
            }
        })
    }

    private fun runRoot(command: String): String {
        return try {
            val suCandidates = listOf("/system/xbin/su", "/system/bin/su", "/vendor/bin/su", "su")
            var lastError = "no su"
            for (su in suCandidates) {
                try {
                    val raw = command.trim()
                    val parts = raw.split(Regex("\\s+")).filter { it.isNotBlank() }.toMutableList()
                    val env = mutableListOf<String>()
                    val envPattern = Regex("[A-Za-z_][A-Za-z0-9_]*=[^\\s]+")
                    while (parts.isNotEmpty() && envPattern.matches(parts.first())) {
                        env += parts.removeAt(0)
                    }
                    if (parts.isEmpty()) return "ROOT ERROR: missing helper command"
                    val safe = parts.joinToString(" ") { "'${it.replace("'", "'\\'\\''")}'" }
                    val helperQ = helper.absolutePath.replace("'", "'\\'\\''")
                    val shell = (if (env.isNotEmpty()) env.joinToString(" ") + " " else "") + "'$helperQ' $safe"
                    val p = Runtime.getRuntime().exec(arrayOf(su, "-c", shell))
                    val stdout = p.inputStream.bufferedReader().readText()
                    val stderr = p.errorStream.bufferedReader().readText()
                    p.waitFor()
                    if (stdout.isNotBlank() || p.exitValue() == 0) {
                        return stdout + if (stderr.isNotBlank()) "\n[stderr]\n$stderr" else ""
                    }
                    lastError = stderr.ifBlank { "su exit=${p.exitValue()}" }
                } catch (e: Exception) {
                    lastError = e.message ?: "su failed"
                }
            }
            "ROOT ERROR: $lastError"
        } catch (e: Exception) {
            "ROOT ERROR: ${e.message}"
        }
    }

    private fun runAction(label: String, command: String) {
        pool.submit {
                val out = when (command) {
                "fix-all", "recover", "recover-aggressive" -> {
                    val old = command
                    val gadgetDhcp = if (prefs.getBoolean("auto_gadget_tether", true)) "1" else "0"
                    val autoRndis = if (prefs.getBoolean("auto_rndis_gadget", true)) "1" else "0"
                    runRoot("USB0_GADGET_DHCP=$gadgetDhcp USB0_AUTO_RNDIS=$autoRndis $old")
                }
                "usb-reset-auto" -> runRoot(if (prefs.getBoolean("auto_rndis", false)) "rndis-aggressive" else "rndis")
                "usb-host-scan" -> UsbHostProbe.scan(this@MainActivity)
                "ppp-scan" -> UsbHostProbe.scan(this@MainActivity, networkOnly = true)
                "ppp-scan-root" -> runRoot("ppp-scan") + "\n" + UsbHostProbe.scan(this@MainActivity, networkOnly = true)
                "ppp-connect" -> {
                    val profile = PppManager.active(this@MainActivity)
                    val file = PppManager.writeRuntimeProfile(this@MainActivity, profile)
                    runRoot("PPP_NETWORK_REPAIR=${if (prefs.getBoolean("ppp_network_repair", true)) 1 else 0} ppp-connect ${file.absolutePath}")
                }
                "ppp-settings" -> { runOnUiThread { showPppSettings() }; "PPP_SETTINGS_OPENED" }
                "ppp-status" -> runRoot("ppp-status")
                "modem-info" -> {
                    val out = runRoot("modem-info")
                    PppManager.syncSimId(this@MainActivity, out)
                    out
                }
                "ppp-log" -> runRoot("ppp-log")
                "ppp-diagnose" -> runRoot("ppp-diagnose")
                "ppp-reconnect" -> {
                    val profile = PppManager.active(this@MainActivity)
                    val file = PppManager.writeRuntimeProfile(this@MainActivity, profile)
                    runRoot("PPP_NETWORK_REPAIR=${if (prefs.getBoolean("ppp_network_repair", true)) 1 else 0} ppp-reconnect ${file.absolutePath}")
                }
                "ppp-protocols" -> runRoot("ppp-protocols")
                "modem-signal" -> runRoot("modem-signal")
                "ppp-widget" -> {
                    runOnUiThread { toast("Add the USB0 PPP widget from the launcher widget list") }
                    "WIDGET_AVAILABLE=YES"
                }
                "kernel" -> KernelCapabilities.report(this@MainActivity)
                "events" -> readEvents()
                "save-report" -> saveReport(makeFullReport()).absolutePath
                "share-report" -> {
                    val f = saveReport(makeFullReport()); runOnUiThread { shareFile(f) }; "REPORT=${f.absolutePath}"
                }
                "usb-state" -> runRoot("usb-functions") + "\n" + runRoot("usb-role")
                else -> runRoot(command)
            }
            append("\n[$stamp] $label\n$out")
            refresh()
        }
    }

    private fun refresh() {
        pool.submit {
            val s = runRoot("status")
            val state = s.lines().filter { it.contains("=") }.take(12).joinToString("|")
            if (lastState.isNotEmpty() && state != lastState) append("[$stamp] USB0 state changed")
            lastState = state
            runOnUiThread { status.text = parseSummary(s) }
        }
    }

    private fun parseSummary(s: String): String {
        fun v(key: String): String = s.lineSequence().firstOrNull { it.startsWith("$key=") }?.substringAfter("=")?.trim() ?: "-"
        return "Role: ${v("ROLE")}  Interface: ${v("INTERFACE")}  Link: ${v("LINK")}  Carrier: ${v("CARRIER")}  PPP: ${v("PPP")}\n" +
            "MAC: ${v("MAC")}  IP: ${v("IP")}/${v("PREFIX")}  GW: ${v("GW")}  Table: ${v("TABLE")}\n" +
            "Route: ${v("ROUTE")}  GW: ${v("GWTEST")}  Internet: ${v("INET")}  DNS: ${v("DNS")}  RP: ${v("RP_FILTER")}  MTU: ${v("MTU")}" 
    }

    private fun setMonitor(on: Boolean) {
        prefs.edit().putBoolean("monitor_on", on).apply()
        val intent = Intent(this, MonitorService::class.java)
        if (on) ContextCompat.startForegroundService(this, intent) else stopService(intent)
        append("[$stamp] Background monitor ${if (on) "enabled" else "disabled"}.")
    }

    private fun showPppSettings() {
        var selected = PppManager.active(this)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8), dp(4), dp(8), dp(4))
        }
        val scroll = ScrollView(this).apply { addView(root) }

        val profiles = PppManager.profiles(this).toMutableList()
        val names = profiles.map { it.name }.toMutableList()
        if (names.isEmpty()) names += "Default"
        val spinner = Spinner(this).apply {
            adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, names)
            setSelection(names.indexOf(selected.name).coerceAtLeast(0))
        }
        root.addView(TextView(this).apply {
            text = "Profile"
            textSize = 10f
        })
        root.addView(spinner, LinearLayout.LayoutParams(-1, dp(44)))

        fun field(hint: String, value: String, password: Boolean = false): EditText = EditText(this).apply {
            this.hint = hint
            setText(value)
            textSize = 11f
            isSingleLine = !password
            maxLines = if (password) 1 else 3
            if (password) inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            root.addView(this, LinearLayout.LayoutParams(-1, dp(if (password) 46 else 48)))
        }

        val name = field("Profile name", selected.name)
        val sim = field("SIM ID / ICCID (optional)", selected.simId)
        val device = field("Modem device (/dev/ttyUSB0, /dev/ttyACM0, /dev/rfcomm0; blank = auto)", selected.device)
        val baud = field("Baud", selected.baud.toString())
        val apn = field("APN", selected.apn)
        val user = field("Username", selected.username)
        val pass = field("Password", selected.password, true)
        val dial = field("Dial string", selected.dial)
        val auth = field("Auth: auto / pap / chap / none", selected.auth)
        val at = field("Custom AT commands (one command per line)", selected.customAt)

        val ignore = field("Ignored USB IDs (e.g. 05c6:904c, comma/space separated)", PppManager.ignoredUsbIds(this))
        val logging = CheckBox(this).apply {
            text = "PPP / modem logging"
            textSize = 10f
            isChecked = PppManager.loggingEnabled(this@MainActivity)
            root.addView(this, LinearLayout.LayoutParams(-1, dp(40)))
        }

        fun refill(profile: PppProfile) {
            selected = profile
            name.setText(profile.name)
            sim.setText(profile.simId)
            device.setText(profile.device)
            baud.setText(profile.baud.toString())
            apn.setText(profile.apn)
            user.setText(profile.username)
            pass.setText(profile.password)
            dial.setText(profile.dial)
            auth.setText(profile.auth)
            at.setText(profile.customAt)
        }

        spinner.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onNothingSelected(parent: android.widget.AdapterView<*>?) = Unit
            override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: android.view.View?, position: Int, id: Long) {
                profiles.firstOrNull { it.name == names.getOrNull(position) }?.let { refill(it) }
            }
        }

        AlertDialog.Builder(this)
            .setTitle("PPP / Modem Profile")
            .setView(scroll)
            .setPositiveButton("Save") { _, _ ->
                val profile = PppProfile(
                    name = name.text.toString().trim().ifBlank { "Default" },
                    simId = sim.text.toString().trim(),
                    device = device.text.toString().trim(),
                    baud = baud.text.toString().toIntOrNull()?.coerceIn(1200, 921600) ?: 115200,
                    apn = apn.text.toString().trim(),
                    username = user.text.toString(),
                    password = pass.text.toString(),
                    dial = dial.text.toString().trim().ifBlank { "ATD*99***1#" },
                    auth = auth.text.toString().trim().lowercase().ifBlank { "auto" },
                    customAt = at.text.toString()
                )
                PppManager.save(this, profile)
                prefs.edit()
                    .putString(PppManager.IGNORE_USB_IDS, ignore.text.toString().trim())
                    .putBoolean(PppManager.LOGGING, logging.isChecked)
                    .apply()
                PppManager.writeRuntimeProfile(this, profile)
                append("[$stamp] PPP profile '${profile.name}' saved; SIM matching and modem settings updated")
            }
            .setNeutralButton("New") { _, _ ->
                PppManager.save(this, PppProfile(name = "Profile-${System.currentTimeMillis() % 10000}"))
                append("[$stamp] New PPP profile created")
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun makeFullReport(): String =
        "=== USB0 MANAGER REPORT ===\nTime=$stamp\n" +
            "--- CHECK ---\n${runRoot("check")}\n" +
            "--- STATUS ---\n${runRoot("status")}\n" +
            "--- USB STATE ---\n${runRoot("usb-functions")}\n${runRoot("usb-role")}\n" +
            "--- DIAGNOSTICS ---\n${runRoot("diagnose")}\n" +
            "--- USB/RNDIS ---\n${runRoot("usbdiag")}\n" +
            "--- DRIVER MATRIX ---\n${runRoot("driver-info")}\n" +
            "--- PPP STATUS ---\n${runRoot("ppp-status")}\n" +
            "--- PPP PROTOCOLS ---\n${runRoot("ppp-protocols")}\n" +
            "--- MODEM STATUS ---\n${runRoot("modem-info")}\n" +
            "--- HOST PROBE ---\n${UsbHostProbe.scan(this)}\n" +
            "--- KERNEL ---\n${KernelCapabilities.report(this)}\n" +
            "--- EVENTS ---\n${readEvents()}"

    private fun readEvents(): String = File(filesDir, "events.log").takeIf { it.exists() }?.readText()?.takeLast(16000) ?: "No event history."

    private fun saveReport(report: String): File {
        val dir = File(getExternalFilesDir(null), "reports")
        dir.mkdirs()
        val f = File(dir, "usb0-${SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())}.txt")
        f.writeText(report)
        return f
    }

    private fun shareFile(file: File) {
        startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_TEXT, file.readText())
        }, "Share USB0 report"))
    }

    private fun append(text: String) {
        runOnUiThread { log.append("\n$text") }
        File(filesDir, "events.log").appendText("${System.currentTimeMillis()} $text\n")
    }

    private fun toast(text: String) = runOnUiThread { Toast.makeText(this, text, Toast.LENGTH_SHORT).show() }
    private fun dp(px: Int): Int = (px * resources.displayMetrics.density).toInt().coerceAtLeast(px)

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        pool.shutdownNow()
        super.onDestroy()
    }
}
