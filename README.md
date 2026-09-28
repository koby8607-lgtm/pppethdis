# USB0 Manager v1.9.0

R36S / Android 11 USB0 and RNDIS recovery toolkit.

## Recovery goals

The app is designed to recover both Android USB gadget mode and USB-host network interfaces. It uses LineageOS root (`su`) first, then its private helper stack.

### Gadget-mode recovery

- `svc usb getFunctions` / `setFunctions`
- persistent USB configuration properties
- ConfigFS RNDIS fallback when the framework USB setting path fails
- Android USB gadget reset (`svc usb resetUsbGadget`) when available
- USB function re-enumeration
- preservation of ADB when already enabled
- gadget MAC validation and ConfigFS `dev_addr` / `host_addr` repair
- optional USB-side DHCP/NAT fallback using bundled BusyBox `udhcpd`

### Host-mode recovery

- interface UP/DOWN/reset
- DHCPv4 retry
- connected-subnet and default-route repair
- Linux policy-routing / FIB-rule repair
- USB-only route flush/rebuild
- neighbour-table refresh
- active ARP gateway resolution
- explicit neighbour MAC installation
- MAC repair when a USB interface reports an invalid address
- MTU and `rp_filter` repair
- DNS/netd diagnostics and repair attempt
- USB device reset / re-authorization
- USB power/autosuspend recovery
- compatible kernel-driver discovery, loading and rebind attempts

### Bundled tools

The project uses the supplied prebuilt ARM64 BusyBox binary and builds the ARM64 `usb0-native`, `pppd` and `chat` tools for Android during GitHub Actions. The resulting binaries are placed in `app/src/main/assets/tools/arm64-v8a/` before Gradle packages the APK.

BusyBox is used privately by the app; it does not replace system `/system/bin` utilities.

### Root

The app searches these LineageOS root entry points in order:

- `/system/xbin/su`
- `/system/bin/su`
- `/vendor/bin/su`
- `su` from PATH

### UI

The main screen is deliberately compact and vertically scrollable for small R36S displays. Long diagnostic output is kept in a separate smaller scroll region.

## CI packaging note

The project uses the supplied prebuilt static ARM64 BusyBox binary. GitHub Actions validates that exact binary and copies it into the APK as `assets/tools/arm64-v8a/busybox`; it does not download or attempt to compile BusyBox.

## PPP / modem subsystem

USB0 Manager 1.9.0 adds a root-controlled PPP subsystem inspired by the documented PPP Widget 3 feature set, without copying its implementation. It provides named profiles with optional SIM/ICCID association, automatic modem/serial-port discovery, APN/username/password/dial configuration, custom AT commands, PPP connect/disconnect/status, signal/registration/operator reporting when the modem exposes standard AT responses, session logging, automatic PPP reconnect, and shell-triggered connect/disconnect actions.

Classic serial-modem PPP requires the running R36S kernel to expose the PPP and USB-serial paths needed by the modem. Ethernet-style modem protocols such as ECM/NCM/QMI still depend on the matching running kernel drivers; the app detects those capabilities instead of assuming they exist.

## PPP Widget 3-style subsystem

USB0 Manager now includes a root-controlled PPP/modem subsystem designed for the R36S and LineageOS 18.1. It uses ARM64 `pppd` and `chat` built from the official PPP 2.5.4 source during CI, the device's enabled PPP kernel support, and the app's existing USB/network recovery engine.

Implemented features include SIM-aware named profiles, automatic profile selection by detected SIM ID/ICCID, APN/username/password/authentication settings, custom modem AT initialization, automatic USB/serial modem discovery (including ttyUSB, ttyACM and RFCOMM-style ports), PPP connect/disconnect/reconnect, PPP and modem logs, modem signal/operator/registration queries, USB modem scanning, ignored USB VID:PID entries, live PPP/WWAN protocol/module reporting, automatic PPP reconnect, Android DNS/route repair after PPP comes up, remote connect/disconnect broadcasts compatible with PPP Widget 3 action names, and an optional home-screen USB0 PPP widget.

PPP Widget 3 also supports NCM, ECM and QMI in addition to PPP and provides full network awareness through platform-specific mechanisms. This project reports those kernel transports and uses the existing RNDIS/USB-network recovery path for Ethernet-style transports; the classic serial PPP path is handled by bundled `pppd`/`chat`. A generic app cannot reproduce PPP Widget 3's private native USB/network-awareness implementation byte-for-byte.
