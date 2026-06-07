<p align="center">
  <img src="client/windows/icon.png" alt="Icon" width="128" height="128">
</p>

# 🎮 NS PC Control 

**Control your Console from a PC (Windows, Linux, macOS) or even through your browser and phone with low latency using a Raspberry Pi.**

This project was built from scratch in **C++** and uses **UDP** to get the lowest possible latency. It's the ideal setup for playing NS games using your PC controller, avoiding the typical lag of Bluetooth or heavy script-based solutions.

**🎮 Up to 4 players simultaneously** — Works with a single raspberry pi and a single PC.

**⌨️ Keyboard Support** — Windows and mac support keyboard controls, either through overriding P1 or with it as one player.

**🌐 Web App & Mobile Touch Controls** — The server includes an embedded web interface with a desktop control panel and touch-optimized mobile gamepad, no client install needed.

**🎮 Gyroscope and Rumble** — PC clients with gyro-capable controllers (DS4, DualSense, Switch Pro Controller) can send motion data to the Switch and receive rumble feedback. Requires the **Pro Controller** server backend.

> **📦 Pre-compiled Binaries Available!**
> You can download ready-to-use GUI/CLI clients and the Raspberry Pi server directly from the **[Releases](https://github.com/Dycool/NS-PC-Control/releases)** page.
>
> The web interface is included in the Raspberry Pi server binary — no additional files needed.



https://github.com/user-attachments/assets/aef8eb25-dd14-4335-a3f7-b1953800f856

---

## 🚀 Quick Start (Pre-compiled)

**1. Raspberry Pi (Server):**
* Download `ns-pc-control-raspberry-pi.zip` to your Pi.
* Start the backend: `sudo chrt -f 99 ./ns-backend`
  (The server handles USB gadget setup automatically — no separate script needed.)

**2. PC (Client):**
* Download the appropriate zip for your OS (Windows, Mac, or Linux).
* Launch the `ns-gui` application.
* Enter your Raspberry Pi's IP address and connect your controller(s)!
* **Up to 4 controllers** are supported simultaneously on a single PC.

**3. Web App (Optional):**
* The backend includes an embedded web server with mobile touch controls.
* Enable it with the `-w` flag: `sudo chrt -f 99 ./ns-backend -w`
* Open `http://<pi-ip>:8080` in your browser.
* See the **[Web App Guide](docs/web-app.md)** for details.

---

## 🎮 Controller Emulation Modes

The Raspberry Pi server can emulate **two different controller types**, chosen at compile time:

| Feature | HORI / Pokken (legacy) | Pro Controller (modern) |
|---------|------------------------|------------------------|
| HID report size | 8 bytes | 64 bytes |
| **Latency** | **Fastest** | Slightly larger packets |
| **Gyro** | **No** | **Yes** |
| **Rumble** | **No** | **Yes** (bidirectional, Switch → PC) |
| **Macros** | **Yes** | **Yes** |

---

## 🕹️ Controls & Shortcuts

Any **XInput-compatible controller** connected to your PC (Xbox controllers and most standard PC gamepads).

| Action  | Shortcut                                |
| ------- | --------------------------------------- |
| HOME    | Press **GUIDE** button, or **L3 + R3** simultaneously |
| CAPTURE | Press **START + BACK** simultaneously   |

---

## 📚 Documentation

Detailed guides and technical information are in our `docs/` folder:

* **[Raspberry Pi System Setup](docs/raspberry-pi-setup.md)** — Enabling USB gadget mode and automating on boot.
* **[Building from Source](docs/building-from-source.md)** — Compiling the client (Windows/Mac/Linux) and server from scratch.
* **[Controller Modes](docs/controller-modes.md)** — HORI vs Pro Controller, gyro, rumble, and how to choose.
* **[Macros](docs/macros.md)** — Recording and replaying button sequences for speedruns and TAS.
* **[Architecture & Security](docs/architecture.md)** — Latency optimization tips and HMAC-SHA256 protocol details.
* **[Web App & Mobile Controls](docs/web-app.md)** — Using the embedded web interface and mobile touch controls.

---

## 📚 References

| Component | Technology |
|---|---|
| **Linux client** | [SDL2 GameController API](https://wiki.libsdl.org/SDL2) / [GTK3](https://docs.gtk.org/gtk3/) |
| **Windows client** | [XInput](https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-input-structures) / [Winsock2](https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-reference) / [MSVC](https://visualstudio.microsoft.com/) / [MinGW](https://www.mingw-w64.org/) |
| **macOS client** | [GameController.framework](https://developer.apple.com/documentation/gamecontroller) / [Foundation](https://developer.apple.com/documentation/foundation) |
| **Raspberry Pi server** | [Linux USB Gadget (configfs / libcomposite)](https://www.kernel.org/doc/html/latest/usb/gadget_configfs.html) / UDP sockets |
| **Cryptography** | [HMAC-SHA256](https://datatracker.ietf.org/doc/html/rfc4868) (standalone C++ implementation) |
| **Protocol** | Custom UDP-based protocol with magic/version/sequence number guards |

---

## 🐛 Reporting Issues

Found a bug or have a feature request? Open an issue at **[github.com/Dycool/NS-PC-Control/issues](https://github.com/Dycool/NS-PC-Control/issues)** with as much detail as possible (OS, controller model, reproduction steps).

---

## 📄 License

See the repository license for details.
