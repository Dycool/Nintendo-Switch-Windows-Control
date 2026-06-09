#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
using SOCKET = int;
static constexpr SOCKET INVALID_SOCKET = -1;
static constexpr int SOCKET_ERROR = -1;
static int closesocket(SOCKET s) { return close(s); }
#endif

#include <limits.h>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <mach-o/dyld.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <QApplication>
#include <QComboBox>
#include <QCloseEvent>
#include <QDialog>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../server/include/sha256.h"
#include "../server/include/protocol.hpp"
#include "../server/include/macros.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#endif

static constexpr uint8_t EXT_PAD_PRESENT = 0x01;
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
struct ExtendedUdpPacket {
    uint32_t magic = ns::PROTO_MAGIC;
    uint8_t version = ns::WEB_PROTO_VERSION_3;
    uint8_t flags = ns::FLAG_NONE;
    uint16_t reserved = 0;
    uint32_t seq = 0;
    uint64_t timestamp_us = 0;
    ns::ExtendedMultiReport3 report{};
    uint8_t hmac[ns::HMAC_TAG_SIZE]{};
} NS_PACKED_ATTR;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

static constexpr size_t EXT_UDP_PACKET_AUTH_SIZE = 20 + sizeof(ns::ExtendedMultiReport3);
static constexpr size_t EXT_UDP_PACKET_SIZE = EXT_UDP_PACKET_AUTH_SIZE + ns::HMAC_TAG_SIZE;
static_assert(sizeof(ExtendedUdpPacket) == EXT_UDP_PACKET_SIZE, "ExtendedUdpPacket wire layout changed");

static void set_pad_present_flag(ns::ExtendedHIDReport3& r, bool present) {
    if (present) r.input.vendor |= EXT_PAD_PRESENT;
    else r.input.vendor &= (uint8_t)~EXT_PAD_PRESENT;
}

static void fill_extended_pad(ns::ExtendedHIDReport3& dst, const ns::HIDReport& input,
                              bool present, const ns::MotionReport motion[3] = nullptr) {
    dst.reset();
    dst.input = input;
    set_pad_present_flag(dst, present);
    if (motion) {
        dst.motion[0] = motion[0];
        dst.motion[1] = motion[1];
        dst.motion[2] = motion[2];
        dst.has_motion = 1;
    }
}

static bool set_socket_nonblocking(SOCKET sock) {
#ifdef _WIN32
    u_long nonblocking = 1;
    return ioctlsocket(sock, FIONBIO, &nonblocking) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    return flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static bool socket_would_block() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static bool resolve_udp_destination(const std::string& host, int port, sockaddr_in& dest) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_buf[8];
    std::snprintf(port_buf, sizeof(port_buf), "%d", port);
    if (getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0 || !res) return false;
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);
    return true;
}

