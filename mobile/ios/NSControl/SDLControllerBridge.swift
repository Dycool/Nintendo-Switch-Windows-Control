import Foundation

/// Swift wrapper around the shared C `sdl_controller` API.
/// Bridges SDL3 gamepad/sensor/haptic calls into the Swift codebase.
final class SDLControllerBridge {
    static let shared = SDLControllerBridge()

    private init() {}

    // MARK: - Lifecycle

    func start() -> Bool {
        sdl_controller_init()
    }

    func stop() {
        sdl_controller_quit()
    }

    // MARK: - Per-frame poll

    func poll() {
        sdl_controller_poll()
    }

    // MARK: - Gamepad state (0-3)

    func padConnected(_ slot: Int) -> Bool {
        sdl_controller_pad_connected(Int32(slot))
    }

    func padInput(_ slot: Int) -> SdlPadInput {
        sdl_controller_pad_input(Int32(slot))
    }

    func padMotion(_ slot: Int) -> SdlPadMotion {
        sdl_controller_pad_motion(Int32(slot))
    }

    func padRumble(_ slot: Int, low: UInt8, high: UInt8, durationMs: UInt32 = 50) {
        sdl_controller_pad_rumble(Int32(slot), low, high, durationMs)
    }

    func padStopRumble(_ slot: Int) {
        sdl_controller_pad_stop_rumble(Int32(slot))
    }

    // MARK: - Phone sensors (for touch controls)

    func phoneSensorsOpen() -> Bool {
        sdl_controller_phone_sensors_open()
    }

    func phoneSensorsClose() {
        sdl_controller_phone_sensors_close()
    }

    func phoneSensorsRead() -> SdlPadMotion {
        sdl_controller_phone_sensors_read()
    }

    // MARK: - Phone haptics (for touch controls)

    func phoneHapticOpen() -> Bool {
        sdl_controller_phone_haptic_open()
    }

    func phoneHapticClose() {
        sdl_controller_phone_haptic_close()
    }

    func phoneHapticRumble(low: UInt8, high: UInt8) {
        sdl_controller_phone_haptic_rumble(low, high)
    }
}
