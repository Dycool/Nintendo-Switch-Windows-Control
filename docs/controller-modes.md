# Controller Emulation Modes

The Raspberry Pi server can emulate **two different controller profiles**. Each has different trade-offs.

---

## Comparison

| Feature | Legacy 8-byte mode | Modern 64-byte mode |
|---------|--------------------|---------------------|
| HID report size | 8 bytes | 64 bytes |
| USB profile | 4-port legacy gamepad | Full-report gamepad |
| USB product ID | 8-byte report device | 64-byte report device |
| **Buttons, D-Pad, Sticks** | Yes | Yes |
| **Latency** | **Lowest** | Low |
| **Gyroscope (6-axis IMU)** | **No** | **Yes** |
| **Accelerometer** | **No** | **Yes** |
| **Rumble** | **No** | **Yes** (bidirectional) |
| **Macros** | **Yes** | **Yes** |
| SPI flash / calibration | No | Yes |
| Built-in gadget setup | Yes | Yes |

### Legacy Mode

- The **fastest** emulation mode: 8-byte HID reports mean minimal USB overhead.
- No gyro, no rumble.
- Macros are fully supported.
- Ideal for competitive play where only buttons and sticks matter.

### Modern Mode

- Full 64-byte report emulation with the advanced feature path enabled.
- **Gyroscope and accelerometer** data is relayed from the PC client to the target console. Compatible with gyro-aiming games.
- **Rumble** is bidirectional: rumble commands from the console are forwarded back to the PC client.
- **Server-side macros** let you upload and replay button sequences.

---

## Gyro & Rumble Requirements

Gyro and rumble require **both** server and client support:

| Side | Requirements |
|------|--------------|
| **Server** | Run the default **modern 64-byte Pro mode** |
| **PC Client** | Must use the extended UDP protocol |
| **PC Controller** | Must support gyro (DS4, DualSense, compatible USB motion pads) or have rumble motors |

Platform-specific gyro support:
- **Windows:** Raw HID for DS4, DualSense, and compatible USB motion pads
- **Linux:** SDL3 `SDL_GetGamepadSensorData` (gyro-capable controllers)
- **macOS:** GameController.framework motion data
- **Browser:** Gamepad API (`pose.angularVelocity`, `pose.linearAcceleration`)

> Gyro and rumble are **not available** in legacy Hori mode.

---

## Choosing a Backend

### Use legacy mode when:
- You only need basic buttons, D-Pad, and analog sticks.
- Lowest possible latency is your top priority.
- You do not need gyro or rumble.

### Use modern mode when:
- You want gyro aiming in supported games.
- You want rumble feedback on your PC controller.
- You want player LEDs to show active ports.

---

## Building

There is now one backend binary:

```bash
cd server/rpi
mkdir build && cd build
cmake ..
make
```

Controller mode is selected at runtime:

```bash
# Modern Pro mode, default:
sudo ./ns-backend

# Legacy Hori mode:
sudo ./ns-backend -hori
```

Pre-built releases use the same binary for both modes.
