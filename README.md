<p align="center">
  <img src="client/windows/icon.png" alt="Icon" width="128" height="128">
</p>

# 🎮 Nintendo Switch PC Control 

**Control your Nintendo Switch from a PC (Windows, Linux, or macOS) with low latency using a Raspberry Pi.**

This project was built from scratch in **C++** and uses **UDP** to guarantee the lowest possible latency. It's the ideal setup for playing Switch games using your PC controller, avoiding the typical lag of Bluetooth or heavy script-based solutions. 

With version 2.0.0+, clients now feature a **Graphical User Interface (GUI)** for easy connection, alongside the classic CLI.

> **📦 Pre-compiled Binaries Available!**
> You can download ready-to-use GUI/CLI clients and the Raspberry Pi server directly from the **[Releases](https://github.com/Dycool/Nintendo-Switch-PC-Control/releases)** page.

---

## 🚀 Quick Start (Pre-compiled)

**1. Raspberry Pi (Server):**
* Download `ns-pc-control-raspberry-pi.zip` to your Pi.
* Run the gadget setup: `sudo bash setup_gadget.sh`
* Start the backend: `sudo chrt -f 99 ./ns-backend`

**2. PC (Client):**
* Download the appropriate zip for your OS (Windows, Mac, or Linux).
* Launch the `ns-gui` application.
* Enter your Raspberry Pi's IP address and connect your controller!

---

## 🕹️ Controls & Shortcuts

Any **XInput-compatible controller** connected to your PC (Xbox controllers and most standard PC gamepads) can control the Nintendo Switch.

| Action  | Shortcut                                |
| ------- | --------------------------------------- |
| HOME    | Press **L3 + R3** simultaneously        |
| CAPTURE | Press **START + BACK** simultaneously   |

---

## 📚 Documentation

Detailed guides and technical information are in our `docs/` folder:

* **[Raspberry Pi System Setup](docs/raspberry-pi-setup.md)** — Enabling USB gadget mode and automating on boot.
* **[Building from Source](docs/building-from-source.md)** — Compiling the client (Windows/Mac/Linux) and server from scratch.
* **[Architecture & Security](docs/architecture.md)** — Latency optimization tips and HMAC-SHA256 protocol details.

---

## 🚀 Planned Features

* Multiple Controllers (With emulation of a GameCube adapter)

---

## 📄 License

See the repository license for details.