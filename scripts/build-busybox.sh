#!/usr/bin/env bash
set -euo pipefail

BUSYBOX="app/src/main/assets/tools/arm64-v8a/busybox"

test -s "$BUSYBOX"
chmod 755 "$BUSYBOX"

file "$BUSYBOX" | grep -Eiq 'ELF 64-bit.*ARM aarch64'
file "$BUSYBOX" | grep -Eiq 'statically linked'

echo "Using the prebuilt ARM64 BusyBox supplied with the project."
sha256sum "$BUSYBOX"
