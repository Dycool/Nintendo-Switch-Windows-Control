/// ns-gamepad.cpp  —  Linux frontend for the Switch wireless gamepad bridge
///
/// Uses SDL2 GameController API for cross-platform controller support.
/// Automatically detects Xbox, PlayStation, and generic controllers
/// via USB or Bluetooth — no raw joystick API needed.
///
/// Build:
///   g++ -O3 -std=c++17 ns-gamepad.cpp -o ns-gamepad -lpthread -lSDL2
///
/// Usage:
///   ./ns-gamepad <RASPBERRY_PI_IP[:PORT]>
///   ./ns-gamepad <RASPBERRY_PI_IP[:PORT]> --legacy

#include <iostream>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <atomic>
#include <csignal>
#include <string>
#include <cerrno>

#include <SDL2/SDL.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/resource.h>
#include <fcntl.h>
#include "../../server/rpi/include/sha256.h"

// Import external protocol structures (Version 4 with MultiReport + ExtendedMultiReport)
#include "../../server/rpi/include/protocol.hpp"

static constexpr uint8_t EXT_PAD_PRESENT = 0x01;

#pragma pack(push, 1)
struct ExtendedUdpPacket {
    uint32_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t seq;
    uint64_t timestamp_us;
    ns::ExtendedMultiReport report;
    uint8_t  hmac[ns::HMAC_TAG_SIZE];
};
#pragma pack(pop)

static constexpr size_t EXT_UDP_PACKET_AUTH_SIZE = 20 + sizeof(ns::ExtendedMultiReport);
static constexpr size_t EXT_UDP_PACKET_SIZE      = EXT_UDP_PACKET_AUTH_SIZE + ns::HMAC_TAG_SIZE;
static_assert(sizeof(ExtendedUdpPacket) == EXT_UDP_PACKET_SIZE, "ExtendedUdpPacket wire size mismatch");

// ─────────────────────────────────────────────────────────────────────────────
//  Shared gamepad state (SDL2)
// ─────────────────────────────────────────────────────────────────────────────

static SDL_GameController* g_pads[4] = {nullptr, nullptr, nullptr, nullptr};
static bool g_pad_accel_enabled[4] = {false, false, false, false};
static bool g_pad_gyro_enabled[4]  = {false, false, false, false};
static bool g_legacy_udp = false;

// ─────────────────────────────────────────────────────────────────────────────
//  Signal handling
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static int16_t clamp_i16_from_float(float v) {
    if (v < -32768.0f) return -32768;
    if (v >  32767.0f) return  32767;
    return (int16_t)std::lrintf(v);
}

static void set_pad_present_flag(ns::ExtendedHIDReport& r, bool present) {
    // ExtendedHIDReport wire layout starts with the 7-byte HIDReport.
    // Byte +7 is the pad-present flag used by the backend/web protocol.
    // This keeps neutral-but-connected SDL pads mapped so they can receive rumble.
    uint8_t* raw = reinterpret_cast<uint8_t*>(&r);
    if (present) raw[7] |= EXT_PAD_PRESENT;
    else         raw[7] &= (uint8_t)~EXT_PAD_PRESENT;
}

