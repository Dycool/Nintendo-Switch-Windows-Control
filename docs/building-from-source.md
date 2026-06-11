# Building from Source

If you prefer to compile the binaries yourself rather than using the release zips, follow the instructions for your platform below.

## Raspberry Pi (Server)

1. Clone the repository:

```bash
git clone https://github.com/Dycool/NS-PC-Control.git
cd NS-PC-Control
```

2. Install dependencies (miniupnpc for UPnP support is optional):

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ pkg-config xxd
# Optional: sudo apt-get install -y miniupnpc
```

3. Compile the unified backend:

```bash
cd server
mkdir build && cd build
cmake ..
make
```

4.  Run the server:

```bash
sudo ./ns-backend
# Hori mode
sudo ./ns-backend -hori
```

> The server automatically creates and binds the HID gadget on startup and cleans up on exit.

> **Note:** To disable UPnP support, add `-DUSE_UPNP=OFF` on the cmake command.

---

## PC Clients

Clone the repository to your PC:

```bash
git clone https://github.com/Dycool/NS-PC-Control.git
cd NS-PC-Control
```

---

### Desktop Clients (Windows, Linux, macOS)

All desktop platforms build from one source file: `client/ns-client.cpp`.

The CMake project builds a single `ns-client` executable with both GUI and CLI modes:

- **GUI mode** (default): launches the Qt6 interface
- **CLI mode**: `ns-client --cli <host> [options]` runs the terminal client

**Prerequisites:**

- CMake 3.20+
- A C++17 compiler
- Qt6 Widgets development files
- SDL3 development files

**Build:**

```bash
cd client
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

If Qt6 or SDL3 is installed outside the system search path, pass `CMAKE_PREFIX_PATH`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/path/to/qt;/path/to/sdl3"
cmake --build build --config Release
```

Platform package names vary, but on Ubuntu/Debian the main dependencies are:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build qt6-base-dev libsdl3-dev
```

On Windows, use Visual Studio Build Tools plus Qt6 and SDL3 packages built for the same MSVC toolchain. On macOS, install Xcode Command Line Tools plus Qt6 and SDL3.

---

### Mobile Client (Android)

**Prerequisites:**

- Android Studio or Android SDK (API 34), NDK (for the C JNI bridge), Gradle 8.5
- Java 17

**Build:**

```bash
cd mobile/android
gradle :app:assembleDebug --no-daemon
```

Output: `mobile/android/app/build/outputs/apk/debug/app-debug.apk`

---

### Mobile Client (iOS)

**Prerequisites:**

- macOS with Xcode 16+
- [CocoaPods](https://cocoapods.org/) (if any pods are added)

**Build:**

Open the Xcode project and build:

```bash
open mobile/ios/NSMobile.xcodeproj
```

Then select your device or simulator and press **⌘B** to build. The `.app` will be available in the Xcode build products directory.

> **Note:** Xcode copies the `webapp/` folder into the app bundle during the build phase. If the folder is missing, run the `copy_webapp.sh` script or copy `webapp/` to `mobile/ios/NSMobile/ns_mobile/` manually.
>
> The Android Gradle build does the same — it copies `webapp/` into `app/src/main/assets/ns_mobile/` during the assemble step.
>
> Sideload the resulting `.app` or exported `.ipa` using AltStore, SideStore, or Xcode's direct device install.

---

### Web App

No build step needed — the web interface is embedded in the `ns-backend` server binary. Enable it with the `-w` flag.
