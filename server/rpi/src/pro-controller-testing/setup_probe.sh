#!/bin/bash
# setup_probe.sh — Standalone Pro Controller USB gadget management
# Usage:
#   sudo bash setup_probe.sh setup       # Set up the probe gadget (replaces ns_ctrl)
#   sudo bash setup_probe.sh teardown     # Tear down the probe gadget
#   sudo bash setup_probe.sh restore      # Restore original ns_ctrl gadget

set -euo pipefail
PROBE_DIR=/sys/kernel/config/usb_gadget/ns_probe
ORIG_DIR=/sys/kernel/config/usb_gadget/ns_ctrl
CONFIG_DIR="$PROBE_DIR/configs/c.1"

cmd="${1:-help}"

# ── Pro Controller HID descriptor (from Linux kernel hid-nintendo) ──
PRO_DESC_HEX='\x05\x01\x09\x05\xa1\x01\x15\x00\x25\x01\x35\x00\x45\x01\x75\x01\x95\x0e\x05\x09\x19\x01\x29\x0e\x81\x02\x95\x02\x81\x01\x05\x01\x25\x07\x46\x3b\x01\x75\x04\x95\x01\x65\x14\x09\x39\x81\x42\x65\x00\x95\x01\x81\x01\x26\xff\x00\x46\xff\x00\x09\x30\x09\x31\x09\x32\x09\x35\x75\x08\x95\x04\x81\x02\x06\x00\xff\x09\x20\x75\x08\x95\x25\x81\x02\x06\x00\xff\x09\x21\x95\x58\x91\x02\xc0'

setup() {
    echo "[probe] Setting up Pro Controller USB gadget..."

    # Unbind existing probe if stale
    echo "" > "$PROBE_DIR/UDC" 2>/dev/null || true
    find "$CONFIG_DIR" -maxdepth 1 -type l -delete 2>/dev/null || true
    rmdir "$CONFIG_DIR/strings/0x409" 2>/dev/null || true
    rmdir "$CONFIG_DIR" 2>/dev/null || true
    rmdir "$PROBE_DIR/functions/hid.usb0" 2>/dev/null || true
    rmdir "$PROBE_DIR/strings/0x409" 2>/dev/null || true
    rmdir "$PROBE_DIR" 2>/dev/null || true
    sleep 0.3

    mkdir -p "$PROBE_DIR/strings/0x409"
    mkdir -p "$CONFIG_DIR/strings/0x409"

    echo 0x0200 > "$PROBE_DIR/bcdUSB"
    echo 0x0100 > "$PROBE_DIR/bcdDevice"
    echo 0x057e > "$PROBE_DIR/idVendor"   # Nintendo
    echo 0x2009 > "$PROBE_DIR/idProduct"  # Pro Controller
    echo 0x00   > "$PROBE_DIR/bDeviceClass"
    echo 0x00   > "$PROBE_DIR/bDeviceSubClass"
    echo 0x00   > "$PROBE_DIR/bDeviceProtocol"

    echo "PROBEPRO000001" > "$PROBE_DIR/strings/0x409/serialnumber"
    echo "Nintendo Co., Ltd." > "$PROBE_DIR/strings/0x409/manufacturer"
    echo "Pro Controller"     > "$PROBE_DIR/strings/0x409/product"
    echo 500 > "$CONFIG_DIR/MaxPower"
    echo "Probe Config" > "$CONFIG_DIR/strings/0x409/configuration"

    mkdir -p "$PROBE_DIR/functions/hid.usb0"
    echo 0   > "$PROBE_DIR/functions/hid.usb0/protocol"
    echo 0   > "$PROBE_DIR/functions/hid.usb0/subclass"
    echo 64  > "$PROBE_DIR/functions/hid.usb0/report_length"
    echo -ne "$PRO_DESC_HEX" > "$PROBE_DIR/functions/hid.usb0/report_desc"

    ln -sf "$PROBE_DIR/functions/hid.usb0" "$CONFIG_DIR/"

    UDC=$(ls /sys/class/udc/ 2>/dev/null | head -1)
    if [[ -z "$UDC" ]]; then
        echo "ERROR: no UDC. Check dtoverlay=dwc2 in /boot/config.txt" >&2
        exit 1
    fi

    echo "$UDC" > "$PROBE_DIR/UDC"
    sleep 0.5
    chmod 666 /dev/hidg* 2>/dev/null || true
    echo "[probe] Pro Controller gadget on /dev/hidg0 (bound to $UDC)"
}

teardown() {
    echo "[probe] Tearing down ns_probe..."
    echo "" > "$PROBE_DIR/UDC" 2>/dev/null || true
    rm -f "$CONFIG_DIR/hid.usb0" 2>/dev/null || true
    rmdir "$CONFIG_DIR/strings/0x409" 2>/dev/null || true
    rmdir "$CONFIG_DIR" 2>/dev/null || true
    rmdir "$PROBE_DIR/functions/hid.usb0" 2>/dev/null || true
    rmdir "$PROBE_DIR/strings/0x409" 2>/dev/null || true
    rmdir "$PROBE_DIR" 2>/dev/null || true
    echo "[probe] ns_probe removed"
}

restore() {
    if [[ -d "$ORIG_DIR" ]]; then
        echo "[gadget] Restoring original ns_ctrl..."
        UDC=$(ls /sys/class/udc/ 2>/dev/null | head -1)
        if [[ -n "$UDC" ]]; then
            echo "$UDC" > "$ORIG_DIR/UDC" 2>/dev/null || echo "WARN: could not rebind ns_ctrl"
            chmod 666 /dev/hidg* 2>/dev/null || true
            echo "[gadget] ns_ctrl restored on $UDC"
        fi
    else
        echo "[gadget] ns_ctrl config not found — nothing to restore"
    fi
}

case "$cmd" in
    setup)    setup ;;
    teardown) teardown ;;
    restore)  restore ;;
    *)
        echo "Usage: sudo bash $0 {setup|teardown|restore}"
        echo ""
        echo "  setup      Create and bind Pro Controller gadget (replaces ns_ctrl)"
        echo "  teardown   Unbind and remove ns_probe gadget"
        echo "  restore    Rebind original ns_ctrl gadget to UDC"
        exit 1
        ;;
esac
