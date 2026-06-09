#include "sdl_controller.h"
#include "ns_protocol.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PADS SDL_CONTROLLER_MAX_PADS
#define SCAN_INTERVAL_US 500000ULL

static struct {
    int initialized;

    SDL_Gamepad* pads[MAX_PADS];
    SDL_JoystickID ids[MAX_PADS];
    int pad_count;

    SdlPadInput pad_inputs[MAX_PADS];
    SdlPadMotion pad_motions[MAX_PADS];

    uint64_t last_scan_us;

    SDL_Sensor* phone_accel;
    SDL_Sensor* phone_gyro;
    int phone_sensors_open;

    SDL_Haptic* phone_haptic;
    int phone_haptic_open;
} C;

// ─── helpers ───────────────────────────────────────────────

static float axis_to_float(int16_t v) {
    if (v > -1024 && v < 1024) return 0.0f;
    return v < 0 ? v / 32768.0f : v / 32767.0f;
}

static int16_t gyro_deadzone_i16(int16_t v) {
    if (v > -8 && v < 8) return 0;
    return v;
}

static uint16_t motor_word(uint8_t v) {
    return (uint16_t)((uint32_t)v * 65535u / 255u);
}

static int has_native_home_capture(uint16_t vid, const char* name) {
    if (vid == 0x057E) return 1; // Nintendo
    (void)name;
    return 0;
}

// ─── scanning ──────────────────────────────────────────────

static int find_free_slot(void) {
    for (int i = 0; i < MAX_PADS; ++i) {
        if (!C.pads[i]) return i;
    }
    return -1;
}

static int slot_for_id(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; ++i) {
        if (C.ids[i] == id && C.pads[i]) return i;
    }
    return -1;
}

static void close_pad(int slot) {
    if (C.pads[slot]) {
        SDL_RumbleGamepad(C.pads[slot], 0, 0, 0);
        SDL_RumbleGamepadTriggers(C.pads[slot], 0, 0, 0);
        SDL_CloseGamepad(C.pads[slot]);
        C.pads[slot] = NULL;
        C.ids[slot] = 0;
        memset(&C.pad_inputs[slot], 0, sizeof(C.pad_inputs[slot]));
        memset(&C.pad_motions[slot], 0, sizeof(C.pad_motions[slot]));
        --C.pad_count;
    }
}

static void scan_gamepads(void) {
    C.last_scan_us = SDL_GetTicks() * 1000ULL;

    for (int i = 0; i < MAX_PADS; ++i) {
        if (C.pads[i] && !SDL_GamepadConnected(C.pads[i])) {
            close_pad(i);
        }
    }

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;

    for (int i = 0; i < count; ++i) {
        SDL_JoystickID id = ids[i];
        if (slot_for_id(id) >= 0) continue;
        int slot = find_free_slot();
        if (slot < 0) break;

        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad) continue;

        C.pads[slot] = pad;
        C.ids[slot] = SDL_GetGamepadID(pad);
        ++C.pad_count;

        // Enable sensors if available
        if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
            SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
        }
        if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
            SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
        }
    }
    SDL_free(ids);
}

// ─── motion conversion ─────────────────────────────────────

static void convert_motion(const float accel[3], const float gyro[3],
                           int16_t* ax, int16_t* ay, int16_t* az,
                           int16_t* gx, int16_t* gy, int16_t* gz) {
    const float accel_scale = ns_accel_scale_android();
    const float gyro_scale = ns_gyro_scale();
    *ax = ns_clamp_motion(-accel[0] * accel_scale);
    *ay = ns_clamp_motion(-accel[2] * accel_scale);
    *az = ns_clamp_motion( accel[1] * accel_scale);
    *gx = gyro_deadzone_i16(ns_clamp_motion(-gyro[0] * gyro_scale));
    *gy = gyro_deadzone_i16(ns_clamp_motion(-gyro[2] * gyro_scale));
    *gz = gyro_deadzone_i16(ns_clamp_motion( gyro[1] * gyro_scale));
}

static void read_pad_motion(SDL_Gamepad* pad, SdlPadMotion* out) {
    memset(out, 0, sizeof(*out));
    float accel[3] = {0, 0, 0};
    float gyro[3] = {0, 0, 0};
    int has_accel = SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3);
    int has_gyro = SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gyro, 3);
    if (has_accel || has_gyro) {
        out->has_motion = 1;
        convert_motion(accel, gyro, &out->ax, &out->ay, &out->az,
                       &out->gx, &out->gy, &out->gz);
    }
}