static void fill_extended_pad(ns::ExtendedHIDReport& dst,
                              const ns::HIDReport& input,
                              bool present,
                              const ns::MotionReport* motion) {
    dst.reset();
    dst.input = input;
    set_pad_present_flag(dst, present);
    if (present && motion) {
        dst.motion = *motion;
        dst.has_motion = true;
    } else {
        dst.has_motion = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Axis conversion
// ─────────────────────────────────────────────────────────────────────────────

/// Convert raw analog stick value to normalized 0-255 range with deadzone applied
uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;

    int scaled;
    if (val >= deadzone) {
        scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    } else {
        scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    }

    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

// ─────────────────────────────────────────────────────────────────────────────
//  SDL2 Discovery, Input, Sensors, Rumble
// ─────────────────────────────────────────────────────────────────────────────

static void enable_pad_sensors(int slot, SDL_GameController* pad) {
    g_pad_accel_enabled[slot] = false;
    g_pad_gyro_enabled[slot] = false;

#if SDL_VERSION_ATLEAST(2, 0, 14)
    if (SDL_GameControllerHasSensor(pad, SDL_SENSOR_ACCEL)) {
        if (SDL_GameControllerSetSensorEnabled(pad, SDL_SENSOR_ACCEL, SDL_TRUE) == 0)
            g_pad_accel_enabled[slot] = true;
    }
    if (SDL_GameControllerHasSensor(pad, SDL_SENSOR_GYRO)) {
        if (SDL_GameControllerSetSensorEnabled(pad, SDL_SENSOR_GYRO, SDL_TRUE) == 0)
            g_pad_gyro_enabled[slot] = true;
    }

    if (g_pad_accel_enabled[slot] || g_pad_gyro_enabled[slot]) {
        std::cout << "  motion enabled:"
                  << (g_pad_accel_enabled[slot] ? " accel" : "")
                  << (g_pad_gyro_enabled[slot] ? " gyro" : "")
                  << "\n";
    }
#else
    (void)slot;
    (void)pad;
#endif
}

void scan_for_gamepads() {
    static uint64_t last_scan = 0;
    uint64_t now = ns::now_us();

    SDL_GameControllerUpdate();

    if (now - last_scan < 1'000'000) return;
    last_scan = now;

    static bool no_controllers_printed = false;
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num; ++i) {
        if (!SDL_IsGameController(i)) continue;

        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool found = false;
        for (int p = 0; p < 4; ++p) {
            if (g_pads[p]) {
                SDL_Joystick* js = SDL_GameControllerGetJoystick(g_pads[p]);
                if (js && SDL_JoystickInstanceID(js) == id) {
                    found = true;
                    break;
                }
            }
        }
        if (found) continue;

        for (int p = 0; p < 4; ++p) {
            if (!g_pads[p]) {
                SDL_GameController* pad = SDL_GameControllerOpen(i);
                if (!pad) break;
                g_pads[p] = pad;
                const char* name = SDL_GameControllerName(pad);
                std::cout << "Mapped '" << (name ? name : "Unknown") << "' to local slot P" << (p + 1) << "\n";
                enable_pad_sensors(p, pad);
                break;
            }
        }
    }
    if (num == 0) {
        if (!no_controllers_printed) {
            std::cout << "No controllers detected — waiting for connections...\n";
            no_controllers_printed = true;
        }
    } else {
        no_controllers_printed = false;
    }
}

void read_pad(int index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    SDL_GameController* pad = g_pads[index];
    if (!pad) { conn = false; return; }

    if (!SDL_GameControllerGetAttached(pad)) {
        std::cout << "Controller in slot P" << (index + 1) << " disconnected.\n";
        SDL_GameControllerClose(pad);
        g_pads[index] = nullptr;
        g_pad_accel_enabled[index] = false;
        g_pad_gyro_enabled[index] = false;
        conn = false;
        return;
    }

    conn = true;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) rep.buttons |= ns::BTN_B;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) rep.buttons |= ns::BTN_A;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) rep.buttons |= ns::BTN_Y;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) rep.buttons |= ns::BTN_X;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) rep.buttons |= ns::BTN_L;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) rep.buttons |= ns::BTN_R;

    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16000) rep.buttons |= ns::BTN_ZL;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) rep.buttons |= ns::BTN_ZR;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK))    rep.buttons |= ns::BTN_MINUS;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START))   rep.buttons |= ns::BTN_PLUS;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK))  rep.buttons |= ns::BTN_LSTICK;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) rep.buttons |= ns::BTN_RSTICK;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_GUIDE))   rep.buttons |= ns::BTN_HOME;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
        rep.buttons |= ns::BTN_HOME; rep.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
    }
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) {
        rep.buttons |= ns::BTN_CAPTURE; rep.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);
    }

    // D-Pad
    bool up    = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
    bool down  = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    bool left  = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    bool right = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    rep.hat = ns::HAT_NEUTRAL;
    if (up && right) rep.hat = ns::HAT_NE; else if (up && left) rep.hat = ns::HAT_NW;
    else if (down && right) rep.hat = ns::HAT_SE; else if (down && left) rep.hat = ns::HAT_SW;
    else if (up) rep.hat = ns::HAT_N; else if (down) rep.hat = ns::HAT_S;
    else if (left) rep.hat = ns::HAT_W; else if (right) rep.hat = ns::HAT_E;

    // Analog sticks — SDL sets UP as negative, and this protocol uses 0 as up.
    int16_t lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    int16_t rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
    int16_t ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);

    rep.lx = apply_deadzone(lx, false);
    rep.ly = apply_deadzone(ly, false);
    rep.rx = apply_deadzone(rx, false);
    rep.ry = apply_deadzone(ry, false);
}

