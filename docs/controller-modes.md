# Controller Emulation Modes

The Raspberry Pi server can emulate **two different controller profiles**. Each has different trade-offs.

---

## Comparison

| Feature | Hori Controller | Pro Controller |
|---------|--------------------|---------------------|
| HID report size | 8 bytes | 64 bytes |
| **Buttons, D-Pad, Sticks** | Yes | Yes |
| **Latency** | **Lowest** | Low |
| **Gyroscope** | **No** | **Yes** |
| **Rumble** | **No** | **Yes** |

### Legacy Mode

- The **fastest** emulation mode: 8-byte HID reports mean minimal USB overhead.
- No gyro, no rumble.

### Modern Mode

- Full 64-byte report emulation with the advanced feature path enabled.
- **Gyroscope and accelerometer** data is sent from the  client to the target console.
- **Rumble**: rumble commands from the console are forwarded back to the client.

---

## Gyro & Rumble Requirements

Gyro and rumble require **both** server and client support:

| Side | Requirements |
|------|--------------|
| **Server** | Run the default **pro controller mode** |
| **Client** | Must use the extended UDP protocol |
| **Controller** | Must support gyro (DS4, DualSense, compatible USB motion pads) or have rumble motors |
> Pro controllers connected to android clients cannot send gyro and recieve rumble. This is a hardware limit from android.

Platform-specific gyro support:

- **Desktop clients:** SDL3 `SDL_GetGamepadSensorData` (gyro-capable controllers on Windows, Linux, and macOS)
- **Android:** Device sensors via `SensorManager` and `Sensor.TYPE_GAME_ROTATION_VECTOR` / `TYPE_GYROSCOPE`
- **iOS:** Device sensors via `CMDeviceMotion` (`CMRotationRate` and `CMAcceleration`), remapped identically to the Android pipeline
- **Browser:** Gamepad API (`pose.angularVelocity`, `pose.linearAcceleration`)

> Gyro and rumble are **not available** in hori controller mode.

---

### Use hori controller mode when:
- You dont need gyro or rumble.
- You only need buttons, sticks, and d-pad.
- The default Pro controller mode is not working.
