# 🎮 Nintendo Switch PC Control (Windows & Linux)

**Control your Nintendo Switch from a PC (Windows or Linux) with ultra-low latency using a Raspberry Pi.**

This project was built from scratch in **pure C++** and uses **UDP** to guarantee the lowest possible latency. It's the ideal setup for playing Switch games using your PC controller, avoiding the typical lag of Bluetooth or heavy script-based solutions.

---

## 🛠️ Build and Run Tutorial

### Part 1: Raspberry Pi (Backend Only)

#### ⚙️ Prerequisite: Enable USB Gadget Mode (Boot Settings)
Before the Raspberry Pi can emulate a USB controller, you must enable the USB OTG drivers at the system level. 

1. Edit the boot config file:
   ```bash
   sudo nano /boot/firmware/config.txt 
   # Note: use /boot/config.txt on older Raspberry Pi OS versions
   ```
2. Scroll to the bottom and add the following line:
   ```ini
   dtoverlay=dwc2
   ```
3. Save and exit (Ctrl+O, Enter, Ctrl+X).
4. Edit the kernel command line file:
   ```bash
   sudo nano /boot/firmware/cmdline.txt
   # Note: use /boot/cmdline.txt on older Raspberry Pi OS versions
   ```
5. Add the following text right after `rootwait`. **(Important: Do not add a new line; `cmdline.txt` must remain a single, continuous line of text):**
   ```text
   modules-load=dwc2,libcomposite
   ```
6. Reboot your Raspberry Pi to apply the changes:
   ```bash
   sudo reboot
   ```

#### Clone the backend

Once rebooted, clone only the backend portion of the repository:

```bash
git clone --depth 1 --filter=blob:none --sparse [https://github.com/Dycool/Nintendo-Switch-PC-Control.git](https://github.com/Dycool/Nintendo-Switch-PC-Control.git) && \
cd Nintendo-Switch-PC-Control && \
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
git clone --depth 1 --filter=blob:none --sparse [https://github.com/Dycool/Nintendo-Switch-PC-Control.git](https://github.com/Dycool/Nintendo-Switch-PC-Control.git) && \
cd Nintendo-Switch-PC-Control && \
git sparse-checkout set frontend
```

---

## 🪟 Windows

1. Navigate to the `frontend/windows/` folder.

2. Build the frontend by running:

```cmd
build.bat
```

The script automatically detects **MSVC** or **MinGW/MSYS2** and builds `gamepad.exe`.

3. Run the application and provide your Raspberry Pi's IP address:

```cmd
gamepad.exe 192.168.1.X
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