static bool read_pad_motion(int index, ns::MotionReport& motion) {
    motion.reset();
    SDL_GameController* pad = g_pads[index];
    if (!pad) return false;

#if SDL_VERSION_ATLEAST(2, 0, 14)
    bool any = false;

    if (g_pad_accel_enabled[index]) {
        float accel[3] = {0, 0, 0};
        if (SDL_GameControllerGetSensorData(pad, SDL_SENSOR_ACCEL, accel, 3) == 0) {
            // SDL acceleration is m/s^2.  Backend calibration uses roughly 4096 units per 1g.
            constexpr float ACCEL_SCALE = 4096.0f / 9.80665f;
            motion.ax = clamp_i16_from_float(accel[0] * ACCEL_SCALE);
            motion.ay = clamp_i16_from_float(accel[1] * ACCEL_SCALE);
            motion.az = clamp_i16_from_float(accel[2] * ACCEL_SCALE);
            any = true;
        }
    }

    if (g_pad_gyro_enabled[index]) {
        float gyro[3] = {0, 0, 0};
        if (SDL_GameControllerGetSensorData(pad, SDL_SENSOR_GYRO, gyro, 3) == 0) {
            // SDL gyro is rad/s.  Keep the scale conservative, matching the macOS client.
            motion.gx = clamp_i16_from_float(gyro[0] * 1000.0f);
            motion.gy = clamp_i16_from_float(gyro[1] * 1000.0f);
            motion.gz = clamp_i16_from_float(gyro[2] * 1000.0f);
            any = true;
        }
    }

    return any;
#else
    (void)index;
    return false;
#endif
}

static void set_pad_rumble(int index, uint8_t low, uint8_t high, uint32_t duration_ms) {
    if (index < 0 || index >= 4) return;
    SDL_GameController* pad = g_pads[index];
    if (!pad) return;

#if SDL_VERSION_ATLEAST(2, 0, 9)
    if (!SDL_GameControllerGetAttached(pad)) return;
    if (!SDL_GameControllerHasRumble(pad)) return;

    uint16_t low16  = (uint16_t)low  * 257u;
    uint16_t high16 = (uint16_t)high * 257u;
    if (SDL_GameControllerRumble(pad, low16, high16, duration_ms) != 0 && (low || high)) {
        static bool warned = false;
        if (!warned) {
            std::cerr << "SDL rumble failed: " << SDL_GetError() << "\n";
            warned = true;
        }
    }
#else
    (void)low;
    (void)high;
    (void)duration_ms;
#endif
}

class RumbleManager {
public:
    void apply_packet(const ns::RumblePacket& rp, const int controller_for_slot[4]) {
        if (rp.subpad >= 4) return;
        const int slot = rp.subpad;
        const uint8_t low = rp.low_freq;
        const uint8_t high = rp.high_freq;
        const bool neutral = (low == 0 && high == 0) || rp.duration_10ms == 0;
        const uint64_t now = ns::now_us();
        const uint64_t dur_us = neutral ? 0ULL : std::max<uint64_t>(250000ULL, (uint64_t)rp.duration_10ms * 10000ULL);

        if (!neutral && states[slot].low == low && states[slot].high == high &&
            now - states[slot].last_set_us < 100000ULL) {
            states[slot].until_us = now + dur_us;
            return;
        }

        states[slot].low = low;
        states[slot].high = high;
        states[slot].until_us = neutral ? 0ULL : now + dur_us;
        states[slot].last_set_us = now;
        set_output(slot, neutral ? 0 : low, neutral ? 0 : high, controller_for_slot[slot]);
    }

    void update_timeouts(const int controller_for_slot[4]) {
        const uint64_t now = ns::now_us();
        for (int i = 0; i < 4; ++i) {
            if (states[i].until_us != 0 && now > states[i].until_us) {
                states[i].until_us = 0;
                states[i].low = states[i].high = 0;
                set_output(i, 0, 0, controller_for_slot[i]);
            }
        }
    }

private:
    struct SlotState {
        uint8_t low = 0, high = 0;
        uint64_t until_us = 0;
        uint64_t last_set_us = 0;
        int last_controller = -1;
    } states[4];

    void set_output(int slot, uint8_t low, uint8_t high, int pad_idx) {
        if (states[slot].last_controller != -1 && states[slot].last_controller != pad_idx)
            set_pad_rumble(states[slot].last_controller, 0, 0, 0);
        if (pad_idx >= 0)
            set_pad_rumble(pad_idx, low, high, (low || high) ? 250 : 0);
        states[slot].last_controller = pad_idx;
    }
};

static void pump_udp_rumble(int sock, RumbleManager& rumble, const int controller_for_slot[4]) {
    uint8_t buf[64];
    for (;;) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                std::cerr << "UDP receive error: " << strerror(errno) << "\n";
            break;
        }
        if (n == (ssize_t)sizeof(ns::RumblePacket)) {
            ns::RumblePacket rp{};
            memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::RUMBLE_MAGIC)
                rumble.apply_packet(rp, controller_for_slot);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    std::string host;
    int port = ns::DEFAULT_PORT;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--legacy") == 0) {
            g_legacy_udp = true;
        } else if (host.empty()) {
            host = argv[i];
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            return 1;
        }
    }

    if (host.empty()) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [--legacy]\n";
        std::cerr << "  --legacy  Send old input-only UDP packets; disables UDP rumble/gyro\n";
        return 1;
    }

    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        port = std::atoi(host.c_str() + colon + 1);
        if (port < 1 || port > 65535) {
            std::cerr << "Invalid port: " << port << " (must be 1–65535)\n";
            return 1;
        }
        host.resize(colon);
    }

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create UDP socket.\n";
        return 1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    // Resolve address
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    char port_str[8]; snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        std::cerr << "Cannot resolve address: " << host << "\n";
        close(sock); return 1;
    }

    sockaddr_in dest{};
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    // Initialise SDL2 GameController subsystem
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    Uint32 sdl_flags = SDL_INIT_GAMECONTROLLER;
#ifdef SDL_INIT_SENSOR
    sdl_flags |= SDL_INIT_SENSOR;
