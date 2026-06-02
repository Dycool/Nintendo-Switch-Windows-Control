# 🎮 Nintendo Switch PC Control (Windows & Linux & macOS)

**Control your Nintendo Switch from a PC (Windows, Linux or macOS) with low latency using a Raspberry Pi.**

This project was built from scratch in **C++** and uses **UDP** to guarantee the lowest possible latency. It's the ideal setup for playing Switch games using your PC controller, avoiding the typical lag of Bluetooth or heavy script-based solutions.

> **📦 Pre-compiled Binaries Available!**
> You can download ready-to-use frontend executables for Windows, Linux, and macOS directly from the **[Releases](https://github.com/Dycool/Nintendo-Switch-PC-Control/releases)** page.

---

## 🛠️ Build and Run Tutorial

### Part 1: Raspberry Pi (Backend Only)

#### ⚙️ Prerequisite: Enable USB Gadget Mode (Boot Settings)
Before the Raspberry Pi can emulate a USB controller, you must enable the USB OTG drivers at the system level. You can do this quickly by running the following commands in your Pi's terminal:

bash
# 1. Enable the dwc2 driver in config.txt
echo "dtoverlay=dwc2" | sudo tee -a /boot/firmware/config.txt

# 2. Add required modules to cmdline.txt
sudo sed -i 's/rootwait/rootwait modules-load=dwc2,libcomposite/' /boot/firmware/cmdline.txt

# 3. Reboot the system to apply changes
sudo reboot


#### Clone the backend

Once rebooted, clone only the backend portion of the repository:

bash
git clone --depth 1 --filter=blob:none --sparse https://github.com/Dycool/Nintendo-Switch-PC-Control.git
cd Nintendo-Switch-PC-Control
git sparse-checkout set backend


#### Compile the backend

bash
cd backend/rpi
mkdir build && cd build
cmake ..
make


#### Run the USB Gadget Script

This script sets up the `libcomposite` gadget to make the Pi emulate a HORI Pokken Controller. Run it **before** connecting the Pi to the Switch and **before** starting the backend.

bash
cd .. # Return to backend/rpi
chmod +x setup_gadget.sh
sudo bash setup_gadget.sh


#### Start the Backend

bash
cd build
sudo chrt -f 99 ./ns-backend


*(`chrt -f 99` gives the process maximum real-time priority for the lowest possible latency.)*

#### Connect to the Switch

Connect the Raspberry Pi to the Switch dock:

* **Raspberry Pi 4:** USB-C port
* **Raspberry Pi Zero / Zero 2 W:** Inner Micro-USB data port

#### 🔄 Automate on Boot (Optional Systemd Service)

If you want the Raspberry Pi to automatically set up the USB gadget and start the backend every time you turn it on, you can create a systemd service.

1. Create a new service file:
bash
sudo nano /etc/systemd/system/ns-control.service


2. Paste the following configuration. **Important:** Adjust the `/home/pi/...` paths to match the exact location where you cloned the repository!
ini
[Unit]
Description=Nintendo Switch PC Control Backend
After=network.target

[Service]
# Run the gadget script before starting the backend
ExecStartPre=/bin/bash /home/YOUR_USER/Nintendo-Switch-PC-Control/backend/rpi/setup_gadget.sh
# Start the backend with real-time priority
ExecStart=/usr/bin/chrt -f 99 /home/YOUR_USER/Nintendo-Switch-PC-Control/backend/rpi/build/ns-backend
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target


3. Enable and start the service:
bash
sudo systemctl daemon-reload
sudo systemctl enable ns-control.service
sudo systemctl start ns-control.service


---

### Part 2: PC (Frontend Only)

Clone only the frontend portion of the repository:

bash
git clone --depth 1 --filter=blob:none --sparse https://github.com/Dycool/Nintendo-Switch-PC-Control.git
cd Nintendo-Switch-PC-Control
git sparse-checkout set frontend


---

## 🪟 Windows

**Prerequisite:** You must have **MSYS2** installed. [Get it here](https://www.msys2.org/). 
*Note: After installing, you must open the **MSYS2 UCRT64** terminal (not the default MSYS terminal) to build the project.*

1. Open the **MSYS2 UCRT64** terminal and navigate to the `frontend/windows/` folder.

2. Build the frontend by running:

bash
g++.exe -std=c++17 -O2 -Wall ns-gamepad.cpp -o ns-gamepad.exe -static -lws2_32 -lxinput


---

## 🐧 Linux (Ubuntu / Debian / SteamOS)

1. Navigate to the `frontend/linux/` folder.

2. Compile the frontend:

bash
g++ -O3 -pthread ns-gamepad.cpp -o ns-gamepad


*(Requires `build-essential` or an equivalent GCC toolchain.)*

3. Run the application:

bash
./ns-gamepad 192.168.1.X [/dev/input/jsY]

💡 **Tip:** To find out which `jsY` index to use, install `jstest-gtk` or `joystick` to find the correct device name by tracking real-time inputs:
bash
sudo apt install joystick
# Test which device reacts to your buttons (e.g., js0, js1, etc.)
jstest /dev/input/js0


*You may need to run with `sudo` or add your user to the `input` group if the application cannot access controller events.*

---

## 🍎 macOS

The macOS frontend uses Apple's **GameController framework**, which natively supports Xbox, PlayStation, MFi, and Switch Pro Controllers over USB or Bluetooth — no third-party drivers needed.

#### ⚙️ Prerequisite: Xcode Command Line Tools

If you don't have them installed yet, run the following and click **Install** in the dialog that appears:

bash
xcode-select --install


Verify the installation completed:

bash
clang++ --version
# Expected output: Apple clang version 15.x.x (or similar)


#### Build the frontend

1. Navigate to the `frontend/macos/` folder.

2. Compile the frontend:

bash
clang++ -std=c++17 -ObjC++ \
        -framework GameController -framework Foundation \
        ns-gamepad.mm -o ns-gamepad


#### Run the application

bash
./ns-gamepad 192.168.1.X


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

## 🔒 Security

Each UDP datagram is authenticated with a truncated **HMAC-SHA256** tag derived from a compiled-in default key. The backend silently drops any packet with an invalid tag — preventing network attackers from injecting controller inputs.

**No configuration needed.** The HMAC is always active on both sides. If you want a different key, edit `DEFAULT_SECRET` in `backend/rpi/include/protocol.hpp` and recompile.

### Additional protection layers:
| Layer | Purpose |
|---|---|
| Magic/version check | Rejects random internet noise on first read |
| Per-IP rate limiter (2000 pkts/sec) | Prevents UDP flood from saturating the Pi |
| IP pinning | Only the first valid client is accepted mid-session |
| HMAC-SHA256 (16-byte truncated) | Cryptographically authenticates every packet |
| Sequence counter | Prevents replay of old captured packets |

---

## Planned

* UI for clients

---

## 📄 License

See the repository license for details.