static int send_all_udp(SOCKET sock, const sockaddr_in& dest, const void* data, size_t len) {
    return sendto(sock, reinterpret_cast<const char*>(data), (int)len, 0,
                  reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

static void send_udp_disconnect_packet(SOCKET sock, const sockaddr_in& dest,
                                       const uint8_t hmac_key[32], uint32_t seq, bool legacy_udp = false) {
    if (sock == INVALID_SOCKET) return;
    if (legacy_udp) {
        ns::Packet pkt{};
        pkt.magic = ns::PROTO_MAGIC;
        pkt.version = ns::PROTO_VERSION;
        pkt.flags = ns::FLAG_RESET | ns::FLAG_DISCONNECT;
        pkt.seq = seq;
        pkt.ts_us = ns::now_us();
        pkt.report.reset();
        uint8_t full_hmac[32];
        hmac_sha256(hmac_key, 32, reinterpret_cast<const uint8_t*>(&pkt), ns::PACKET_AUTH_SIZE, full_hmac);
        std::memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        for (int i = 0; i < 3; ++i) {
            send_all_udp(sock, dest, &pkt, ns::PACKET_SIZE);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return;
    }
    ExtendedUdpPacket pkt{};
    pkt.magic = ns::PROTO_MAGIC;
    pkt.version = ns::WEB_PROTO_VERSION;
    pkt.flags = ns::FLAG_RESET | ns::FLAG_DISCONNECT;
    pkt.seq = seq;
    pkt.timestamp_us = ns::now_us();
    pkt.report.reset();
    uint8_t full_hmac[32];
    hmac_sha256(hmac_key, 32, reinterpret_cast<const uint8_t*>(&pkt), EXT_UDP_PACKET_AUTH_SIZE, full_hmac);
    std::memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
    for (int i = 0; i < 3; ++i) {
        send_all_udp(sock, dest, &pkt, sizeof(pkt));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

static constexpr uint64_t SDL_DIGITAL_RELEASE_GRACE_US = 35000ULL;

struct DigitalReleaseFilter {
    uint16_t last_buttons = 0;
    uint64_t button_until[16]{};
    uint8_t last_hat = ns::HAT_NEUTRAL;
    uint64_t hat_until = 0;

    void reset() {
        last_buttons = 0;
        std::fill(std::begin(button_until), std::end(button_until), 0);
        last_hat = ns::HAT_NEUTRAL;
        hat_until = 0;
    }

    void apply(ns::HIDReport& r, uint64_t now) {
        for (int i = 0; i < 16; ++i) {
            uint16_t bit = (uint16_t)(1u << i);
            if (r.buttons & bit) {
                last_buttons |= bit;
                button_until[i] = now + SDL_DIGITAL_RELEASE_GRACE_US;
            } else if ((last_buttons & bit) && button_until[i] != 0 && now <= button_until[i]) {
                r.buttons |= bit;
            } else {
                last_buttons &= (uint16_t)~bit;
                button_until[i] = 0;
            }
        }

        if (r.hat != ns::HAT_NEUTRAL) {
            last_hat = r.hat;
            hat_until = now + SDL_DIGITAL_RELEASE_GRACE_US;
        } else if (hat_until != 0 && now <= hat_until) {
            r.hat = last_hat;
        } else {
            last_hat = ns::HAT_NEUTRAL;
            hat_until = 0;
        }
    }
};

struct SdlPadState {
    bool connected = false;
    ns::HIDReport input{};
    ns::MotionReport motion{};
    ns::MotionReport motion_samples[3]{};
    bool has_motion = false;
    uint64_t last_input_us = 0;
    std::string name;
    uint16_t vid = 0;
    uint16_t pid = 0;
    SDL_JoystickID instance_id = 0;
};

static uint8_t sdl_axis_to_byte(Sint16 val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled = 128;
    if (val >= deadzone) scaled = 128 + ((int)(val - deadzone) * 127) / (32767 - deadzone);
    else scaled = 128 - ((int)(-val - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

static int16_t clamp_motion_i16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)std::lround(v);
}

static int16_t gyro_deadzone_i16(int16_t v) {
    constexpr int16_t GYRO_DEADZONE = 32;
    return std::abs((int)v) <= GYRO_DEADZONE ? 0 : v;
}

class SDLInputManager {
public:
    bool start() {
        std::lock_guard<std::mutex> lk(mtx);
        if (initialized) return true;
        SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_" "SW" "ITCH", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_" "JOY" "_CONS", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_XBOX", "1");
        SDL_SetHint("SDL_JOYSTICK_ENHANCED_REPORTS", "1");
        Uint32 flags = SDL_INIT_GAMEPAD | SDL_INIT_EVENTS;
#ifdef SDL_INIT_SENSOR
        flags |= SDL_INIT_SENSOR;
#endif
#ifdef SDL_INIT_HAPTIC
        flags |= SDL_INIT_HAPTIC;
#endif
        if (!SDL_Init(flags)) {
            const char* e = SDL_GetError();
            last_error = (e && *e) ? e : "SDL_Init failed";
            return false;
        }
        initialized = true;
        scan_locked(true);
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(mtx);
        if (!initialized) return;
        close_all_locked();
        clear_states_locked();
        Uint32 flags = SDL_INIT_GAMEPAD | SDL_INIT_EVENTS;
#ifdef SDL_INIT_SENSOR
        flags |= SDL_INIT_SENSOR;
#endif
#ifdef SDL_INIT_HAPTIC
        flags |= SDL_INIT_HAPTIC;
#endif
        SDL_QuitSubSystem(flags);
        initialized = false;
    }

    void poll() {
        std::lock_guard<std::mutex> lk(mtx);
        if (!initialized) return;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) force_scan = true;
        }
        SDL_UpdateGamepads();
        SDL_UpdateSensors();
        uint64_t now = ns::now_us();
        if (force_scan || last_scan_us == 0 || now - last_scan_us > 500000ULL) scan_locked(false);
        refresh_states_locked(now);
    }

    std::array<SdlPadState, 4> snapshot() {
        std::lock_guard<std::mutex> lk(mtx);
        return states;
    }

    std::string error() const {
        std::lock_guard<std::mutex> lk(mtx);
        return last_error;
    }

    void request_rescan() {
        std::lock_guard<std::mutex> lk(mtx);
        force_scan = true;
    }

    void set_rumble(int sdl_slot, uint8_t low, uint8_t high, uint32_t duration_ms) {
        std::lock_guard<std::mutex> lk(mtx);
        if (!initialized || sdl_slot < 0 || sdl_slot >= 4) return;
        Device* d = device_for_slot_locked(sdl_slot);
        if (!d || !d->pad || !SDL_GamepadConnected(d->pad)) return;
        const Uint16 low_word = motor_word(low);
        const Uint16 high_word = motor_word(high);
        const bool stop = (low_word == 0 && high_word == 0) || duration_ms == 0;
        bool ok_main = SDL_RumbleGamepad(d->pad, stop ? 0 : low_word, stop ? 0 : high_word, duration_ms);
        bool ok_trigger = true;
        SDL_PropertiesID props = SDL_GetGamepadProperties(d->pad);
        bool trigger_capable = props && SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
        if (trigger_capable || !ok_main || stop) {
            ok_trigger = SDL_RumbleGamepadTriggers(d->pad, stop ? 0 : low_word, stop ? 0 : high_word, duration_ms);
        }
        if (!stop && !ok_main && !ok_trigger) {
            const char* e = SDL_GetError();
            last_error = (e && *e) ? e : "SDL rumble failed";
        }
    }

    void stop_all_rumble() {
        std::lock_guard<std::mutex> lk(mtx);
        stop_all_rumble_locked();
    }

private:
    struct Device {
        SDL_Gamepad* pad = nullptr;
        SDL_JoystickID id = 0;
        int slot = -1;
        bool gyro_enabled = false;
        bool accel_enabled = false;
        bool rumble_capable = false;
        bool trigger_rumble_capable = false;
        std::string name;
        uint16_t vid = 0;
        uint16_t pid = 0;
        ns::MotionReport motion_samples[3]{};
        bool has_motion_samples = false;
    };

    mutable std::mutex mtx;
    bool initialized = false;
    bool force_scan = false;
    uint64_t last_scan_us = 0;
    std::string last_error;
    std::array<SdlPadState, 4> states{};
    std::vector<Device> devices;

    static Uint16 motor_word(uint8_t v) { return (Uint16)((uint32_t)v * 65535u / 255u); }
    static bool button(SDL_Gamepad* pad, SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); }

    static std::string upper_copy(std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }

    static bool contains_upper(const std::string& haystack, const char* needle) {
        return upper_copy(haystack).find(needle) != std::string::npos;
    }

    static bool has_native_home_capture(const Device& d) {
        if (d.vid == 0x057E) return true;
        return contains_upper(d.name, "PRO CONTROLLER") || contains_upper(d.name, "JOY-CON");
    }

    static bool should_use_combo_shortcuts(const Device& d) {
        if (has_native_home_capture(d)) return false;
        if (d.vid == 0x045E) return true;
        return contains_upper(d.name, "XBOX") || contains_upper(d.name, "XINPUT") ||
               contains_upper(d.name, "DUALSHOCK") || contains_upper(d.name, "DUALSENSE");
    }

    static ns::HIDReport map_gamepad(const Device& d) {
        ns::HIDReport r;
        r.reset();
        SDL_Gamepad* pad = d.pad;
        if (!pad) return r;
        if (button(pad, SDL_GAMEPAD_BUTTON_SOUTH)) r.buttons |= ns::BTN_B;
        if (button(pad, SDL_GAMEPAD_BUTTON_EAST)) r.buttons |= ns::BTN_A;
        if (button(pad, SDL_GAMEPAD_BUTTON_WEST)) r.buttons |= ns::BTN_Y;
        if (button(pad, SDL_GAMEPAD_BUTTON_NORTH)) r.buttons |= ns::BTN_X;
        if (button(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) r.buttons |= ns::BTN_L;
        if (button(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) r.buttons |= ns::BTN_R;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) r.buttons |= ns::BTN_ZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) r.buttons |= ns::BTN_ZR;
        if (button(pad, SDL_GAMEPAD_BUTTON_BACK)) r.buttons |= ns::BTN_MINUS;
        if (button(pad, SDL_GAMEPAD_BUTTON_START)) r.buttons |= ns::BTN_PLUS;
        if (button(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) r.buttons |= ns::BTN_LSTICK;
        if (button(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) r.buttons |= ns::BTN_RSTICK;
        if (button(pad, SDL_GAMEPAD_BUTTON_GUIDE)) r.buttons |= ns::BTN_HOME;
        if (button(pad, SDL_GAMEPAD_BUTTON_MISC1)) r.buttons |= ns::BTN_CAPTURE;
        if (should_use_combo_shortcuts(d) &&
            button(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK) && button(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
            r.buttons |= ns::BTN_HOME;
            r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
        }
        if (should_use_combo_shortcuts(d) &&
            button(pad, SDL_GAMEPAD_BUTTON_BACK) && button(pad, SDL_GAMEPAD_BUTTON_START)) {
            r.buttons |= ns::BTN_CAPTURE;
            r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);
        }
        bool up = button(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        bool down = button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        bool left = button(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        bool right = button(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        if (up && right) r.hat = ns::HAT_NE;
        else if (up && left) r.hat = ns::HAT_NW;
        else if (down && right) r.hat = ns::HAT_SE;
        else if (down && left) r.hat = ns::HAT_SW;
        else if (up) r.hat = ns::HAT_N;
        else if (down) r.hat = ns::HAT_S;
        else if (left) r.hat = ns::HAT_W;
        else if (right) r.hat = ns::HAT_E;
        r.lx = sdl_axis_to_byte(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
        r.ly = sdl_axis_to_byte(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
        r.rx = sdl_axis_to_byte(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
        r.ry = sdl_axis_to_byte(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
        return r;
    }

    static bool report_non_neutral(const ns::HIDReport& r) {
        return r.buttons != 0 || r.hat != ns::HAT_NEUTRAL ||
               r.lx != 128 || r.ly != 128 || r.rx != 128 || r.ry != 128;
    }

    static void push_motion_sample(Device& d, const ns::MotionReport& sample) {
        if (!d.has_motion_samples) {
            d.motion_samples[0] = sample;
            d.motion_samples[1] = sample;
            d.motion_samples[2] = sample;
            d.has_motion_samples = true;
            return;
        }
        d.motion_samples[0] = d.motion_samples[1];
        d.motion_samples[1] = d.motion_samples[2];
        d.motion_samples[2] = sample;
    }

    static void apply_motion(Device& d, ns::MotionReport out_samples[3], bool& has_motion) {
        SDL_Gamepad* pad = d.pad;
        for (int i = 0; i < 3; ++i) out_samples[i].reset();
        has_motion = false;

        ns::MotionReport sample{};
        constexpr float STANDARD_GRAVITY = 9.80665f;
        constexpr float ACCEL_SCALE = 4096.0f / STANDARD_GRAVITY;
        constexpr float RAD_TO_DEG = 57.29577951308232f;
        constexpr float GYRO_SCALE = RAD_TO_DEG * 16.384f;

        if (d.accel_enabled) {
            float accel[3] = {0, 0, 0};
            if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3)) {
                sample.ax = clamp_motion_i16(-accel[0] * ACCEL_SCALE);
                sample.ay = clamp_motion_i16(-accel[2] * ACCEL_SCALE);
                sample.az = clamp_motion_i16(accel[1] * ACCEL_SCALE);
                has_motion = true;
            } else {
                sample.az = 4096;
            }
        } else {
            sample.az = 4096;
        }

        if (d.gyro_enabled) {
            float gyro[3] = {0, 0, 0};
            if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gyro, 3)) {
                const float gx = -gyro[0];
                const float gy = -gyro[2];
                const float gz =  gyro[1];
                sample.gx = gyro_deadzone_i16(clamp_motion_i16(gx * GYRO_SCALE));
                sample.gy = gyro_deadzone_i16(clamp_motion_i16(gy * GYRO_SCALE));
                sample.gz = gyro_deadzone_i16(clamp_motion_i16(gz * GYRO_SCALE));
                has_motion = true;
            }
        }

        if (has_motion) {
            push_motion_sample(d, sample);
            out_samples[0] = d.motion_samples[0];
            out_samples[1] = d.motion_samples[1];
            out_samples[2] = d.motion_samples[2];
        } else {
            d.has_motion_samples = false;
            for (int i = 0; i < 3; ++i) d.motion_samples[i].reset();
        }
    }

    Device* device_for_slot_locked(int slot) {
        for (auto& d : devices) if (d.slot == slot) return &d;
        return nullptr;
    }

    int first_free_slot_locked() const {
        bool used[4] = {false, false, false, false};
        for (const auto& d : devices) if (d.slot >= 0 && d.slot < 4) used[d.slot] = true;
        for (int i = 0; i < 4; ++i) if (!used[i]) return i;
        return -1;
    }

    bool has_device_locked(SDL_JoystickID id) const {
        for (const auto& d : devices) if (d.id == id) return true;
        return false;
    }

    void close_device_locked(Device& d) {
        if (d.pad) {
            SDL_RumbleGamepad(d.pad, 0, 0, 0);
            SDL_RumbleGamepadTriggers(d.pad, 0, 0, 0);
            SDL_CloseGamepad(d.pad);
            d.pad = nullptr;
        }
    }

    void close_all_locked() {
        for (auto& d : devices) close_device_locked(d);
        devices.clear();
    }

    void clear_states_locked() {
        for (auto& s : states) s = SdlPadState{};
    }

    void stop_all_rumble_locked() {
        for (auto& d : devices) {
            if (d.pad) {
                SDL_RumbleGamepad(d.pad, 0, 0, 0);
                SDL_RumbleGamepadTriggers(d.pad, 0, 0, 0);
            }
        }
    }

    void scan_locked(bool initial) {
        (void)initial;
        force_scan = false;
        last_scan_us = ns::now_us();
        for (auto it = devices.begin(); it != devices.end();) {
            if (!it->pad || !SDL_GamepadConnected(it->pad)) {
                if (it->slot >= 0 && it->slot < 4) states[it->slot] = SdlPadState{};
                close_device_locked(*it);
                it = devices.erase(it);
            } else {
                ++it;
            }
        }
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (!ids) return;
        for (int i = 0; i < count; ++i) {
            SDL_JoystickID id = ids[i];
            if (has_device_locked(id)) continue;
            int slot = first_free_slot_locked();
            if (slot < 0) break;
            SDL_Gamepad* pad = SDL_OpenGamepad(id);
            if (!pad) continue;
            Device d{};
            d.pad = pad;
            d.id = SDL_GetGamepadID(pad);
            d.slot = slot;
            const char* name = SDL_GetGamepadName(pad);
            d.name = (name && *name) ? name : "SDL3 Gamepad";
            d.vid = SDL_GetGamepadVendor(pad);
            d.pid = SDL_GetGamepadProduct(pad);
            SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
            if (props) {
                d.rumble_capable = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
                d.trigger_rumble_capable = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
            }
            if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) d.accel_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
            if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) d.gyro_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
            devices.push_back(d);
        }
        SDL_free(ids);
    }

    void refresh_states_locked(uint64_t now) {
        clear_states_locked();
        for (auto& d : devices) {
            if (!d.pad || d.slot < 0 || d.slot >= 4 || !SDL_GamepadConnected(d.pad)) continue;
            SdlPadState st{};
            st.connected = true;
            st.input = map_gamepad(d);
            st.name = d.name;
            st.vid = d.vid;
            st.pid = d.pid;
            st.instance_id = d.id;
            apply_motion(d, st.motion_samples, st.has_motion);
            st.motion = st.has_motion ? st.motion_samples[2] : ns::MotionReport{};
            if (report_non_neutral(st.input) || st.has_motion) st.last_input_us = now;
            states[d.slot] = st;
        }
    }
};

static SDLInputManager g_sdlInput;

class RumbleManager {
public:
    void apply_precision_packet(const ns::PrecisionRumblePacket& rp, const int controller_for_slot[4]) {
        if (rp.subpad >= 4) return;
        ns::RumblePacket fallback{};
        fallback.magic = ns::RUMBLE_MAGIC;
        fallback.subpad = rp.subpad;
        fallback.low_freq = rp.low_freq;
        fallback.high_freq = rp.high_freq;
        fallback.duration_10ms = rp.duration_10ms;
        apply_packet(fallback, controller_for_slot);
        states[rp.subpad].suppress_classic_until_us = ns::now_us() + 20000ULL;
    }

    void apply_packet(const ns::RumblePacket& rp, const int controller_for_slot[4]) {
        if (rp.subpad >= 4) return;
        const int slot = rp.subpad;
        if (ns::now_us() < states[slot].suppress_classic_until_us) return;
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

    void stop_all() {
        int none[4] = {-1, -1, -1, -1};
        for (int i = 0; i < 4; ++i) set_output(i, 0, 0, none[i]);
        g_sdlInput.stop_all_rumble();
    }

private:
    struct SlotState {
        uint8_t low = 0, high = 0;
        uint64_t until_us = 0;
        uint64_t last_set_us = 0;
        int last_controller = -1;
        uint64_t suppress_classic_until_us = 0;
    } states[4];

    void set_output(int slot, uint8_t low, uint8_t high, int pad_idx) {
        if (states[slot].last_controller != -1 && states[slot].last_controller != pad_idx)
            g_sdlInput.set_rumble(states[slot].last_controller, 0, 0, 0);
        if (pad_idx >= 0)
            g_sdlInput.set_rumble(pad_idx, low, high, (low || high) ? 250 : 0);
        states[slot].last_controller = pad_idx;
    }
};

static void pump_udp_rumble(SOCKET sock, RumbleManager& rumble, const int controller_for_slot[4]) {
    uint8_t buf[64];
    for (;;) {
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(buf), (int)sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
#else
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
#endif
        if (n < 0) {
            if (!socket_would_block()) {
                // Keep the sender alive; rumble is opportunistic.
            }
            break;
        }
        if (n == (int)sizeof(ns::PrecisionRumblePacket)) {
            ns::PrecisionRumblePacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::PRECISION_RUMBLE_MAGIC) rumble.apply_precision_packet(rp, controller_for_slot);
        } else if (n == (int)sizeof(ns::RumblePacket)) {
            ns::RumblePacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::RUMBLE_MAGIC) rumble.apply_packet(rp, controller_for_slot);
        }
    }
}

static bool detect_server_is_legacy(SOCKET sock, const sockaddr_in& dest) {
    ns::ServerInfoProbe probe{};
    send_all_udp(sock, dest, &probe, sizeof(probe));
    const uint64_t deadline = ns::now_us() + 150000ULL;
    while (ns::now_us() < deadline) {
        ns::ServerInfoReply reply{};
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(&reply), (int)sizeof(reply), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
#else
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, &reply, sizeof(reply), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
#endif
        if (n == (int)sizeof(reply) &&
            reply.magic == ns::SERVER_INFO_MAGIC &&
            reply.version == ns::SERVER_INFO_VERSION) {
            return reply.backend == ns::SERVER_BACKEND_LEGACY;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // No reply: assume modern mode unless --hori was explicitly given.
    return false;
}

static uint32_t g_macro_udp_seq = 0;

static uint32_t next_macro_upload_id() {
    uint32_t v = ++g_macro_udp_seq;
    if (v == 0) v = ++g_macro_udp_seq;
    return v;
}

template <typename SockT>
static bool send_macro_udp_packet(SockT sock, const sockaddr_in& dest, const uint8_t hmac_key[32],
                                  const std::string& json_or_commands, uint8_t subpad = 0) {
    if (json_or_commands.size() > ns::macro::UDP_TEXT_MAX) return false;
    if (json_or_commands.size() + ns::macro::UDP_HEADER_SIZE + ns::HMAC_TAG_SIZE <= 1400) {
        std::vector<uint8_t> buf(ns::macro::UDP_HEADER_SIZE + json_or_commands.size() + ns::HMAC_TAG_SIZE);
        ns::macro::MacroUdpHeaderWire hdr{};
        hdr.magic = ns::macro::UDP_MAGIC;
        hdr.version = ns::PROTO_VERSION;
        hdr.subpad = subpad;
        hdr.text_len = (uint32_t)json_or_commands.size();
        hdr.seq = next_macro_upload_id();
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        if (!json_or_commands.empty()) {
            std::memcpy(buf.data() + sizeof(hdr), json_or_commands.data(), json_or_commands.size());
        }
        uint8_t full_hmac[32];
        hmac_sha256(hmac_key, 32, buf.data(), sizeof(hdr) + json_or_commands.size(), full_hmac);
        std::memcpy(buf.data() + sizeof(hdr) + json_or_commands.size(), full_hmac, ns::HMAC_TAG_SIZE);
        return send_all_udp((SOCKET)sock, dest, buf.data(), buf.size()) != SOCKET_ERROR;
    }

    const uint32_t upload_id = next_macro_upload_id();
    const uint32_t chunk_count = (uint32_t)((json_or_commands.size() + ns::macro::UDP_CHUNK_MAX - 1) / ns::macro::UDP_CHUNK_MAX);
    for (uint32_t i = 0; i < chunk_count; ++i) {
        size_t off = (size_t)i * ns::macro::UDP_CHUNK_MAX;
        size_t n = std::min(ns::macro::UDP_CHUNK_MAX, json_or_commands.size() - off);
        std::vector<uint8_t> buf(ns::macro::CHUNK_HEADER_SIZE + n + ns::HMAC_TAG_SIZE);
        ns::macro::MacroUdpChunkHeaderWire hdr{};
        hdr.magic = ns::macro::UDP_CHUNK_MAGIC;
        hdr.version = ns::PROTO_VERSION;
        hdr.subpad = subpad;
        hdr.flags = (i + 1 == chunk_count) ? 0x01 : 0x00;
        hdr.reserved = 0;
        hdr.upload_id = upload_id;
        hdr.chunk_index = i;
        hdr.chunk_count = chunk_count;
        hdr.total_len = (uint32_t)json_or_commands.size();
        hdr.chunk_len = (uint16_t)n;
        hdr.seq = next_macro_upload_id();
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        if (n > 0) std::memcpy(buf.data() + sizeof(hdr), json_or_commands.data() + off, n);
        uint8_t full_hmac[32];
        hmac_sha256(hmac_key, 32, buf.data(), ns::macro::CHUNK_HEADER_SIZE + n, full_hmac);
        std::memcpy(buf.data() + ns::macro::CHUNK_HEADER_SIZE + n, full_hmac, ns::HMAC_TAG_SIZE);
        if (send_all_udp((SOCKET)sock, dest, buf.data(), buf.size()) == SOCKET_ERROR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

static std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char sep =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + sep + b;
}

static std::string dirname_of(std::string path) {
    size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return ".";
    return path.substr(0, slash);
}

static std::string executable_dir() {
#ifdef _WIN32
    char buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return dirname_of(buf);
    return ".";
#elif defined(__APPLE__)
    char buf[PATH_MAX]{};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return dirname_of(buf);
    std::vector<char> big(size + 1);
    if (_NSGetExecutablePath(big.data(), &size) == 0) {
        big[size] = '\0';
        return dirname_of(big.data());
    }
    return ".";
#else
    char buf[PATH_MAX]{};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return dirname_of(buf); }
    return ".";
#endif
}

static void make_dir_if_needed(const std::string& dir) {
#ifdef _WIN32
    CreateDirectoryA(dir.c_str(), nullptr);
#else
    mkdir(dir.c_str(), 0755);
#endif
}

static std::string user_config_dir() {
#ifdef _WIN32
    char appdata[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    std::string dir = (n > 0 && n < MAX_PATH) ? path_join(appdata, "NSPCControl") : "NSPCControl";
    make_dir_if_needed(dir);
    return dir;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    std::string base = home ? path_join(home, "Library/Application Support") : ".";
    std::string dir = path_join(base, "NSPCControl");
    make_dir_if_needed(dir);
    return dir;
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string base = xdg && *xdg ? xdg : (home ? path_join(home, ".config") : ".");
    std::string dir = path_join(base, "NSPCControl");
    make_dir_if_needed(dir);
    return dir;
#endif
}

static std::string settings_path() { return path_join(user_config_dir(), "settings.ini"); }
static std::string bindings_path() { return path_join(user_config_dir(), "bindings.ini"); }
static std::string macros_path() { return path_join(user_config_dir(), "macros.json"); }

static std::unordered_map<std::string, std::string> read_kv_file(const std::string& path) {
    std::unordered_map<std::string, std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[ns::macro::trim(line.substr(0, eq))] = ns::macro::trim(line.substr(eq + 1));
    }
    return out;
}

static bool write_kv_file(const std::string& path, const std::unordered_map<std::string, std::string>& values) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    for (const auto& kv : values) f << kv.first << "=" << kv.second << "\n";
    return (bool)f;
}

enum { KB_OFF = 0, KB_SINGLE = 1, KB_OVERRIDE = 2 };
static std::atomic<int> g_keyboardMode{KB_OFF};
static std::unordered_map<std::string, std::string> g_keyBindings;
static std::mutex g_pressedKeysMutex;
static std::unordered_set<std::string> g_pressedKeys;

static std::vector<std::pair<std::string, std::string>> binding_keys() {
    return {
        {"A", "V"}, {"B", "X"}, {"X", "C"}, {"Y", "Z"},
        {"L", "Q"}, {"R", "E"}, {"ZL", "1"}, {"ZR", "2"},
        {"MINUS", "3"}, {"PLUS", "4"},
        {"LSTICK", "LSHIFT"}, {"RSTICK", "RSHIFT"},
        {"HOME", "HOME"}, {"LSTICK_UP", "W"}, {"LSTICK_DOWN", "S"},
        {"LSTICK_LEFT", "A"}, {"LSTICK_RIGHT", "D"},
        {"RSTICK_UP", "I"}, {"RSTICK_DOWN", "K"},
        {"RSTICK_LEFT", "J"}, {"RSTICK_RIGHT", "L"},
        {"DPAD_UP", "UP"}, {"DPAD_DOWN", "DOWN"},
        {"DPAD_LEFT", "LEFT"}, {"DPAD_RIGHT", "RIGHT"},
        {"CAPTURE", "SNAPSHOT"}
    };
}

static std::unordered_map<std::string, std::string> default_key_bindings() {
    std::unordered_map<std::string, std::string> out;
    for (const auto& kv : binding_keys()) out[kv.first] = kv.second;
    return out;
}

static std::string normalize_key_name(std::string s) {
    return ns::macro::upper(ns::macro::trim(std::move(s)));
}

static std::string normalize_macro_hotkey_for_io(const std::string& s) {
    return normalize_key_name(s);
}

static void load_saved_bindings() {
    g_keyBindings = default_key_bindings();
    auto kv = read_kv_file(bindings_path());
    for (auto& it : g_keyBindings) {
        auto found = kv.find(it.first);
        if (found != kv.end()) it.second = normalize_key_name(found->second);
    }
}

static void save_bindings() {
    write_kv_file(bindings_path(), g_keyBindings);
}

static std::string load_saved_ip() {
    auto kv = read_kv_file(settings_path());
    auto it = kv.find("LastIP");
    if (it != kv.end() && !it->second.empty()) return it->second;
    return "192.168.1.100";
}

static void save_last_ip(const std::string& ip) {
    auto kv = read_kv_file(settings_path());
    kv["LastIP"] = ip;
    write_kv_file(settings_path(), kv);
}

static int load_saved_keyboard_mode() {
    auto kv = read_kv_file(settings_path());
    auto it = kv.find("KeyboardMode");
    if (it == kv.end()) return KB_OFF;
    int mode = std::atoi(it->second.c_str());
    return (mode >= KB_OFF && mode <= KB_OVERRIDE) ? mode : KB_OFF;
}

static void save_keyboard_mode(int mode) {
    auto kv = read_kv_file(settings_path());
    kv["KeyboardMode"] = std::to_string(mode);
    write_kv_file(settings_path(), kv);
}

static void set_key_pressed(const std::string& key, bool down) {
    std::string k = normalize_key_name(key);
    if (k.empty()) return;
    std::lock_guard<std::mutex> lk(g_pressedKeysMutex);
    if (down) g_pressedKeys.insert(k);
    else g_pressedKeys.erase(k);
}

static bool pressed_key_cache_contains(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_pressedKeysMutex);
    return g_pressedKeys.count(normalize_key_name(key)) != 0;
}

#ifdef _WIN32
static int windows_vk_for_key(const std::string& name) {
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') return name[0];
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') return name[0];
    struct Map { const char* n; int vk; };
    static const Map map[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
        {"LALT", VK_LMENU}, {"RALT", VK_RMENU}, {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN},
        {"TAB", VK_TAB}, {"ESC", VK_ESCAPE}, {"BACKSPACE", VK_BACK}, {"HOME", VK_HOME},
        {"SNAPSHOT", VK_SNAPSHOT}, {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
        {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9},
        {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12}
    };
    for (const auto& m : map) if (name == m.n) return m.vk;
    return 0;
}
#endif

#ifdef __APPLE__
static int mac_keycode_for_key(const std::string& name) {
    static const std::unordered_map<std::string, int> map = {
        {"A", 0}, {"S", 1}, {"D", 2}, {"F", 3}, {"H", 4}, {"G", 5}, {"Z", 6}, {"X", 7},
        {"C", 8}, {"V", 9}, {"B", 11}, {"Q", 12}, {"W", 13}, {"E", 14}, {"R", 15},
        {"Y", 16}, {"T", 17}, {"1", 18}, {"2", 19}, {"3", 20}, {"4", 21}, {"6", 22},
        {"5", 23}, {"=", 24}, {"9", 25}, {"7", 26}, {"-", 27}, {"8", 28}, {"0", 29},
        {"O", 31}, {"U", 32}, {"I", 34}, {"P", 35}, {"L", 37}, {"J", 38}, {"K", 40},
        {"N", 45}, {"M", 46}, {"TAB", 48}, {"SPACE", 49}, {"BACKSPACE", 51}, {"ESC", 53},
        {"LCTRL", 59}, {"LSHIFT", 56}, {"LALT", 58}, {"RCTRL", 62}, {"RSHIFT", 60}, {"RALT", 61},
        {"LEFT", 123}, {"RIGHT", 124}, {"DOWN", 125}, {"UP", 126}, {"ENTER", 36}, {"HOME", 115},
        {"F1", 122}, {"F2", 120}, {"F3", 99}, {"F4", 118}, {"F5", 96}, {"F6", 97},
        {"F7", 98}, {"F8", 100}, {"F9", 101}, {"F10", 109}, {"F11", 103}, {"F12", 111}
    };
    auto it = map.find(name);
    return it == map.end() ? -1 : it->second;
}
#endif

static bool key_is_down(const std::string& name_raw) {
    std::string name = normalize_key_name(name_raw);
    if (name.empty()) return false;
#ifdef _WIN32
    int vk = windows_vk_for_key(name);
    if (vk && (GetAsyncKeyState(vk) & 0x8000)) return true;
#endif
#ifdef __APPLE__
    int kc = mac_keycode_for_key(name);
    if (kc >= 0 && CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, (CGKeyCode)kc)) return true;
#endif
    return pressed_key_cache_contains(name);
}

static void apply_keyboard_to_report(ns::HIDReport& rep, bool override_mode) {
    auto get = [](const std::string& btn) -> std::string {
        auto it = g_keyBindings.find(btn);
        return it != g_keyBindings.end() ? it->second : "";
    };
    std::string k;
    k = get("Y"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_Y;
    k = get("B"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_B;
    k = get("A"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_A;
    k = get("X"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_X;
    k = get("L"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_L;
    k = get("R"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_R;
    k = get("ZL"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_ZL;
    k = get("ZR"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_ZR;
    k = get("MINUS"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_MINUS;
    k = get("PLUS"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_PLUS;
    k = get("LSTICK"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_LSTICK;
    k = get("RSTICK"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_RSTICK;
    k = get("HOME"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_HOME;
    k = get("CAPTURE"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_CAPTURE;

    bool du = !get("DPAD_UP").empty() && key_is_down(get("DPAD_UP"));
    bool dd = !get("DPAD_DOWN").empty() && key_is_down(get("DPAD_DOWN"));
    bool dl = !get("DPAD_LEFT").empty() && key_is_down(get("DPAD_LEFT"));
    bool dr = !get("DPAD_RIGHT").empty() && key_is_down(get("DPAD_RIGHT"));
    rep.hat = ns::HAT_NEUTRAL;
    if (du && dr) rep.hat = ns::HAT_NE;
    else if (du && dl) rep.hat = ns::HAT_NW;
    else if (dd && dr) rep.hat = ns::HAT_SE;
    else if (dd && dl) rep.hat = ns::HAT_SW;
    else if (du) rep.hat = ns::HAT_N;
    else if (dd) rep.hat = ns::HAT_S;
    else if (dr) rep.hat = ns::HAT_E;
    else if (dl) rep.hat = ns::HAT_W;

    bool lsu = !get("LSTICK_UP").empty() && key_is_down(get("LSTICK_UP"));
    bool lsd = !get("LSTICK_DOWN").empty() && key_is_down(get("LSTICK_DOWN"));
    bool lsl = !get("LSTICK_LEFT").empty() && key_is_down(get("LSTICK_LEFT"));
    bool lsr = !get("LSTICK_RIGHT").empty() && key_is_down(get("LSTICK_RIGHT"));
    if (lsl && !lsr) rep.lx = 0;
    else if (lsr && !lsl) rep.lx = 255;
    else if (!override_mode) rep.lx = 128;
    if (lsu && !lsd) rep.ly = 0;
    else if (lsd && !lsu) rep.ly = 255;
    else if (!override_mode) rep.ly = 128;

    bool rsu = !get("RSTICK_UP").empty() && key_is_down(get("RSTICK_UP"));
    bool rsd = !get("RSTICK_DOWN").empty() && key_is_down(get("RSTICK_DOWN"));
    bool rsl = !get("RSTICK_LEFT").empty() && key_is_down(get("RSTICK_LEFT"));
    bool rsr = !get("RSTICK_RIGHT").empty() && key_is_down(get("RSTICK_RIGHT"));
    if (rsl && !rsr) rep.rx = 0;
    else if (rsr && !rsl) rep.rx = 255;
    else if (!override_mode) rep.rx = 128;
    if (rsu && !rsd) rep.ry = 0;
    else if (rsd && !rsu) rep.ry = 255;
    else if (!override_mode) rep.ry = 128;
}

static std::mutex g_macro_mtx;
static std::vector<ns::macro::Step> g_macro_steps;
static bool g_macro_running = false;
static uint64_t g_macro_start_us = 0;
static std::string g_macro_upload_pending;
static std::vector<ns::macro::Entry> g_macro_entries;
static std::unordered_map<std::string, bool> g_macro_hotkey_down;

static int find_macro_entry_by_name(const std::string& name) {
    std::string wanted = ns::macro::upper(ns::macro::trim(name));
    if (wanted.empty()) return -1;
    for (int i = 0; i < (int)g_macro_entries.size(); ++i) {
        if (ns::macro::upper(g_macro_entries[i].name) == wanted) return i;
    }
    return -1;
}

static std::string unique_macro_name(const std::string& base_raw) {
    std::string base = ns::macro::trim(base_raw);
    if (base.empty()) base = "Recorded Macro";
    std::string name = base;
    int suffix = 2;
    while (find_macro_entry_by_name(name) >= 0) name = base + " " + std::to_string(suffix++);
    return name;
}

static bool macro_hotkey_conflicts(const std::string& hotkey, std::string* conflict_name = nullptr) {
    std::string hk = normalize_key_name(hotkey);
    if (hk.empty()) return false;
    for (const auto& kv : g_keyBindings) {
        if (normalize_key_name(kv.second) == hk) {
            if (conflict_name) *conflict_name = kv.first;
            return true;
        }
    }
    return false;
}

static bool macro_entry_hotkey_conflicts(const std::string& hotkey, int skip_index, std::string* conflict_name = nullptr) {
    std::string hk = normalize_key_name(hotkey);
    if (hk.empty()) return false;
    for (int i = 0; i < (int)g_macro_entries.size(); ++i) {
        if (i == skip_index) continue;
        if (normalize_key_name(g_macro_entries[i].hotkey) == hk) {
            if (conflict_name) *conflict_name = g_macro_entries[i].name.empty() ? "another macro" : g_macro_entries[i].name;
            return true;
        }
    }
    return false;
}

static void rebuild_macro_hotkey_state() {
    g_macro_hotkey_down.clear();
    for (const auto& e : g_macro_entries) {
        std::string hk = normalize_key_name(e.hotkey);
        if (!hk.empty()) g_macro_hotkey_down[hk] = false;
    }
}

static void load_macro_entries() {
    std::string err;
    std::vector<ns::macro::Entry> loaded;
    std::string raw = ns::macro::read_text_file_limited(macros_path(), &err);
    if (!ns::macro::parse_entries_text(raw, loaded, err, normalize_macro_hotkey_for_io)) loaded.clear();
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_entries = std::move(loaded);
    rebuild_macro_hotkey_state();
}

static bool save_macro_entries_to_disk() {
    std::string json;
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        json = ns::macro::entries_to_json(g_macro_entries, normalize_macro_hotkey_for_io);
    }
    if (json.size() > ns::macro::JSON_MAX_BYTES) return false;
    std::ofstream f(macros_path(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(json.data(), (std::streamsize)json.size());
    return (bool)f;
}

static bool start_macro_text(const std::string& txt, std::string* err_out = nullptr) {
    std::vector<ns::macro::Step> parsed;
    if (!ns::macro::validate_text(txt, parsed, nullptr)) {
        if (err_out) *err_out = ns::macro::last_error();
        return false;
    }
    std::string pretty, err;
    if (!ns::macro::validate_to_pretty_json(txt, pretty, err, "Macro")) {
        if (err_out) *err_out = err;
        return false;
    }
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_upload_pending = pretty;
    g_macro_steps = std::move(parsed);
    g_macro_running = false;
    g_macro_start_us = ns::now_us();
    return true;
}

static bool upsert_macro_entry(ns::macro::Entry e, bool force_unique_name, std::string* err_out = nullptr) {
    std::string pretty, err;
    if (!ns::macro::validate_to_pretty_json(e.json, pretty, err, e.name.empty() ? "Macro" : e.name)) {
        if (err_out) *err_out = err;
        return false;
    }
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    e.name = ns::macro::trim(e.name);
    if (e.name.empty()) e.name = ns::macro::extract_name_or_default(pretty, "Macro");
    if (force_unique_name) e.name = unique_macro_name(e.name);
    e.hotkey = normalize_key_name(e.hotkey);
    e.json = ns::macro::pretty_json_with_forced_name(pretty, e.name);
    int existing = force_unique_name ? -1 : find_macro_entry_by_name(e.name);
    std::string conflict;
    if (macro_hotkey_conflicts(e.hotkey, &conflict) || macro_entry_hotkey_conflicts(e.hotkey, existing, &conflict)) {
        e.hotkey.clear();
    }
    if (existing >= 0) g_macro_entries[existing] = std::move(e);
    else g_macro_entries.push_back(std::move(e));
    rebuild_macro_hotkey_state();
    return true;
}

static void poll_macro_entry_hotkeys() {
    std::vector<std::string> to_run;
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        for (const auto& e : g_macro_entries) {
            std::string hk = normalize_key_name(e.hotkey);
            if (hk.empty()) continue;
            if (macro_hotkey_conflicts(hk, nullptr)) continue;
            bool down = key_is_down(hk);
            bool was_down = g_macro_hotkey_down[hk];
            g_macro_hotkey_down[hk] = down;
            if (down && !was_down) to_run.push_back(e.json);
        }
    }
    for (const auto& json : to_run) start_macro_text(json, nullptr);
}

static ns::macro::Recorder g_macro_recorder;

static void macro_record_start() {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_recorder.start(ns::now_us());
}

static std::string macro_record_stop() {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    return g_macro_recorder.stop(ns::now_us());
}

static void macro_record_sample(const ns::HIDReport& report) {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_recorder.sample(report, ns::now_us(), g_macro_running);
}

static bool poll_macro_record_p1(ns::HIDReport& report) {
    report.reset();
    auto sdl = g_sdlInput.snapshot();
    if (sdl[0].connected) {
        report = sdl[0].input;
        return true;
    }
    int km = g_keyboardMode.load();
    if (km != KB_OFF) {
        apply_keyboard_to_report(report, km == KB_OVERRIDE);
        return true;
    }
    return false;
}

static void macro_record_sample_p1() {
    ns::HIDReport report;
    poll_macro_record_p1(report);
    macro_record_sample(report);
}

static bool apply_macro_override(ns::HIDReport logical_reports[4], bool present[4], bool has_motion[4]) {
    (void)has_motion;
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    if (!g_macro_running) return false;
    uint64_t elapsed_ms = (ns::now_us() - g_macro_start_us) / 1000ULL;
    ns::HIDReport mr;
    bool active = ns::macro::report_at(g_macro_steps, elapsed_ms, mr);
    for (int i = 0; i < 4; ++i) {
        logical_reports[i].reset();
        present[i] = false;
        has_motion[i] = false;
    }
    logical_reports[0] = mr;
    present[0] = true;
    if (!active && elapsed_ms > ns::macro::total_ms(g_macro_steps) + 120) g_macro_running = false;
    return true;
}

static std::atomic<bool> g_senderRunning{false};
static std::atomic<bool> g_connected{false};
static std::thread g_senderThread;
static uint8_t g_hmacKey[32]{};
static std::atomic<uint32_t> g_packetCount{0};
static std::mutex g_statusMutex;
static std::string g_statusMessage = "Ready";
static std::string g_lastError;

static void set_status_message(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    g_statusMessage = s;
}

static std::string status_message() {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    return g_statusMessage;
}

static bool parse_host_port(std::string in, std::string& host, int& port) {
    in = ns::macro::trim(in);
    if (in.empty()) return false;
    port = ns::DEFAULT_PORT;
    size_t colon = in.find(':');
    if (colon != std::string::npos) {
        int p = std::atoi(in.c_str() + colon + 1);
        if (p >= 1 && p <= 65535) port = p;
        in.resize(colon);
    }
    host = ns::macro::trim(in);
    return !host.empty();
}

static void raise_process_priority() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#else
    setpriority(PRIO_PROCESS, 0, -20);
#endif
}

static void raise_sender_priority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}

struct ClientFrame {
    ns::HIDReport reports[4];
    ns::MotionReport motion[4][3];
    bool present[4] = {false, false, false, false};
    bool has_motion[4] = {false, false, false, false};
    int controller_for_slot[4] = {-1, -1, -1, -1};
    int active_count = 0;

    void reset() {
        active_count = 0;
        for (int i = 0; i < 4; ++i) {
            reports[i].reset();
            for (int j = 0; j < 3; ++j) motion[i][j].reset();
            present[i] = false;
            has_motion[i] = false;
            controller_for_slot[i] = -1;
        }
    }
};

struct ClientStreamConfig {
    std::string host;
    int port = ns::DEFAULT_PORT;
    bool force_legacy_udp = false;
    bool gui_features = false;
    bool print_cli_waiting_messages = false;
    int idle_sleep_ms = 50;
    const uint8_t* hmac_key = nullptr;
};

static void build_client_frame(ClientFrame& frame,
                               DigitalReleaseFilter filters[4],
                               bool send_motion,
                               int keyboard_mode) {
    frame.reset();

    auto sdl = g_sdlInput.snapshot();
    const uint64_t filter_now = ns::now_us();

    for (int i = 0; i < 4; ++i) {
        if (!sdl[i].connected) {
            filters[i].reset();
            continue;
        }

        frame.reports[i] = sdl[i].input;
        filters[i].apply(frame.reports[i], filter_now);
        for (int j = 0; j < 3; ++j) frame.motion[i][j] = sdl[i].motion_samples[j];
        frame.present[i] = true;
        frame.has_motion[i] = send_motion && sdl[i].has_motion;
        frame.controller_for_slot[i] = i;
        ++frame.active_count;
    }

    if (keyboard_mode == KB_SINGLE) {
        if (frame.present[0]) {
            int target = -1;
            for (int s = 1; s < 4; ++s) {
                if (!frame.present[s]) { target = s; break; }
            }
            if (target >= 0) {
                frame.reports[target] = frame.reports[0];
                for (int j = 0; j < 3; ++j) frame.motion[target][j] = frame.motion[0][j];
                frame.has_motion[target] = frame.has_motion[0];
                frame.present[target] = true;
                frame.controller_for_slot[target] = frame.controller_for_slot[0];
                ++frame.active_count;
            }
        }

        frame.reports[0].reset();
        for (int j = 0; j < 3; ++j) frame.motion[0][j].reset();
        apply_keyboard_to_report(frame.reports[0], false);
        frame.present[0] = true;
        frame.has_motion[0] = false;
        frame.controller_for_slot[0] = -1;
        frame.active_count = std::max(frame.active_count, 1);
    } else if (keyboard_mode == KB_OVERRIDE) {
        apply_keyboard_to_report(frame.reports[0], true);
        frame.present[0] = true;
        frame.active_count = std::max(frame.active_count, 1);
    }
}

static void send_client_frame(SOCKET sock,
                              const sockaddr_in& dest,
                              const uint8_t hmac_key[32],
                              uint32_t& seq,
                              bool legacy_packet,
                              const ClientFrame& frame) {
    if (legacy_packet) {
        ns::Packet pkt{};
        pkt.magic = ns::PROTO_MAGIC;
        pkt.version = ns::PROTO_VERSION;
        pkt.flags = ns::FLAG_NONE;
        pkt.seq = seq++;
        pkt.ts_us = ns::now_us();
        pkt.report.reset();

        ns::HIDReport* pads[4] = {&pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4};
        for (int i = 0; i < 4; ++i) *pads[i] = frame.reports[i];

        uint8_t full_hmac[32];
        hmac_sha256(hmac_key, 32, reinterpret_cast<const uint8_t*>(&pkt), ns::PACKET_AUTH_SIZE, full_hmac);
        std::memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        send_all_udp(sock, dest, &pkt, ns::PACKET_SIZE);
        return;
    }

    ExtendedUdpPacket pkt{};
    pkt.magic = ns::PROTO_MAGIC;
    pkt.version = ns::WEB_PROTO_VERSION_3;
    pkt.flags = ns::FLAG_NONE;
    pkt.seq = seq++;
    pkt.timestamp_us = ns::now_us();
    pkt.report.reset();

    ns::ExtendedHIDReport3* pads[4] = {&pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4};
    for (int i = 0; i < 4; ++i) {
        fill_extended_pad(*pads[i], frame.reports[i], frame.present[i],
                          frame.has_motion[i] ? frame.motion[i] : nullptr);
    }

    uint8_t full_hmac[32];
    hmac_sha256(hmac_key, 32, reinterpret_cast<const uint8_t*>(&pkt), EXT_UDP_PACKET_AUTH_SIZE, full_hmac);
    std::memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
    send_all_udp(sock, dest, &pkt, sizeof(pkt));
}

static int run_client_stream(const ClientStreamConfig& cfg,
                             std::atomic<bool>& running,
                             std::string* err_out = nullptr) {
    if (!cfg.hmac_key) {
        if (err_out) *err_out = "Missing HMAC key.";
        return 1;
    }

    raise_sender_priority();

    if (!g_sdlInput.start()) {
        if (err_out) *err_out = "SDL3 input failed: " + g_sdlInput.error();
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        if (err_out) *err_out = "Failed to create UDP socket.";
        return 1;
    }
    set_socket_nonblocking(sock);

    sockaddr_in dest{};
    if (!resolve_udp_destination(cfg.host, cfg.port, dest)) {
        if (err_out) *err_out = "Cannot resolve address: " + cfg.host;
        closesocket(sock);
        return 1;
    }

    const bool server_is_legacy = detect_server_is_legacy(sock, dest);
    const bool legacy_packet = cfg.force_legacy_udp || server_is_legacy;
    const bool send_motion = !legacy_packet;
    const int active_send_interval_ms = ns::LEGACY_UDP_INTERVAL_MS; // always 250 Hz

    uint32_t seq = 0;
    RumbleManager rumble;
    DigitalReleaseFilter sdl_filters[4];
    bool no_controllers_printed = false;

    while (running.load(std::memory_order_relaxed)) {
        if (cfg.gui_features) {
            std::string upload;
            {
                std::lock_guard<std::mutex> lk(g_macro_mtx);
                upload.swap(g_macro_upload_pending);
            }
            if (!upload.empty()) send_macro_udp_packet(sock, dest, cfg.hmac_key, upload, 0);

            poll_macro_entry_hotkeys();
        }

        g_sdlInput.poll();

        ClientFrame frame;
        build_client_frame(frame, sdl_filters, send_motion, g_keyboardMode.load());

        if (cfg.gui_features) {
            macro_record_sample(frame.reports[0]);
            if (apply_macro_override(frame.reports, frame.present, frame.has_motion)) {
                frame.active_count = 1;
            }
        }

        send_client_frame(sock, dest, cfg.hmac_key, seq, legacy_packet, frame);

        if (!legacy_packet) {
            pump_udp_rumble(sock, rumble, frame.controller_for_slot);
            rumble.update_timeouts(frame.controller_for_slot);
        }

        ++g_packetCount;

        if (frame.active_count > 0) {
            no_controllers_printed = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(active_send_interval_ms));
        } else {
            if (cfg.print_cli_waiting_messages && !no_controllers_printed) {
                std::cout << "No controllers detected - waiting for connections...\n";
                no_controllers_printed = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.idle_sleep_ms));
        }
    }

    rumble.stop_all();
    send_udp_disconnect_packet(sock, dest, cfg.hmac_key, seq++, legacy_packet);
    closesocket(sock);
    return 0;
}

static void sender_thread_main(std::string host, uint16_t port, bool legacy_udp) {
    ClientStreamConfig cfg{};
    cfg.host = std::move(host);
    cfg.port = port;
    cfg.force_legacy_udp = legacy_udp;
    cfg.gui_features = true;
    cfg.print_cli_waiting_messages = false;
    cfg.idle_sleep_ms = 50;
    cfg.hmac_key = g_hmacKey;

    g_senderRunning.store(true);
    set_status_message("Connected to " + cfg.host + ":" + std::to_string(cfg.port));

    std::string err;
    int rc = run_client_stream(cfg, g_senderRunning, &err);
    if (rc != 0 && !err.empty()) {
        g_lastError = err;
        set_status_message(err);
    }

    g_connected.store(false);
    g_senderRunning.store(false);
}

static bool start_connection(const std::string& target, std::string* err_out = nullptr) {
    if (g_connected.load()) return true;
    std::string host;
    int port = ns::DEFAULT_PORT;
    if (!parse_host_port(target, host, port)) {
        if (err_out) *err_out = "Please enter a Raspberry Pi IP address.";
        return false;
    }
    if (!g_sdlInput.start()) {
        if (err_out) *err_out = "SDL3 input failed: " + g_sdlInput.error();
        return false;
    }
    derive_key(ns::DEFAULT_SECRET, g_hmacKey);
    save_last_ip(target);
    load_macro_entries();
    g_packetCount.store(0);
    g_lastError.clear();
    g_connected.store(true);
    g_senderRunning.store(true);
    if (g_senderThread.joinable()) g_senderThread.join();
    g_senderThread = std::thread(sender_thread_main, host, (uint16_t)port, false);
    return true;
}

static void stop_connection() {
    if (!g_connected.load()) return;
    g_connected.store(false);
    g_senderRunning.store(false);
    if (g_senderThread.joinable()) g_senderThread.join();
    set_status_message("Disconnected");
}

class NetworkRuntime {
public:
    NetworkRuntime() {
#ifdef _WIN32
        timeBeginPeriod(1);
        WSADATA wsa{};
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        signal(SIGPIPE, SIG_IGN);
        ok = true;
#endif
    }
    ~NetworkRuntime() {
        stop_connection();
        g_sdlInput.stop();
#ifdef _WIN32
        if (ok) WSACleanup();
        timeEndPeriod(1);
#endif
    }
    bool good() const { return ok; }
private:
    bool ok = false;
};

static std::atomic<bool> g_cliRunning{true};

#ifdef _WIN32
static BOOL WINAPI cli_console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT ||
        ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
        g_cliRunning.store(false);
        return TRUE;
    }
    return FALSE;
}
#else
static void cli_signal_handler(int) {
    g_cliRunning.store(false);
}
#endif

static void print_cli_usage(const char* exe) {
    std::cerr << "Usage: " << exe << " --cli <RASPBERRY_PI_IP[:PORT]> [-k [single|override]] [--hori] [--macro file.json]\n";
    std::cerr << "  --cli      Run the terminal client from this unified executable\n";
    std::cerr << "  -k         Enable keyboard mode where supported (default: single)\n";
    std::cerr << "  --hori     Send old input-only HORI-compatible UDP packets; disables UDP rumble/gyro\n";
    std::cerr << "  --macro    Upload a P1 server-side macro JSON/string, wait for it, then exit\n";
}

static int cli_main(const std::vector<std::string>& original_args) {
    NetworkRuntime net;
    if (!net.good()) {
        std::cerr << "ERROR: network startup failed\n";
        return 1;
    }
    raise_process_priority();
    g_cliRunning.store(true);
#ifdef _WIN32
    SetConsoleCtrlHandler(cli_console_ctrl_handler, TRUE);
#else
    signal(SIGINT, cli_signal_handler);
    signal(SIGTERM, cli_signal_handler);
#endif

    std::vector<std::string> args;
    args.reserve(original_args.size());
    for (const std::string& a : original_args) if (a != "--cli") args.push_back(a);
    if (args.empty()) args.push_back("ns-client");
    if (args.size() < 2) {
        print_cli_usage(args[0].c_str());
        return 1;
    }

    std::string host;
    int port = ns::DEFAULT_PORT;
    bool legacy_udp = false;
    bool macro_mode = false;
    std::string macro_path;
    int cli_keyboard_mode = KB_OFF;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--hori") {
            legacy_udp = true;
        } else if (a == "--macro" || a == "--upload-macro" || a == "--server-macro" || a == "-m") {
            if (i + 1 >= args.size()) {
                std::cerr << a << " requires a macro JSON/commands file path\n";
                return 1;
            }
            macro_mode = true;
            macro_path = args[++i];
        } else if (a == "-k" || a == "--keyboard") {
            cli_keyboard_mode = KB_SINGLE;
            if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != '-') {
                if (args[i + 1] == "override") cli_keyboard_mode = KB_OVERRIDE;
                else if (args[i + 1] == "single") cli_keyboard_mode = KB_SINGLE;
                else {
                    std::cerr << "Unknown keyboard mode: " << args[i + 1] << "\n";
                    return 1;
                }
                ++i;
            }
        } else if (a == "--help" || a == "-h" || a == "/?") {
            print_cli_usage(args[0].c_str());
            return 0;
        } else if (host.empty()) {
            std::string target = a;
            if (!parse_host_port(target, host, port)) {
                std::cerr << "Invalid host: " << a << "\n";
                return 1;
            }
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            print_cli_usage(args[0].c_str());
            return 1;
        }
    }

    if (host.empty()) {
        print_cli_usage(args[0].c_str());
        return 1;
    }

    load_saved_bindings();
    g_keyboardMode.store(cli_keyboard_mode);
    if (cli_keyboard_mode != KB_OFF) {
        std::cout << "Keyboard mode enabled (" << (cli_keyboard_mode == KB_SINGLE ? "single" : "override") << ") - ";
        std::cout << (cli_keyboard_mode == KB_SINGLE ? "replaces" : "augments") << " Player 1\n";
    }
    std::cout << (legacy_udp ? "Hori UDP mode: input only. UDP rumble and gyro are disabled.\n"
                             : "Extended UDP mode: SDL3 rumble replies + gyro/motion enabled where supported.\n");

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    if (macro_mode) {
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == INVALID_SOCKET) {
            std::cerr << "ERROR: socket() failed\n";
            return 1;
        }
        set_socket_nonblocking(sock);

        sockaddr_in dest{};
        if (!resolve_udp_destination(host, port, dest)) {
            std::cerr << "ERROR: Unable to resolve IP/host: " << host << "\n";
            closesocket(sock);
            return 1;
        }

        std::string err;
        std::string macro_raw = ns::macro::read_text_file_limited(macro_path, &err);
        if (macro_raw.empty()) {
            std::cerr << "Macro file is empty or cannot be read: " << macro_path << "\n";
            closesocket(sock);
            return 1;
        }
        auto steps = ns::macro::parse_text(macro_raw);
        if (steps.empty()) {
            std::cerr << "Macro file has no usable commands: " << macro_path << "\n";
            closesocket(sock);
            return 1;
        }
        bool sent = send_macro_udp_packet(sock, dest, hmac_key, macro_raw, 0);
        std::cout << (sent ? "Uploaded server-side macro to P1.\n" : "Failed to upload server-side macro.\n");
        uint64_t wait_ms = std::min<uint64_t>(ns::macro::total_ms(steps), 600000ULL);
        std::this_thread::sleep_for(std::chrono::milliseconds((int)wait_ms + 180));
        closesocket(sock);
        return sent ? 0 : 1;
    }

    ClientStreamConfig cfg{};
    cfg.host = host;
    cfg.port = port;
    cfg.force_legacy_udp = legacy_udp;
    cfg.gui_features = false;
    cfg.print_cli_waiting_messages = true;
    cfg.idle_sleep_ms = 500;
    cfg.hmac_key = hmac_key;

    std::cout << "Started unified CLI client. Press Ctrl+C to stop.\n";
    std::string err;
    int rc = run_client_stream(cfg, g_cliRunning, &err);
    if (rc != 0 && !err.empty()) std::cerr << "ERROR: " << err << "\n";
    std::cout << "\nShutting down...\n";
    return rc;
}

static std::string q_to_std(const QString& s) { return s.toUtf8().constData(); }
static QString std_to_q(const std::string& s) { return QString::fromUtf8(s.c_str()); }

static QString key_name_from_qkey(QKeyEvent* event) {
#ifdef _WIN32
    int vk = (int)event->nativeVirtualKey();
    if (vk == VK_LSHIFT) return "LSHIFT";
    if (vk == VK_RSHIFT) return "RSHIFT";
    if (vk == VK_LCONTROL) return "LCTRL";
    if (vk == VK_RCONTROL) return "RCTRL";
    if (vk == VK_LMENU) return "LALT";
    if (vk == VK_RMENU) return "RALT";
#endif
    int key = event->key();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return QString(QChar('A' + key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return QString(QChar('0' + key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) return QString("F%1").arg(key - Qt::Key_F1 + 1);
    switch (key) {
        case Qt::Key_Up: return "UP";
        case Qt::Key_Down: return "DOWN";
        case Qt::Key_Left: return "LEFT";
        case Qt::Key_Right: return "RIGHT";
        case Qt::Key_Shift: return "LSHIFT";
        case Qt::Key_Control: return "LCTRL";
        case Qt::Key_Alt: return "LALT";
        case Qt::Key_Space: return "SPACE";
        case Qt::Key_Return:
        case Qt::Key_Enter: return "ENTER";
        case Qt::Key_Tab: return "TAB";
        case Qt::Key_Escape: return "ESC";
        case Qt::Key_Backspace: return "BACKSPACE";
        case Qt::Key_Home: return "HOME";
        case Qt::Key_Print: return "SNAPSHOT";
        default: break;
    }
    return {};
}

static QIcon app_icon() {
    static QIcon cached;
    static bool loaded = false;
    if (loaded) return cached;
    loaded = true;

    QIcon embedded(":/icon.png");
    if (!embedded.isNull()) {
        cached = embedded;
        return cached;
    }

    const std::string exe_dir = executable_dir();

    std::vector<std::string> candidates = {
#ifdef __APPLE__
        path_join(path_join(exe_dir, "../Resources"), "icon.icns"),
        path_join(path_join(exe_dir, "../Resources"), "icon.png"),
#endif
#ifndef _WIN32
        path_join(path_join(exe_dir, "../share/icons/hicolor/256x256/apps"), "ns-client.png"),
        path_join(path_join(exe_dir, "../share/pixmaps"), "ns-client.png"),
#endif
        path_join(exe_dir, "icon.ico"),
        path_join(exe_dir, "icon.png"),
        path_join(NS_CLIENT_SOURCE_DIR, "icon.ico"),
        path_join(NS_CLIENT_SOURCE_DIR, "icon.png")
    };

    for (const std::string& p : candidates) {
        QIcon icon(std_to_q(p));
        if (!icon.isNull()) {
            cached = icon;
            return cached;
        }
    }

    return {};
}

class KeyCaptureDialog : public QDialog {
public:
    explicit KeyCaptureDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle("Press key");
        setModal(true);
        auto* layout = new QVBoxLayout(this);
        auto* label = new QLabel("Press a key, or Esc to clear.", this);
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);
        resize(260, 80);
    }
    QString keyName;
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            keyName.clear();
            accept();
            return;
        }
        keyName = key_name_from_qkey(event);
        if (!keyName.isEmpty()) accept();
    }
};

class BindingsDialog : public QDialog {
public:
    explicit BindingsDialog(QWidget* parent = nullptr) : QDialog(parent), editBindings(g_keyBindings) {
        setWindowTitle("Keyboard Bindings");
        setModal(true);
        setMinimumWidth(620);
        auto* outer = new QVBoxLayout(this);
        auto* grid = new QGridLayout();
        grid->setHorizontalSpacing(4);
        grid->setVerticalSpacing(4);
        auto keys = binding_keys();
        int half = (int)keys.size() / 2;
        for (int i = 0; i < half; ++i) {
            addRow(grid, i, 0, i, keys[i].first);
            addRow(grid, i, 3, i + half, keys[i + half].first);
        }
        outer->addLayout(grid);

        auto* buttons = new QGridLayout();
        QPushButton* setup = new QPushButton("Setup", this);
        QPushButton* clear = new QPushButton("Clear", this);
        QPushButton* reset = new QPushButton("Reset", this);
        QPushButton* save = new QPushButton("Save", this);
        QPushButton* cancel = new QPushButton("Cancel", this);
        buttons->addWidget(setup, 0, 0);
        buttons->addWidget(clear, 0, 1);
        buttons->addWidget(reset, 0, 2);
        buttons->addWidget(save, 0, 4);
        buttons->addWidget(cancel, 0, 5);
        outer->addLayout(buttons);
        connect(setup, &QPushButton::clicked, this, [this] {
            setupMode = true;
            listeningIndex = 0;
            for (const auto& kv : binding_keys()) editBindings[kv.first].clear();
            refresh();
            if (!valueLabels.empty()) valueLabels[0]->setText("...");
            setFocus();
        });
        connect(clear, &QPushButton::clicked, this, [this] {
            setupMode = false;
            listeningIndex = -1;
            for (const auto& kv : binding_keys()) editBindings[kv.first].clear();
            refresh();
        });
        connect(reset, &QPushButton::clicked, this, [this] {
            editBindings = default_key_bindings();
            refresh();
        });
        connect(save, &QPushButton::clicked, this, [this] {
            g_keyBindings = editBindings;
            save_bindings();
            accept();
        });
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        refresh();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (listeningIndex < 0) {
            QDialog::keyPressEvent(event);
            return;
        }
        if (event->isAutoRepeat()) return;
        const auto keys = binding_keys();
        if (event->key() == Qt::Key_Escape) {
            editBindings[keys[listeningIndex].first].clear();
        } else {
            QString key = key_name_from_qkey(event);
            if (!key.isEmpty()) {
                std::string name = q_to_std(key);
                bool already = false;
                for (const auto& kv : editBindings) {
                    if (kv.first != keys[listeningIndex].first && normalize_key_name(kv.second) == name) {
                        already = true;
                        break;
                    }
                }
                if (!(already && setupMode)) {
                    if (!setupMode && macro_entry_hotkey_conflicts(name, -1)) {
                        QMessageBox::information(this, "Key Conflict",
                            std_to_q("The key " + name + " is already used by a macro."));
                        listeningIndex = -1;
                        refresh();
                        return;
                    }
                    for (auto& kv : editBindings) if (normalize_key_name(kv.second) == name) kv.second.clear();
                    editBindings[keys[listeningIndex].first] = name;
                } else {
                    refresh();
                    valueLabels[listeningIndex]->setText("...");
                    return;
                }
            }
        }
        refresh();
        if (setupMode) {
            ++listeningIndex;
            if (listeningIndex < (int)keys.size()) {
                valueLabels[listeningIndex]->setText("...");
                return;
            }
        }
        listeningIndex = -1;
        setupMode = false;
    }

private:
    std::unordered_map<std::string, std::string> editBindings;
    std::vector<QLabel*> valueLabels;
    int listeningIndex = -1;
    bool setupMode = false;

    void addRow(QGridLayout* grid, int row, int col, int index, const std::string& name) {
        QLabel* label = new QLabel(std_to_q(name), this);
        label->setAlignment(Qt::AlignCenter);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        label->setFont(mono);
        QLabel* value = new QLabel(this);
        value->setFrameShape(QFrame::StyledPanel);
        value->setAlignment(Qt::AlignCenter);
        value->setMinimumWidth(104);
        value->setFont(mono);
        QPushButton* change = new QPushButton("Change", this);
        change->setMinimumWidth(66);
        connect(change, &QPushButton::clicked, this, [this, index] {
            listeningIndex = index;
            setupMode = false;
            refresh();
            valueLabels[index]->setText("...");
            setFocus();
        });
        while ((int)valueLabels.size() <= index) valueLabels.push_back(nullptr);
        valueLabels[index] = value;
        grid->addWidget(label, row, col);
        grid->addWidget(value, row, col + 1);
        grid->addWidget(change, row, col + 2);
    }

    void refresh() {
        auto keys = binding_keys();
        for (int i = 0; i < (int)keys.size(); ++i) {
            auto it = editBindings.find(keys[i].first);
            valueLabels[i]->setText(it == editBindings.end() ? "" : std_to_q(it->second));
        }
    }
};

static bool validate_macro_hotkey_for_entry_qt(const std::string& hotkey, int skip_index, QWidget* parent) {
    std::string conflict;
    if (macro_hotkey_conflicts(hotkey, &conflict)) {
        QMessageBox::warning(parent, "Macro keybind", std_to_q("Macro keybind conflicts with keyboard binding: " + conflict));
        return false;
    }
    if (macro_entry_hotkey_conflicts(hotkey, skip_index, &conflict)) {
        QMessageBox::warning(parent, "Macro keybind", std_to_q("Macro keybind is already used by: " + conflict));
        return false;
    }
    return true;
}

static std::string macro_safe_file_stem(const std::string& raw_name) {
    std::string name = ns::macro::trim(raw_name);
    if (name.empty()) name = "Macro";
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) c = '_';
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty()) name = "Macro";
    if (name.size() > 180) name.resize(180);
    return name;
}

class MacroDialog : public QDialog {
public:
    explicit MacroDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle("Macros");
        setModal(false);
        setMinimumWidth(620);
        outer = new QVBoxLayout(this);
        rows = new QGridLayout();
        rows->setHorizontalSpacing(4);
        rows->setVerticalSpacing(4);
        outer->addLayout(rows);
        controls = new QGridLayout();
        importBtn = new QPushButton("Import", this);
        recordBtn = new QPushButton("Record P1", this);
        exportBtn = new QPushButton("Export", this);
        closeBtn = new QPushButton("Close", this);
        controls->addWidget(importBtn, 0, 0);
        controls->addWidget(recordBtn, 0, 4);
        controls->addWidget(exportBtn, 1, 0);
        controls->addWidget(closeBtn, 1, 4);
        outer->addLayout(controls);
        recordTimer = new QTimer(this);
        recordTimer->setInterval(16);
        connect(recordTimer, &QTimer::timeout, this, [] { macro_record_sample_p1(); });
        connect(importBtn, &QPushButton::clicked, this, [this] { importMacros(); });
        connect(exportBtn, &QPushButton::clicked, this, [this] { exportMacros(); });
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
        connect(recordBtn, &QPushButton::clicked, this, [this] { toggleRecord(); });
        rebuild();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (listeningMacro < 0) {
            QDialog::keyPressEvent(event);
            return;
        }
        bool changed = false;
        if (listeningMacro >= 0 && listeningMacro < (int)g_macro_entries.size()) {
            if (event->key() == Qt::Key_Escape) {
                std::lock_guard<std::mutex> lk(g_macro_mtx);
                if (listeningMacro >= 0 && listeningMacro < (int)g_macro_entries.size()) {
                    g_macro_entries[listeningMacro].hotkey.clear();
                    rebuild_macro_hotkey_state();
                    changed = true;
                }
            } else {
                QString key = key_name_from_qkey(event);
                if (!key.isEmpty()) {
                    std::string name = normalize_key_name(q_to_std(key));
                    if (validate_macro_hotkey_for_entry_qt(name, listeningMacro, this)) {
                        std::lock_guard<std::mutex> lk(g_macro_mtx);
                        if (listeningMacro >= 0 && listeningMacro < (int)g_macro_entries.size()) {
                            g_macro_entries[listeningMacro].hotkey = name;
                            rebuild_macro_hotkey_state();
                            changed = true;
                        }
                    }
                }
            }
        }
        if (changed) save_macro_entries_to_disk();
        listeningMacro = -1;
        rebuild();
    }

    void closeEvent(QCloseEvent* event) override {
        if (recording) {
            macro_record_stop();
            recording = false;
            recordTimer->stop();
        }
        listeningMacro = -1;
        QDialog::closeEvent(event);
    }

private:
    QVBoxLayout* outer = nullptr;
    QGridLayout* rows = nullptr;
    QGridLayout* controls = nullptr;
    QPushButton* importBtn = nullptr;
    QPushButton* recordBtn = nullptr;
    QPushButton* exportBtn = nullptr;
    QPushButton* closeBtn = nullptr;
    QTimer* recordTimer = nullptr;
    bool recording = false;
    int listeningMacro = -1;

    void clearRows() {
        while (QLayoutItem* item = rows->takeAt(0)) {
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }
    }

    void rebuild() {
        clearRows();
        std::vector<ns::macro::Entry> entries;
        {
            std::lock_guard<std::mutex> lk(g_macro_mtx);
            entries = g_macro_entries;
        }
        if (entries.empty()) {
            QLabel* empty = new QLabel("No macros", this);
            empty->setMinimumWidth(250);
            rows->addWidget(empty, 0, 0, 1, 5);
        }
        for (int i = 0; i < (int)entries.size(); ++i) {
            const auto& e = entries[i];
            QPushButton* run = new QPushButton(std_to_q(e.name.empty() ? "Macro" : e.name), this);
            QPushButton* key = new QPushButton(std_to_q(listeningMacro == i ? "..." : normalize_key_name(e.hotkey)), this);
            QPushButton* rename = new QPushButton("Rename", this);
            QPushButton* exp = new QPushButton("Export", this);
            QPushButton* del = new QPushButton("Delete", this);
            run->setMinimumWidth(250);
            key->setMinimumWidth(110);
            rows->addWidget(run, i, 0);
            rows->addWidget(key, i, 1);
            rows->addWidget(rename, i, 2);
            rows->addWidget(exp, i, 3);
            rows->addWidget(del, i, 4);
            connect(run, &QPushButton::clicked, this, [this, i] {
                std::string json;
                {
                    std::lock_guard<std::mutex> lk(g_macro_mtx);
                    if (i >= 0 && i < (int)g_macro_entries.size()) json = g_macro_entries[i].json;
                }
                std::string err;
                if (!start_macro_text(json, &err)) QMessageBox::warning(this, "Macro validation", std_to_q("Invalid macro: " + err));
            });
            connect(key, &QPushButton::clicked, this, [this, i] {
                listeningMacro = i;
                rebuild();
                setFocus();
            });
            connect(rename, &QPushButton::clicked, this, [this, i] { renameMacro(i); });
            connect(exp, &QPushButton::clicked, this, [this, i] { exportOne(i); });
            connect(del, &QPushButton::clicked, this, [this, i] {
                {
                    std::lock_guard<std::mutex> lk(g_macro_mtx);
                    if (i >= 0 && i < (int)g_macro_entries.size()) g_macro_entries.erase(g_macro_entries.begin() + i);
                    rebuild_macro_hotkey_state();
                }
                save_macro_entries_to_disk();
                rebuild();
            });
        }
        recordBtn->setText(recording ? "Stop" : "Record P1");
    }

    void renameMacro(int idx) {
        std::string old_name;
        {
            std::lock_guard<std::mutex> lk(g_macro_mtx);
            if (idx < 0 || idx >= (int)g_macro_entries.size()) return;
            old_name = g_macro_entries[idx].name.empty() ? "Macro" : g_macro_entries[idx].name;
        }
        bool ok = false;
        QString text = QInputDialog::getText(this, "Rename Macro", "Macro name:", QLineEdit::Normal, std_to_q(old_name), &ok);
        if (!ok) return;
        std::string new_name = ns::macro::trim(q_to_std(text));
        if (new_name.empty()) {
            QMessageBox::warning(this, "Rename Macro", "Macro name cannot be empty.");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_macro_mtx);
            int duplicate = find_macro_entry_by_name(new_name);
            if (duplicate >= 0 && duplicate != idx) {
                QMessageBox::warning(this, "Rename Macro", "Another macro already uses that name.");
                return;
            }
            g_macro_entries[idx].name = new_name;
            g_macro_entries[idx].json = ns::macro::pretty_json_with_forced_name(g_macro_entries[idx].json, new_name);
        }
        save_macro_entries_to_disk();
        rebuild();
    }

    void importMacros() {
        QString path = QFileDialog::getOpenFileName(this, "Import Macros JSON", QString(), "JSON files (*.json);;All files (*)");
        if (path.isEmpty()) return;
        std::string err;
        std::string raw = ns::macro::read_text_file_limited(q_to_std(path), &err);
        if (raw.empty()) {
            QMessageBox::warning(this, "Macro validation", std_to_q(err.empty() ? "Invalid or empty macro file." : err));
            return;
        }
        std::vector<ns::macro::Entry> imported;
        if (!ns::macro::parse_entries_text(raw, imported, err, normalize_macro_hotkey_for_io) || imported.empty()) {
            QMessageBox::warning(this, "Macro validation", std_to_q("Invalid macro JSON: " + err));
            return;
        }
        if (imported.size() > 1 || raw.find("\"macros\"") != std::string::npos) {
            {
                std::lock_guard<std::mutex> lk(g_macro_mtx);
                g_macro_entries = std::move(imported);
                rebuild_macro_hotkey_state();
            }
        } else {
            ns::macro::Entry e = imported[0];
            std::string upsert_err;
            if (!upsert_macro_entry(e, false, &upsert_err)) {
                QMessageBox::warning(this, "Macro validation", std_to_q("Invalid macro: " + upsert_err));
                return;
            }
        }
        save_macro_entries_to_disk();
        rebuild();
    }

    void exportMacros() {
        save_macro_entries_to_disk();
        QString path = QFileDialog::getSaveFileName(this, "Export Macros JSON", "ns-macros.json", "JSON files (*.json);;All files (*)");
        if (path.isEmpty()) return;
        std::string json;
        {
            std::lock_guard<std::mutex> lk(g_macro_mtx);
            json = ns::macro::entries_to_json(g_macro_entries, normalize_macro_hotkey_for_io);
        }
        std::ofstream f(q_to_std(path), std::ios::binary | std::ios::trunc);
        if (!f.write(json.data(), (std::streamsize)json.size()))
            QMessageBox::warning(this, "Export Macros JSON", "Could not export macro JSON.");
    }

    void exportOne(int idx) {
        std::vector<ns::macro::Entry> one;
        std::string name = "Macro";
        {
            std::lock_guard<std::mutex> lk(g_macro_mtx);
            if (idx < 0 || idx >= (int)g_macro_entries.size()) return;
            one.push_back(g_macro_entries[idx]);
            name = one[0].name.empty() ? "Macro" : one[0].name;
        }
        QString default_name = std_to_q(macro_safe_file_stem(name) + ".json");
        QString path = QFileDialog::getSaveFileName(this, "Export Macros JSON", default_name, "JSON files (*.json);;All files (*)");
        if (path.isEmpty()) return;
        std::string json = ns::macro::entries_to_json(one, normalize_macro_hotkey_for_io);
        std::ofstream f(q_to_std(path), std::ios::binary | std::ios::trunc);
        if (!f.write(json.data(), (std::streamsize)json.size()))
            QMessageBox::warning(this, "Export Macros JSON", "Could not export macro JSON.");
    }

    void toggleRecord() {
        if (!recording) {
            macro_record_start();
            macro_record_sample_p1();
            recording = true;
            recordTimer->start();
            recordBtn->setText("Stop");
        } else {
            macro_record_sample_p1();
            std::string recorded = macro_record_stop();
            recording = false;
            recordTimer->stop();
            if (!recorded.empty()) {
                ns::macro::Entry e;
                e.name = "Recorded Macro";
                e.hotkey = "";
                e.json = recorded;
                std::string err;
                upsert_macro_entry(e, true, &err);
                save_macro_entries_to_disk();
            }
            rebuild();
        }
    }
};

class MainWindow : public QWidget {
public:
    explicit MainWindow() {
        setWindowTitle("NS PC Control");
        setWindowIcon(app_icon());
        setFixedSize(platformWidth(), platformHeight());
        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(16, 12, 16, 14);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(8);

        title = new QLabel("NS PC Control", this);
        QFont titleFont = title->font();
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
        title->setFont(titleFont);
        grid->addWidget(title, 0, 0, 1, 4);

        auto* ipLabel = new QLabel("Raspberry Pi IP:", this);
        ipLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ipEdit = new QLineEdit(std_to_q(load_saved_ip()), this);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        ipEdit->setFont(mono);
        grid->addWidget(ipLabel, 1, 0);
        grid->addWidget(ipEdit, 1, 1, 1, 3);

        auto* kbLabel = new QLabel("Keyboard Mode:", this);
        kbLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        keyboardCombo = new QComboBox(this);
        keyboardCombo->addItem("OFF");
        keyboardCombo->addItem("ON (single)");
        keyboardCombo->addItem("ON (override)");
        int savedMode = load_saved_keyboard_mode();
        g_keyboardMode.store(savedMode);
        keyboardCombo->setCurrentIndex(savedMode);
        bindingsBtn = new QPushButton("Bindings...", this);
        bindingsBtn->setEnabled(savedMode != KB_OFF);
        grid->addWidget(kbLabel, 2, 0);
        grid->addWidget(keyboardCombo, 2, 1, 1, 2);
        grid->addWidget(bindingsBtn, 2, 3);

        macrosBtn = new QPushButton("Macros...", this);
        grid->addWidget(macrosBtn, 3, 1, 1, 2);

        connectBtn = new QPushButton("Connect", this);
        quitBtn = new QPushButton("Quit", this);
        grid->addWidget(connectBtn, 4, 1);
        grid->addWidget(quitBtn, 4, 3);

        auto* sep = new QFrame(this);
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Sunken);
        grid->addWidget(sep, 5, 0, 1, 4);

        statusLabel = new QLabel("Ready", this);
        grid->addWidget(statusLabel, 6, 0, 1, 4);

        for (int i = 0; i < 4; ++i) {
            padLabels[i] = new QLabel(std_to_q("P" + std::to_string(i + 1) + ": Not connected"), this);
            padLabels[i]->setIndent(10);
            grid->addWidget(padLabels[i], 7 + i, 0, 1, 4);
        }

        connect(keyboardCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
            g_keyboardMode.store(idx);
            save_keyboard_mode(idx);
            bindingsBtn->setEnabled(idx != KB_OFF && !g_connected.load());
        });
        connect(bindingsBtn, &QPushButton::clicked, this, [this] {
            BindingsDialog dlg(this);
            dlg.exec();
        });
        connect(macrosBtn, &QPushButton::clicked, this, [this] {
            load_macro_entries();
            auto* dlg = new MacroDialog(this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->show();
        });
        connect(connectBtn, &QPushButton::clicked, this, [this] { toggleConnection(); });
        connect(quitBtn, &QPushButton::clicked, qApp, &QApplication::quit);

        timer = new QTimer(this);
        timer->setInterval(100);
        connect(timer, &QTimer::timeout, this, [this] { updateUi(); });
        timer->start();
        load_saved_bindings();
        load_macro_entries();
        g_sdlInput.start();
        updateUi();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        QString k = key_name_from_qkey(event);
        if (!k.isEmpty()) set_key_pressed(q_to_std(k), true);
        QWidget::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        QString k = key_name_from_qkey(event);
        if (!k.isEmpty()) set_key_pressed(q_to_std(k), false);
        QWidget::keyReleaseEvent(event);
    }

    void closeEvent(QCloseEvent* event) override {
        stop_connection();
        QWidget::closeEvent(event);
    }

private:
    QLabel* title = nullptr;
    QLineEdit* ipEdit = nullptr;
    QComboBox* keyboardCombo = nullptr;
    QPushButton* bindingsBtn = nullptr;
    QPushButton* macrosBtn = nullptr;
    QPushButton* connectBtn = nullptr;
    QPushButton* quitBtn = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* padLabels[4]{};
    QTimer* timer = nullptr;

    static int platformWidth() {
#ifdef _WIN32
        return 410;
#elif defined(__APPLE__)
        return 420;
#else
        return 400;
#endif
    }

    static int platformHeight() {
#ifdef _WIN32
        return 345;
#elif defined(__APPLE__)
        return 345;
#else
        return 320;
#endif
    }

    void toggleConnection() {
        if (g_connected.load()) {
            stop_connection();
            connectBtn->setText("Connect");
            ipEdit->setEnabled(true);
            keyboardCombo->setEnabled(true);
            bindingsBtn->setEnabled(g_keyboardMode.load() != KB_OFF);
            statusLabel->setText("Disconnected");
            updateUi();
            return;
        }
        std::string err;
        if (!start_connection(q_to_std(ipEdit->text()), &err)) {
            QMessageBox::critical(this, "Error", std_to_q(err));
            return;
        }
        connectBtn->setText("Disconnect");
        ipEdit->setEnabled(false);
        keyboardCombo->setEnabled(false);
        bindingsBtn->setEnabled(false);
    }

    void updateUi() {
        const bool connected = g_connected.load();
        if (!connected) g_sdlInput.poll();
        connectBtn->setText(connected ? "Disconnect" : "Connect");
        ipEdit->setEnabled(!connected);
        keyboardCombo->setEnabled(!connected);
        bindingsBtn->setEnabled(!connected && g_keyboardMode.load() != KB_OFF);
        statusLabel->setText(std_to_q(status_message()));
        auto sdl = g_sdlInput.snapshot();
        std::string sdlErr = g_sdlInput.error();
        int km = g_keyboardMode.load();
        int shifted_p1_target = -1;
        if (km == KB_SINGLE && sdl[0].connected) {
            for (int s = 1; s < 4; ++s) {
                if (!sdl[s].connected) { shifted_p1_target = s; break; }
            }
        }
        for (int i = 0; i < 4; ++i) {
            QString text;
            if (i == 0 && km != KB_OFF) {
                text = km == KB_SINGLE ? "P1: Keyboard" : (sdl[0].connected ? "P1: SDL3 Controller / Keyboard" : "P1: Idle / Keyboard");
            } else if (i == shifted_p1_target) {
                text = std_to_q("P" + std::to_string(i + 1) + ": " + (sdl[0].name.empty() ? "SDL3 Gamepad" : sdl[0].name) + " (Shifted)");
            } else if (sdl[i].connected) {
                text = std_to_q("P" + std::to_string(i + 1) + ": " + (sdl[i].name.empty() ? "SDL3 Gamepad" : sdl[i].name) +
                                (sdl[i].has_motion ? " + gyro" : ""));
            } else if (!sdlErr.empty() && i == 0) {
                text = "P1: SDL3 error";
            } else {
                text = std_to_q("P" + std::to_string(i + 1) + ": Not connected");
            }
            padLabels[i]->setText(text);
        }
    }
};
static bool has_cli_flag(const std::vector<std::string>& args) {
    return std::find(args.begin(), args.end(), "--cli") != args.end();
}

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");
    if (has_cli_flag(args)) {
#ifdef _WIN32
        AttachConsole(ATTACH_PARENT_PROCESS);
#endif
        return cli_main(args);
    }
    NetworkRuntime net;
    if (!net.good()) return 1;
    raise_process_priority();
    QApplication app(argc, argv);
    QApplication::setApplicationName("NS PC Control");
    QApplication::setOrganizationName("NSPCControl");
    QApplication::setWindowIcon(app_icon());
#if defined(__APPLE__)
    if (QStyle* style = QStyleFactory::create("macOS")) QApplication::setStyle(style);
#elif defined(_WIN32)
    if (QStyle* style = QStyleFactory::create("windowsvista")) QApplication::setStyle(style);
#endif
    MainWindow window;
    window.show();
    return app.exec();
}
