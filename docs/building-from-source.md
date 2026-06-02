# 🛠️ Building from Source

If you prefer to compile the binaries yourself rather than using the release zips, follow the instructions for your platform below.

## 🍓 Raspberry Pi (Server)

1. Clone only the server portion of the repository:
```bash
git clone --depth 1 --filter=blob:none --sparse https://github.com/Dycool/Nintendo-Switch-PC-Control.git
cd Nintendo-Switch-PC-Control
git sparse-checkout set server
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

Clone only the client portion of the repository to your PC:
```bash
git clone --depth 1 --filter=blob:none --sparse https://github.com/Dycool/Nintendo-Switch-PC-Control.git
cd Nintendo-Switch-PC-Control
git sparse-checkout set client
```

### 🪟 Windows
**Prerequisite:** You must have **MSYS2** installed. [Get it here](https://www.msys2.org/). 
*Note: After installing, open the **MSYS2 UCRT64** terminal (not the default MSYS terminal).*

1. Open the **MSYS2 UCRT64** terminal and navigate to `client/windows/`.
2. Build the client:
```bash
g++.exe -std=c++17 -O2 -Wall ns-gamepad.cpp -o ns-gamepad.exe -static -lws2_32 -lxinput
```

### 🐧 Linux (Ubuntu / Debian / SteamOS)
*(Requires `build-essential` or an equivalent GCC toolchain.)*

1. Navigate to `client/linux/`.
2. Compile the client:
```bash
g++ -O3 -pthread ns-gamepad.cpp -o ns-gamepad
```
*Tip: You may need to run with `sudo` or add your user to the `input` group if the application cannot access controller events.*

### 🍎 macOS
The macOS client uses Apple's **GameController framework**.

**Prerequisite:** Install Xcode Command Line Tools: `xcode-select --install`

1. Navigate to `client/mac/`.
2. Compile the CLI client:
```bash
clang++ -std=c++17 -ObjC++ \
        -framework GameController -framework Foundation \
        ns-gamepad.mm -o ns-gamepad
```
*(Note: On macOS 10.15+, Bluetooth controllers may require you to grant **Input Monitoring** permission to your terminal app in System Settings).*