# 🎮 Nintendo Switch PC Control (Windows & Linux & macOS)

**Control your Nintendo Switch from a PC (Windows, Linux or macOS) with low latency using a Raspberry Pi.**

This project was built from scratch in **C++** and uses **UDP** to guarantee the lowest possible latency. It's the ideal setup for playing Switch games using your PC controller, avoiding the typical lag of Bluetooth or heavy script-based solutions.

---

## 🛠️ Build and Run Tutorial

### Part 1: Raspberry Pi (Backend Only)

#### ⚙️ Prerequisite: Enable USB Gadget Mode (Boot Settings)
Before the Raspberry Pi can emulate a USB controller, you must enable the USB OTG drivers at the system level. You can do this quickly by running the following commands in your Pi's terminal:

```bash
# 1. Enable the dwc2 driver in config.txt
echo "dtoverlay=dwc2" | sudo tee -a /boot/firmware/config.txt

# 2. Add required modules to cmdline.txt
sudo sed -i 's/rootwait/rootwait modules-load=dwc2,libcomposite/' /boot/firmware/cmdline.txt

# 3. Reboot the system to apply changes
sudo reboot
```

#### Clone the backend

Once rebooted, clone only the backend portion of the repository:

```bash
git clone --depth 1 --filter=blob:none --sparse https://github.com/Dycool/Nintendo-Switch-PC-Control.git
cd Nintendo-Switch-PC-Control
git sparse-checkout set backend
```

#### Compile the backend

```bash
cd backend/rpi
mkdir build && cd build
cmake ..
make
```

#### Run the USB Gadget Script

This script sets up the `libcomposite` gadget to make the Pi emulate a HORI Pokken Controller. Run it **before** connecting the Pi to the Switch and **before** starting the backend.

```bash
cd .. # Return to backend/rpi
sudo bash setup_gadget.sh
```

#### Start the Backend

```bash
cd build
sudo chrt -f 99 ./ns-backend
```

*(`chrt -f 99` gives the process maximum real-time priority for the lowest possible latency.)*

#### Connect to the Switch

Connect the Raspberry Pi to the Switch dock:

* **Raspberry Pi 4:** USB-C port
* **Raspberry Pi Zero / Zero 2 W:** Inner Micro-USB data port

---

### Part 2: PC (Frontend Only)

Clone only the frontend portion of the repository:

```bash
git clone --depth 1 --filter=blob:none --sparse https://github.com/Dycool/Nintendo-Switch-PC-Control.git
cd Nintendo-Switch-PC-Control
git sparse-checkout set frontend
```

---

## 🪟 Windows

**Prerequisite:** You must have **MSYS2** installed. [Get it here](https://www.msys2.org/). 
*Note: After installing, you must open the **MSYS2 UCRT64** terminal (not the default MSYS terminal) to build the project.*

1. Open the **MSYS2 UCRT64** terminal and navigate to the `frontend/windows/` folder.

2. Build the frontend by running:

```bash
g++.exe -std=c++17 -O2 -Wall ns-gamepad.cpp -o ns-gamepad.exe -static -lws2_32 -lxinput
```

---

## 🐧 Linux (Ubuntu / Debian / SteamOS)

1. Navigate to the `frontend/linux/` folder.

2. Compile the frontend:

```bash
g++ -O3 -pthread ns-gamepad.cpp -o ns-gamepad
```

*(Requires `build-essential` or an equivalent GCC toolchain.)*

3. Run the application:

```bash
./ns-gamepad 192.168.1.X [/dev/input/jsY]
```
💡 **Tip:** To find out which `jsY` index to use, install `jstest-gtk` or `joystick` to find the correct device name by tracking real-time inputs:
```bash
sudo apt install joystick
# Test which device reacts to your buttons (e.g., js0, js1, etc.)
jstest /dev/input/js0
```

*You may need to run with `sudo` or add your user to the `input` group if the application cannot access controller events.*

---

## 🍎 macOS

The macOS frontend uses Apple's **GameController framework**, which natively supports Xbox, PlayStation, MFi, and Switch Pro Controllers over USB or Bluetooth — no third-party drivers needed.

#### ⚙️ Prerequisite: Xcode Command Line Tools

If you don't have them installed yet, run the following and click **Install** in the dialog that appears:

```bash
xcode-select --install
```

Verify the installation completed:

```bash
clang++ --version
# Expected output: Apple clang version 15.x.x (or similar)
```

#### Build the frontend

1. Navigate to the `frontend/macos/` folder.

2. Compile the frontend:

```bash
clang++ -std=c++17 -ObjC++ \
        -framework GameController -framework Foundation \
        ns-gamepad.mm -o ns-gamepad
```

#### Run the application

```bash
./ns-gamepad 192.168.1.X
```

#### 🎮 Controller support

Connect your controller via USB or pair it via Bluetooth before launching the app. If a controller is already connected when the app starts, it will be picked up automatically. If you connect one afterwards, it will be detected on the fly.

> **Supported controllers:** Xbox One/Series, PlayStation 4/5, Switch Pro Controller (macOS 12+), and most MFi-certified gamepads.

#### 🔒 Input Monitoring permission (Bluetooth controllers)

On **macOS 10.15 Catalina and later**, Bluetooth controllers may require the **Input Monitoring** permission. If your controller is connected but inputs aren't being read, go to:

**System Settings → Privacy & Security → Input Monitoring**

and enable it for Terminal (or whichever app you're running the frontend from).

---


## 🕹️ Controls & Shortcuts

Any **XInput-compatible controller** connected to your PC (Xbox controllers and most standard PC gamepads) can control the Nintendo Switch.

| Action  | Shortcut                                |
| ------- | --------------------------------------- |
| HOME    | Press **L3 + R3** simultaneously        |
| CAPTURE | Press **START + BACK** simultaneously   |

---

## ⚡ Latency Optimization

For the lowest possible latency:

* Use a wired Ethernet connection whenever possible.
* Run the backend with `sudo chrt -f 99`.
* Connect controllers directly to the PC via USB.
* Keep the Raspberry Pi and PC on the same local network.
* Avoid Wi-Fi power-saving modes.

---

## 📄 License

See the repository license for details.
