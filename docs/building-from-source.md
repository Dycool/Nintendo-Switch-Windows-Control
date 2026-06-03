# 🛠️ Building from Source

If you prefer to compile the binaries yourself rather than using the release zips, follow the instructions for your platform below.

## 🍓 Raspberry Pi (Server)

1. Clone the repository:
```bash
git clone https://github.com/Dycool/NS-PC-Control.git
cd NS-PC-Control
```

2. Compile the server:
```bash
cd server/rpi
mkdir build && cd build
cmake ..
make
```

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
g++.exe -std=c++17 -O2 -Wall ns-gamepad.cpp -o ns-gamepad.exe -static -lws2_32 -lxinput -lwinmm
```

**To build the GUI (MSVC):**
1. Ensure you have the **Visual Studio Build Tools** installed (C++ Desktop Development workload).
2. Open the **x64 Native Tools Command Prompt for VS**.
3. Navigate to `client/windows/`.
4. Compile the resources and the GUI application:
```cmd
rc /nologo ns-gui.rc
cl /std:c++17 /O2 /EHsc /W3 ns-gui.cpp ns-gui.res /link ws2_32.lib xinput.lib setupapi.lib comctl32.lib user32.lib kernel32.lib gdi32.lib advapi32.lib winmm.lib /out:ns-gui.exe
```

---

### 🐧 Linux (Ubuntu / Debian / SteamOS)
To compile on Linux, you need a C++ compiler and the GTK3 development headers for the GUI.

**Prerequisites:**
```bash
sudo apt-get update
sudo apt-get install -y build-essential libgtk-3-dev libsdl2-dev
```

Navigate to `client/linux/` and run the following depending on what you want to build:

**Build the CLI:**
```bash
g++ -O3 -pthread ns-gamepad.cpp -o ns-gamepad -lSDL2
```
*Tip: You may need to run with `sudo` or add your user to the `input` group if the application cannot access controller events.*

**Build the GUI:**
```bash
g++ -std=c++17 -O2 -Wall ns-gui.cpp -o ns-gui $(pkg-config --cflags --libs gtk+-3.0) -lpthread -lSDL2
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
clang++ -std=c++17 -ObjC++ \
        -framework GameController -framework Foundation \
        ns-gamepad.mm -o ns-gamepad
```
*(Note: On macOS 10.15+, Bluetooth controllers may require you to grant **Input Monitoring** permission to your terminal app in System Settings).*

**Build the GUI App Bundle:**
We have included a script that automatically compiles the GUI and packages it into a native `.app` bundle.
```bash
bash build_gui.sh
```
