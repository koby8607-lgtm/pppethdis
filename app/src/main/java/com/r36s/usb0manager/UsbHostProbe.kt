package com.r36s.usb0manager

import android.content.Context
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager

object UsbHostProbe {
    fun scan(context: Context, networkOnly: Boolean = false): String {
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val devices = manager.deviceList.values.sortedWith(compareBy({ it.vendorId }, { it.productId }, { it.deviceName }))
        if (devices.isEmpty()) return "USB_HOST_DEVICES=NONE"
        val sb = StringBuilder()
        sb.append("=== USB HOST PROBE ===\n")
        sb.append("DEVICES=${devices.size}\n")
        for (device in devices) {
            val interfaces = (0 until device.interfaceCount).map { device.getInterface(it) }
            val matches = interfaces.map { describeInterface(it) }
            val interesting = matches.any { it.contains("RNDIS") || it.contains("ECM") || it.contains("NCM") || it.contains("EEM") || it.contains("QMI") || it.contains("MBIM") || it.contains("PPP") || it.contains("SERIAL") || it.contains("NETWORK") }
            if (networkOnly && !interesting) continue
            sb.append("DEVICE ${device.deviceName} VID=0x${device.vendorId.toString(16).padStart(4, '0')} PID=0x${device.productId.toString(16).padStart(4, '0')} \n")
            sb.append("  manufacturer=${device.manufacturerName ?: "-"} product=${device.productName ?: "-"} class=${hex(device.deviceClass)} subclass=${hex(device.deviceSubclass)} protocol=${hex(device.deviceProtocol)}\n")
            for (index in 0 until device.interfaceCount) {
                sb.append("  IFACE[$index] ${describeInterface(device.getInterface(index))}\n")
            }
        }
        if (networkOnly && sb.toString().lines().none { it.startsWith("DEVICE ") }) {
            sb.append("NO_NETWORK_CAPABLE_USB_DEVICE_MATCHES\n")
        }
        sb.append("\nProtocol notes:\n")
        sb.append("RNDIS/ECM/EEM/NCM require a host transport; QMI/MBIM are WWAN protocols, not ordinary Ethernet gadgets.\n")
        sb.append("The app never assumes a protocol merely from VID/PID; interface descriptors are the primary classification signal.\n")
        return sb.toString()
    }

    private fun describeInterface(i: UsbInterface): String {
        val cls = i.interfaceClass
        val sub = i.interfaceSubclass
        val proto = i.interfaceProtocol
        val label = when {
            cls == 0xE0 && sub == 0x01 && proto == 0x03 -> "RNDIS/WIRELESS"
            cls == UsbConstants.USB_CLASS_COMM && sub == 0x06 -> "CDC-ECM"
            cls == UsbConstants.USB_CLASS_COMM && sub == 0x0D -> "CDC-NCM"
            cls == UsbConstants.USB_CLASS_COMM && sub == 0x0E -> "CDC-MBIM"
            cls == UsbConstants.USB_CLASS_COMM && sub == 0x02 && proto == 0x01 -> "CDC-ACM/MODEM"
            cls == UsbConstants.USB_CLASS_COMM && sub == 0x02 -> "CDC-CONTROL/PPP-CANDIDATE"
            cls == UsbConstants.USB_CLASS_CDC_DATA -> "CDC-DATA/NETWORK"
            cls == UsbConstants.USB_CLASS_VENDOR_SPEC -> "VENDOR-SPECIFIC/WWAN-CANDIDATE"
            cls == UsbConstants.USB_CLASS_HID -> "HID"
            else -> "OTHER"
        }
        return "$label class=${hex(cls)} subclass=${hex(sub)} protocol=${hex(proto)} endpoints=${i.endpointCount}"
    }

    private fun hex(v: Int) = "0x" + v.toString(16).padStart(2, '0')
}
