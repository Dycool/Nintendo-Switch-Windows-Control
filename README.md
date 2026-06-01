# 🎮 Nintendo Switch PC Control (Windows & Linux)

**Control your Nintendo Switch from a PC (Windows or Linux) with ultra-low latency using a Raspberry Pi.**

This project was built from scratch in **pure C++** and uses **UDP** to guarantee the lowest possible latency (1-2ms processing time). It's the ideal setup for playing Switch games via local *Cloud Gaming* tools (like Moonlight / Sunshine / OBS) using your PC controller, avoiding the typical lag of Bluetooth or heavy script-based solutions.

---

## 🛠️ Build and Run Tutorial

### Part 1: Raspberry Pi (Backend & USB Gadget)

1.  **Clone the repository** onto your Raspberry Pi:
    ```bash
    git clone [https://github.com/Dycool/Nintendo-Switch-PC-Control.git](https://github.com/Dycool/Nintendo-Switch-PC-Control.git)
    cd Nintendo-Switch-PC-Control
    ```

2.  **Compile the backend** C++ code:
    ```bash
    cd backend/rpi
    mkdir build && cd build
    cmake ..
    make
    ```

3.  **Run the USB Gadget Script**:
    This script makes the Pi pretend to be a HORI Pokken controller. You must run this *before* plugging the Pi into the Switch and *before* starting the backend.
    ```bash
    cd .. # Go back to backend/rpi folder
    sudo bash setup_gadget.sh
    ```

4.  **Start the Backend**:
    ```bash
    cd build
    sudo chrt -f 99 ./ns-backend
    ```
    *(The `chrt` command gives the process maximum real-time priority for lowest latency).*

5.  **Connect to Switch:** Now, plug the appropriate USB port (USB-C on Pi 4, inner Micro-USB on Pi Zero) into the Switch dock.

---

### Part 2: PC (Frontend)

Now that your Pi is ready and listening, you need to run the frontend on your PC to send inputs.

#### 🪟 For Windows
1.  **Navigate** to the `frontend/windows/` folder.
2.  **Compile the frontend:** Double-click the `build.bat` script in Windows Explorer, or run it from the command line. It will automatically detect MSVC or MinGW/MSYS2 and compile the `gamepad.exe` executable.
3.  **Run the program:** Open a command prompt (CMD or PowerShell), navigate to the folder containing the compiled executable, and run it by providing your Pi's IP address:
    ```cmd
    gamepad.exe 192.168.1.X
    ```

#### 🐧 For Linux (Ubuntu / Debian / SteamOS)
1.  **Navigate** to the `frontend/linux/` folder.
2.  **Compile the frontend:** Use `g++` (requires `build-essential`):
    ```bash
    g++ -O3 -pthread ns-gamepad.cpp -o ns-gamepad
    ```
3.  **Run the program:** Provide your Pi's IP address:
    ```bash
    ./ns-gamepad 192.168.1.X
    ```
    *(Note: You may need to run with `sudo` or add your user to the `input` group if the program cannot read your controller events).*

---

### 🕹️ Controls & Shortcuts

Any XInput-compatible controller (Xbox, standard PC gamepads) connected to your PC will now control the Nintendo Switch.

* **HOME Button:** Press Left Stick + Right Stick simultaneously (L3 + R3).
* **CAPTURE Button:** Press START + BACK simultaneously.