// ─── input mapping ─────────────────────────────────────────

static void read_pad_input(SDL_Gamepad* pad, SdlPadInput* out) {
    memset(out, 0, sizeof(*out));
    out->connected = 1;

    uint16_t b = 0;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) b |= NS_BTN_B;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST))  b |= NS_BTN_A;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST))  b |= NS_BTN_Y;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH)) b |= NS_BTN_X;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  b |= NS_BTN_L;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= NS_BTN_R;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  > 16384) b |= NS_BTN_ZL;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) b |= NS_BTN_ZR;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK))       b |= NS_BTN_MINUS;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START))      b |= NS_BTN_PLUS;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK))  b |= NS_BTN_LSTICK;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) b |= NS_BTN_RSTICK;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE))      b |= NS_BTN_HOME;
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_MISC1))      b |= NS_BTN_CAPTURE;

    // Combo shortcuts for non-Nintendo controllers
    uint16_t vid = SDL_GetGamepadVendor(pad);
    const char* name = SDL_GetGamepadName(pad);
    if (!has_native_home_capture(vid, name)) {
        // L3 + R3 -> Home
        if ((b & NS_BTN_LSTICK) && (b & NS_BTN_RSTICK)) {
            b |= NS_BTN_HOME;
            b &= ~(NS_BTN_LSTICK | NS_BTN_RSTICK);
        }
        // Minus + Plus -> Capture
        if ((b & NS_BTN_MINUS) && (b & NS_BTN_PLUS)) {
            b |= NS_BTN_CAPTURE;
            b &= ~(NS_BTN_MINUS | NS_BTN_PLUS);
        }
    }

    out->buttons = b;
    out->dpad_up    = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    out->dpad_down  = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    out->dpad_left  = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    out->dpad_right = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

    out->lx = axis_to_float(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
    out->ly = axis_to_float(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    out->rx = axis_to_float(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
    out->ry = axis_to_float(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
}

// ─── public API ────────────────────────────────────────────

int sdl_controller_init(void) {
    if (C.initialized) return 1;

    SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_SWITCH", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_JOY_CONS", "1");
    SDL_SetHint("SDL_JOYSTICK_ENHANCED_REPORTS", "1");

    Uint32 flags = SDL_INIT_GAMEPAD | SDL_INIT_EVENTS | SDL_INIT_SENSOR | SDL_INIT_HAPTIC;
    if (!SDL_Init(flags)) {
        return 0;
    }

    memset(&C, 0, sizeof(C));
    C.initialized = 1;
    return 1;
}

void sdl_controller_quit(void) {
    if (!C.initialized) return;
    sdl_controller_phone_haptic_close();
    sdl_controller_phone_sensors_close();
    for (int i = 0; i < MAX_PADS; ++i) close_pad(i);
    SDL_Quit();
    memset(&C, 0, sizeof(C));
}

void sdl_controller_poll(void) {
    if (!C.initialized) return;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_GAMEPAD_ADDED ||
            ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
            C.last_scan_us = 0; // force rescan
        }
    }

    SDL_UpdateGamepads();
    SDL_UpdateSensors();

    uint64_t now = SDL_GetTicks() * 1000ULL;
    if (C.last_scan_us == 0 || now - C.last_scan_us > SCAN_INTERVAL_US) {
        scan_gamepads();
    }

    for (int i = 0; i < MAX_PADS; ++i) {
        if (C.pads[i]) {
            read_pad_input(C.pads[i], &C.pad_inputs[i]);
            read_pad_motion(C.pads[i], &C.pad_motions[i]);
        } else {
            memset(&C.pad_inputs[i], 0, sizeof(C.pad_inputs[i]));
            memset(&C.pad_motions[i], 0, sizeof(C.pad_motions[i]));
        }
    }
}

int sdl_controller_pad_connected(int slot) {
    if (slot < 0 || slot >= MAX_PADS) return 0;
    return C.pads[slot] && SDL_GamepadConnected(C.pads[slot]) ? 1 : 0;
}

SdlPadInput sdl_controller_pad_input(int slot) {
    SdlPadInput z = {0};
    if (slot < 0 || slot >= MAX_PADS) return z;
    return C.pad_inputs[slot];
}

