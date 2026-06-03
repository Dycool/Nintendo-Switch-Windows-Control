#!/bin/bash
# Build macOS GUI app with icon
set -e

clang++ -std=c++17 -ObjC++ -O2 -Wall \
    ns-gui.mm \
    -framework Cocoa -framework GameController -framework Foundation \
    -o ns-gui
echo "Built ns-gui"

# Create .app bundle
mkdir -p ns-gui.app/Contents/MacOS
mkdir -p ns-gui.app/Contents/Resources

cp ns-gui ns-gui.app/Contents/MacOS/

# Generate icon.icns from icon.png (requires macOS 10.8+)
if [ -f icon.png ]; then
    mkdir -p icon.iconset
    # Standard icon sizes required by Apple
    cp icon.png icon.iconset/icon_256x256.png
    sips -z 16 16 icon.png --out icon.iconset/icon_16x16.png >/dev/null 2>&1
    sips -z 32 32 icon.png --out icon.iconset/icon_16x16@2x.png >/dev/null 2>&1
    sips -z 32 32 icon.png --out icon.iconset/icon_32x32.png >/dev/null 2>&1
    sips -z 64 64 icon.png --out icon.iconset/icon_32x32@2x.png >/dev/null 2>&1
    sips -z 128 128 icon.png --out icon.iconset/icon_128x128.png >/dev/null 2>&1
    sips -z 256 256 icon.png --out icon.iconset/icon_128x128@2x.png >/dev/null 2>&1
    sips -z 256 256 icon.png --out icon.iconset/icon_256x256.png >/dev/null 2>&1
    sips -z 512 512 icon.png --out icon.iconset/icon_256x256@2x.png >/dev/null 2>&1
    sips -z 512 512 icon.png --out icon.iconset/icon_512x512.png >/dev/null 2>&1
    sips -z 1024 1024 icon.png --out icon.iconset/icon_512x512@2x.png >/dev/null 2>&1
    iconutil -c icns icon.iconset -o ns-gui.app/Contents/Resources/icon.icns
    rm -rf icon.iconset
    echo "Created icon.icns"
fi

cat > ns-gui.app/Contents/Info.plist <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>ns-gui</string>
    <key>CFBundleIdentifier</key>
    <string>com.nswitch.gui</string>
    <key>CFBundleName</key>
    <string>Switch PC Control</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSMinimumSystemVersion</key>
    <string>10.15</string>
    <key>CFBundleIconFile</key>
    <string>icon</string>
    <!-- Required on macOS 14+ for GameController framework background access -->
    <key>NSGameControllerUsageDescription</key>
    <string>This app uses game controllers to send inputs to your NS.</string>
    <!-- Required for Bluetooth controllers (wireless Xbox/PlayStation/Switch Pro) -->
    <key>NSBluetoothAlwaysUsageDescription</key>
    <string>This app uses Bluetooth to communicate with wireless game controllers.</string>
    <key>NSBluetoothPeripheralUsageDescription</key>
    <string>This app uses Bluetooth to communicate with wireless game controllers.</string>
    <!-- Required on macOS 10.15+ for Bluetooth HID controllers -->
    <key>NSInputMonitoringUsageDescription</key>
    <string>This app needs input monitoring to read game controller inputs when the window is not focused.</string>
</dict>
</plist>
EOF

# Sign the app bundle (ad-hoc signing for local builds)
codesign --force --deep --sign - ns-gui.app 2>/dev/null && echo "Signed ns-gui.app" || echo "Warning: codesign failed (run manually: codesign --force --deep --sign - ns-gui.app)"

echo "Created ns-gui.app bundle"
