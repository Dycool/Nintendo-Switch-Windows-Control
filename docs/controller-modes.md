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

### Legacy Mode

- The **fastest** emulation mode: 8-byte HID reports mean minimal USB overhead.
- No gyro, no rumble.

### Modern Mode

- Full 64-byte report emulation with the advanced feature path enabled.
- **Gyroscope and accelerometer** data is relayed from the PC client to the target console. Compatible with gyro-aiming games.
- **Rumble** is bidirectional: rumble commands from the console are forwarded back to the PC client.

---

## Gyro & Rumble Requirements

Gyro and rumble require **both** server and client support:

| Side | Requirements |
|------|--------------|
| **Server** | Run the default **modern 64-byte mode** |
| **PC Client** | Must use the extended UDP protocol |
| **PC Controller** | Must support gyro (DS4, DualSense, compatible USB motion pads) or have rumble motors |

Platform-specific gyro support:

- **Desktop clients:** SDL3 `SDL_GetGamepadSensorData` (gyro-capable controllers on Windows, Linux, and macOS)
- **Android:** Device sensors via `SensorManager` and `Sensor.TYPE_GAME_ROTATION_VECTOR` / `TYPE_GYROSCOPE`
- **iOS:** Device sensors via `CMDeviceMotion` (`CMRotationRate` and `CMAcceleration`), remapped identically to the Android pipeline
- **Browser:** Gamepad API (`pose.angularVelocity`, `pose.linearAcceleration`)

> Gyro and rumble are **not available** in legacy mode.

---

## Choosing a Backend

### Use legacy mode when:
- You prioritize absolute minimum latency over gyro/rumble.
- You only need buttons, sticks, and d-pad.

### Use modern mode (default) when:
- You want gyroscope aiming or accelerometer input.
- You want bidirectional rumble feedback.
- You want to use server-side macros.
- You want SPI flash calibration support.

