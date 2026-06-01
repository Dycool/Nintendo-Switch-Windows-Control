# 🎮 Nintendo Switch Windows Control

**Control your Nintendo Switch from a Windows PC with ultra-low latency using a Raspberry Pi.**

This project was built from scratch in **pure C++** and uses **UDP** to guarantee the lowest possible latency (1-2ms processing time). It's the ideal setup for playing Switch games via local *Cloud Gaming* tools (like Moonlight / Sunshine / OBS) using your PC controller, avoiding the typical lag of Bluetooth or heavy script-based solutions.

---

## 🛠️ Build and Run Tutorial

### Part 1: Raspberry Pi (Backend & USB Gadget)

1.  **Clone the repository** onto your Raspberry Pi:
    ```bash
    git clone [https://github.com/Dycool/Nintendo-Switch-Windows-Control.git](https://github.com/Dycool/Nintendo-Switch-Windows-Control.git)
    cd Nintendo-Switch-Windows-Control
    ```

2.  **Compile the backend** C++ code:
    ```bash
    mkdir build && cd build
    cmake ..
    make ns-backend
    ```

3.  **Run the USB Gadget Script**:
    This script makes the Pi pretend to be a HORI Pokken controller. You must run this *before* plugging the Pi into the Switch and *before* starting the backend.
    ```bash
    sudo bash setup_gadget.sh
    ```

4.  **Start the Backend** (in a new terminal, or run it in the background):
    ```bash
    sudo chrt -f 99 ./ns-backend
    ```
    *(The `chrt` command gives the process maximum real-time priority for lowest latency).*

5.  **Connect to Switch:** Now, plug the appropriate USB port (USB-C on Pi 4, inner Micro-USB on Pi Zero) into the Switch dock.

### Part 2: PC Windows (Frontend)

1.  **Download the source code** to your Windows PC.
2.  **Compile the frontend:** Use the provided `build.bat` script. Simply double-click it in Windows Explorer, or run it from the command line. It will automatically detect MSVC or MinGW/MSYS2 and compile the `gamepad.exe` executable.
3.  **Run the program:** Open a command prompt (CMD or PowerShell), navigate to the folder containing `gamepad.exe`, and run it by providing the IP address of your Raspberry Pi:
    ```cmd
    gamepad.exe 192.168.1.X
    ```
    *(Replace `192.168.1.X` with your Pi's actual IP address).*

Now, any XInput-compatible controller connected to your PC will control the Nintendo Switch!

**Shortcuts:**
* **HOME Button:** Press Left Stick + Right Stick simultaneously (L3 + R3).
* **CAPTURE Button:** Press START + BACK simultaneously.