#endif
#ifdef SDL_INIT_HAPTIC
    sdl_flags |= SDL_INIT_HAPTIC;
#endif
    if (SDL_Init(sdl_flags) < 0) {
        std::cerr << "Failed to initialise SDL2: " << SDL_GetError() << "\n";
        close(sock); return 1;
    }

    if (g_legacy_udp)
        std::cout << "Legacy UDP mode: input only. UDP rumble and gyro are disabled.\n";
    else
        std::cout << "Extended UDP mode: SDL rumble replies + SDL sensor gyro enabled where supported.\n";
    std::cout << "Started... Press Ctrl+C to stop\n";

    // Elevate process priority for low-latency input reading
    setpriority(PRIO_PROCESS, 0, -20);

    uint32_t seq = 0;
    auto next_tick = std::chrono::steady_clock::now();
    RumbleManager rumble;

    // ── Main Loop (Input Polling & UDP Networking) ────────────────────────────
    while (g_running.load(std::memory_order_relaxed)) {

        while (std::chrono::steady_clock::now() < next_tick) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }

        // 1. Scan for newly plugged controllers
        scan_for_gamepads();

        // 2. Read active controllers, preserving fixed slot assignment
        ns::HIDReport reports[4];
        ns::MotionReport motions[4];
        bool present[4] = {false, false, false, false};
        bool has_motion[4] = {false, false, false, false};
        int controller_for_slot[4] = {-1, -1, -1, -1};
        for (int i = 0; i < 4; ++i) {
            reports[i].reset();
            motions[i].reset();
        }

        int active_count = 0;
        for (int i = 0; i < 4; ++i) {
            bool is_conn = false;
            read_pad(i, reports[i], is_conn);
            if (is_conn) {
                present[i] = true;
                controller_for_slot[i] = i;
                has_motion[i] = read_pad_motion(i, motions[i]);
                active_count++;
            }
        }

        // 3. Transmit to Server
        if (g_legacy_udp) {
            ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet));
            pkt.magic   = ns::PROTO_MAGIC;
            pkt.version = ns::PROTO_VERSION;
            pkt.flags   = ns::FLAG_NONE;
            pkt.seq     = seq++;
            pkt.ts_us   = ns::now_us();
            pkt.report.reset();

            ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
            for (int i = 0; i < 4; ++i)
                *out_reports[i] = reports[i];

            uint8_t full_hmac[32];
            hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);

            sendto(sock, (const char*)&pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
        } else {
            ExtendedUdpPacket pkt; memset(&pkt, 0, sizeof(pkt));
            pkt.magic        = ns::PROTO_MAGIC;
            pkt.version      = ns::PROTO_VERSION;
            pkt.flags        = ns::FLAG_NONE;
            pkt.seq          = seq++;
            pkt.timestamp_us = ns::now_us();
            pkt.report.reset();

            ns::ExtendedHIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
            for (int i = 0; i < 4; ++i)
                fill_extended_pad(*out_reports[i], reports[i], present[i], has_motion[i] ? &motions[i] : nullptr);

            uint8_t full_hmac[32];
            hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, EXT_UDP_PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);

            sendto(sock, (const char*)&pkt, (int)sizeof(pkt), 0, (struct sockaddr*)&dest, sizeof(dest));

            pump_udp_rumble(sock, rumble, controller_for_slot);
            rumble.update_timeouts(controller_for_slot);
        }

        // Dynamic throttling: 250Hz when active, light keepalive when idle.
        if (active_count > 0) next_tick += std::chrono::milliseconds(4);
        else next_tick += std::chrono::milliseconds(50);
    }

    // ── Graceful shutdown ──────────────────────────────────────────────────────
    std::cout << "\nShutting down...\n";
    for (int i = 0; i < 4; ++i) {
        if (g_pads[i]) {
            set_pad_rumble(i, 0, 0, 0);
            SDL_GameControllerClose(g_pads[i]);
            g_pads[i] = nullptr;
        }
    }
    SDL_Quit();
    close(sock);
    return 0;
}
