# Controller Emulation Modes

The Raspberry Pi server can emulate **two different Nintendo Switch controller types**. Each has different trade-offs.

---

## Comparison

| Feature | HORI / Pokken (legacy) | Pro Controller (modern) |
|---------|------------------------|------------------------|
| HID report size | 8 bytes | 64 bytes |
| USB vendor | HORI CO., LTD. | Nintendo Co., Ltd. |
| USB product ID | Pokken Tournament Controller | Pro Controller (0x2009) |
| **Buttons, D-Pad, Sticks** | Yes | Yes |
| **Latency** | **Lowest** | Low |
| **Gyroscope (6-axis IMU)** | **No** | **Yes** |
| **Accelerometer** | **No** | **Yes** |
| **Rumble** | **No** | **Yes** (bidirectional) |
| **Macros** | **Yes** | **Yes** |
| SPI flash / calibration | No | Yes |
| Built-in gadget setup | Yes | Yes |

### HORI Mode

- The **fastest** emulation mode — 8-byte HID reports mean minimal USB overhead.
- No gyro, no rumble.
- Macros are fully supported (both backends include the macro engine).
- Ideal for competitive play where only buttons and sticks matter.

### Pro Controller Mode

- Full Nintendo Switch Pro Controller emulation with **all features**.
- **Gyroscope and accelerometer** data is relayed from the PC client to the Switch. Compatible with gyro-aiming games (Splatoon, etc.).
- **Rumble** is bidirectional: rumble commands from the Switch are forwarded back to the PC client.
- **Server-side macros** let you upload and replay button sequences (both backends support macros).

---

## Gyro & Rumble Requirements

Gyro and rumble require **both** server and client support:

| Side | Requirements |
|------|-------------|
| **Server** | Must be compiled with the **Pro Controller backend** |
| **PC Client** | Must use the **extended UDP protocol** (default; disable with `--legacy`) |
| **PC Controller** | Must support gyro (DS4, DualSense, Switch Pro Controller via USB) or have rumble motors |

Platform-specific gyro support:
- **Windows:** Raw HID for DS4, DualSense, and Switch Pro Controller
- **Linux:** SDL3 `SDL_GetGamepadSensorData` (gyro-capable controllers)
- **macOS:** GameController.framework motion data
- **Browser:** Gamepad API (`pose.angularVelocity`, `pose.linearAcceleration`)

> ⚠️ Gyro and rumble are **not available** in HORI mode or when the client uses the `--legacy` flag.

---

## Choosing a Backend

### Use HORI mode when:
- You only need basic buttons, D-Pad, and analog sticks (macros still work in either mode).
- Lowest possible latency is your top priority.
- You don't need gyro or rumble.

### Use Pro Controller mode when:
- You want **gyro aiming** in supported games.
- You want **Rumble** feedback on your PC controller.
- You want player LEDs to show active ports.

---

## Building

The backend is selected at compile time in `server/rpi/CMakeLists.txt`:

```cmake
# HORI mode (default):
set(BACKEND_SOURCE "src/backend/hori-main.cpp" CACHE STRING "Backend source")

# Pro Controller mode:
set(BACKEND_SOURCE "src/backend/pro-main.cpp" CACHE STRING "Backend source")
```

Or pass it on the CMake command line:

```bash
# HORI mode:
cmake -DBACKEND_SOURCE=src/backend/hori-main.cpp ..

# Pro Controller mode:
cmake -DBACKEND_SOURCE=src/backend/pro-main.cpp ..
```

Pre-built releases include both variants — download the appropriate zip for your use case.
