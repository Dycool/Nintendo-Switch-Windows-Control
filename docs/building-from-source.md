# 🛠️ Building from Source

If you prefer to compile the binaries yourself rather than using the release zips, follow the instructions for your platform below.

## 🍓 Raspberry Pi (Server)

1. Clone the repository:
```bash
git clone https://github.com/Dycool/NS-PC-Control.git
cd NS-PC-Control
```

2. Install dependencies (miniupnpc for UPnP support is optional):
```bash
sudo apt-get update
sudo apt-get install -y cmake g++ pkg-config
# Optional: sudo apt-get install -y miniupnpc
```

3. Choose a backend and compile:

**HORI mode** (legacy, 8-byte, fastest — no gyro/rumble):
```bash
cd server/rpi
mkdir build && cd build
cmake .. -DBACKEND_SOURCE=src/backend/hori-main.cpp
make
```

**Pro Controller mode** (modern, 64-byte — gyro + rumble + macros):
```bash
cd server/rpi
mkdir build && cd build
cmake .. -DBACKEND_SOURCE=src/backend/pro-main.cpp
make
```

> The server includes **built-in USB gadget setup** — no external `setup_gadget.sh` script needed. It automatically creates and binds the HID gadget on startup and cleans up on exit.

> **Note:** To disable UPnP support, add `-DUSE_UPNP=OFF` to the cmake command.

---

## 💻 PC Clients

Clone the repository to your PC:
```bash
git clone https://github.com/Dycool/NS-PC-Control.git
cd NS-PC-Control
```

---

### 🪟 Windows
On Windows, the CLI tool is built using MinGW (GCC), while the GUI application is built using Microsoft Visual C++ (MSVC).

**To build the CLI (MinGW):**
1. Install **MSYS2** ([Get it here](https://www.msys2.org/)).
2. Open the **MSYS2 UCRT64** terminal and navigate to `client/windows/`.
3. Build the CLI client:
```bash
g++ -std=c++17 -O2 -Wall ns-gamepad.cpp -o ns-gamepad.exe -static -lws2_32 -lxinput -lwinmm -luser32 -lhid -lsetupapi
```

**To build the GUI (MSVC):**
1. Ensure you have the **Visual Studio Build Tools** installed (C++ Desktop Development workload).
2. Open the **x64 Native Tools Command Prompt for VS**.
3. Navigate to `client/windows/`.
4. Compile the resources and the GUI application:
```cmd
rc /nologo ns-gui.rc
cl /std:c++17 /O2 /EHsc /W3 ns-gui.cpp ns-gui.res /link ws2_32.lib xinput.lib setupapi.lib hid.lib comctl32.lib user32.lib kernel32.lib gdi32.lib advapi32.lib winmm.lib /out:ns-gui.exe
```

Alternatively, run `build.bat` which auto-detects MinGW or MSVC and builds both targets.

---

### 🐧 Linux (Ubuntu / Debian / SteamOS)
To compile on Linux, you need a C++ compiler and the GTK3 development headers for the GUI.

**Prerequisites:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential libgtk-3-dev libsdl3-dev
```

Navigate to `client/linux/` and run the following depending on what you want to build:

**Build the CLI:**
```bash
g++ -O3 -std=c++17 ns-gamepad.cpp -o ns-gamepad -lpthread -lSDL3
```

**Build the GUI:**
```bash
g++ -O3 -std=c++17 ns-gui.cpp -o ns-gui $(pkg-config --cflags --libs gtk+-3.0) -lpthread -lSDL3
```

---

### 🍎 macOS
The macOS client uses Apple's **GameController framework**.

**Prerequisite:** Install Xcode Command Line Tools: 
```bash
xcode-select --install
```

Navigate to `client/mac/` and run the following depending on what you want to build:

**Build the CLI:**
```bash
clang++ -std=c++17 -ObjC++ -framework GameController -framework Foundation -framework CoreGraphics -framework CoreHaptics ns-gamepad.mm -o ns-gamepad
```
*(Note: On macOS 10.15+, Bluetooth controllers may require you to grant **Input Monitoring** permission to your terminal app in System Settings).*

**Build the GUI App Bundle:**
We have included a script that automatically compiles the GUI and packages it into a native `.app` bundle.
```bash
bash build_gui.sh
```
