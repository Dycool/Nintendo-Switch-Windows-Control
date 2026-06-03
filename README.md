<p align="center">
  <img src="client/windows/icon.png" alt="Icon" width="128" height="128">
</p>

# 🎮 NS PC Control 

**Control your Console from a PC (Windows, Linux, or macOS) with low latency using a Raspberry Pi.**

This project was built from scratch in **C++** and uses **UDP** to get the lowest possible latency. It's the ideal setup for playing NS games using your PC controller, avoiding the typical lag of Bluetooth or heavy script-based solutions.

**🎮 Up to 4 players simultaneously** — Works with a single raspberry pi and a single PC.

> **📦 Pre-compiled Binaries Available!**
> You can download ready-to-use GUI/CLI clients and the Raspberry Pi server directly from the **[Releases](https://github.com/Dycool/NS-PC-Control/releases)** page.

<video src="docs/demo.mp4" width="100%" controls autoplay muted loop></video>
---

## 🚀 Quick Start (Pre-compiled)

**1. Raspberry Pi (Server):**
* Download `ns-pc-control-raspberry-pi.zip` to your Pi.
* Run the gadget setup: `sudo bash setup_gadget.sh`
* Start the backend: `sudo chrt -f 99 ./ns-backend`

**2. PC (Client):**
* Download the appropriate zip for your OS (Windows, Mac, or Linux).
* Launch the `ns-gui` application.
* Enter your Raspberry Pi's IP address and connect your controller(s)!
* **Up to 4 controllers** are supported simultaneously on a single PC.

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
* **[Architecture & Security](docs/architecture.md)** — Latency optimization tips and HMAC-SHA256 protocol details.

---

## 🚀 Planned Features

* Keyboard support

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