SdlPadMotion sdl_controller_pad_motion(int slot) {
    SdlPadMotion z = {0};
    if (slot < 0 || slot >= MAX_PADS) return z;
    return C.pad_motions[slot];
}

void sdl_controller_pad_rumble(int slot, uint8_t low, uint8_t high, uint32_t duration_ms) {
    if (slot < 0 || slot >= MAX_PADS || !C.pads[slot]) return;
    if (!SDL_GamepadConnected(C.pads[slot])) return;

    uint16_t lw = motor_word(low);
    uint16_t hw = motor_word(high);
    int stop = (lw == 0 && hw == 0) || duration_ms == 0;

    (void)SDL_RumbleGamepad(C.pads[slot], stop ? 0 : lw, stop ? 0 : hw, duration_ms);
    (void)SDL_RumbleGamepadTriggers(C.pads[slot], stop ? 0 : lw, stop ? 0 : hw, duration_ms);
}

void sdl_controller_pad_stop_rumble(int slot) {
    sdl_controller_pad_rumble(slot, 0, 0, 0);
}

// ─── phone sensors ─────────────────────────────────────────

int sdl_controller_phone_sensors_open(void) {
    if (C.phone_sensors_open) return 1;

    int count = 0;
    SDL_SensorID* ids = SDL_GetSensors(&count);
    if (!ids) return 0;

    for (int i = 0; i < count; ++i) {
        SDL_Sensor* s = SDL_OpenSensorDeviceInstanceID(ids[i]);
        if (!s) continue;
        int type = SDL_GetSensorType(s);
        if (type == SDL_SENSOR_ACCEL && !C.phone_accel) {
            C.phone_accel = s;
        } else if (type == SDL_SENSOR_GYRO && !C.phone_gyro) {
            C.phone_gyro = s;
        } else {
            SDL_CloseSensor(s);
        }
    }
    SDL_free(ids);

    C.phone_sensors_open = (C.phone_accel != NULL || C.phone_gyro != NULL) ? 1 : 0;
    return C.phone_sensors_open;
}

void sdl_controller_phone_sensors_close(void) {
    if (C.phone_accel) { SDL_CloseSensor(C.phone_accel); C.phone_accel = NULL; }
    if (C.phone_gyro)  { SDL_CloseSensor(C.phone_gyro);  C.phone_gyro = NULL; }
    C.phone_sensors_open = 0;
}

SdlPadMotion sdl_controller_phone_sensors_read(void) {
    SdlPadMotion m = {0};
    if (!C.phone_sensors_open) return m;

    float accel[3] = {0, 0, 0};
    float gyro[3] = {0, 0, 0};
    int has_accel = C.phone_accel ? SDL_GetSensorData(C.phone_accel, accel, 3) : 0;
    int has_gyro  = C.phone_gyro  ? SDL_GetSensorData(C.phone_gyro, gyro, 3) : 0;

    if (has_accel || has_gyro) {
        m.has_motion = 1;
        convert_motion(accel, gyro, &m.ax, &m.ay, &m.az, &m.gx, &m.gy, &m.gz);
    }
    return m;
}

// ─── phone haptics ─────────────────────────────────────────

int sdl_controller_phone_haptic_open(void) {
    if (C.phone_haptic_open) return 1;
    C.phone_haptic = SDL_OpenHaptic(0);
    C.phone_haptic_open = (C.phone_haptic != NULL) ? 1 : 0;
    return C.phone_haptic_open;
}

void sdl_controller_phone_haptic_close(void) {
    if (C.phone_haptic) {
        SDL_PlayHapticRumble(C.phone_haptic, 0.0, 0, 0);
        SDL_CloseHaptic(C.phone_haptic);
        C.phone_haptic = NULL;
    }
    C.phone_haptic_open = 0;
}

void sdl_controller_phone_haptic_rumble(uint8_t low, uint8_t high) {
    if (!C.phone_haptic) return;
    if (low == 0 && high == 0) {
        SDL_PlayHapticRumble(C.phone_haptic, 0.0, 0, 0);
        return;
    }
    float strength = (float)((int)low + (int)high) / 510.0f;
    if (strength > 1.0f) strength = 1.0f;
    SDL_PlayHapticRumble(C.phone_haptic, strength, 50);
}
