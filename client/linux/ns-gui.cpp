/// ns-gui.cpp  —  GTK3 GUI frontend for the Switch wireless gamepad bridge
///
/// Features Smart Discovery: Automatically scans js0-js15, ignores mice,
/// and natively supports up to 4 local controllers packed into a single UDP stream.
///
/// Build:
///   g++ -O3 -std=c++17 ns-gui.cpp -o ns-gui \
///       $(pkg-config --cflags --libs gtk+-3.0) -lpthread
///
/// Usage:
///   ./ns-gui

#include <gtk/gtk.h>
#include <glib.h>

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <sstream>
#include <fstream>
#include <cerrno>


#include <SDL2/SDL.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/resource.h>

#include "../../server/rpi/include/sha256.h"

// Import external protocol structures (Version 4 with MultiReport + ExtendedMultiReport)
#include "../../server/rpi/include/protocol.hpp"

static constexpr uint8_t EXT_PAD_PRESENT = 0x01;

// ── Macro support ───────────────────────────────────────────────────────────
struct MacroStep {
    uint16_t buttons = 0;
    uint32_t duration_ms = 0;
};

static std::string macro_trim(std::string s) {
    auto not_space = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::string macro_upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

static std::string macro_read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string macro_unescape_json_string(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else out += n;
        } else out += s[i];
    }
    return out;
}

static std::string macro_extract_commands_text(const std::string& raw) {
    size_t key = raw.find("\"commands\"");
    if (key == std::string::npos) key = raw.find("commands");
    if (key != std::string::npos) {
        size_t colon = raw.find(':', key);
        if (colon != std::string::npos) {
            size_t q1 = raw.find('"', colon + 1);
            if (q1 != std::string::npos) {
                std::string val;
                bool esc = false;
                for (size_t i = q1 + 1; i < raw.size(); ++i) {
                    char c = raw[i];
                    if (esc) { val += '\\'; val += c; esc = false; continue; }
                    if (c == '\\') { esc = true; continue; }
                    if (c == '"') return macro_unescape_json_string(val);
                    val += c;
                }
            }
        }
    }
    return raw;
}

static uint16_t macro_button_bit(std::string name) {
    name = macro_upper(macro_trim(name));
    if (name == "A" || name == "BTN_A") return ns::BTN_A;
    if (name == "B" || name == "BTN_B") return ns::BTN_B;
    if (name == "X" || name == "BTN_X") return ns::BTN_X;
    if (name == "Y" || name == "BTN_Y") return ns::BTN_Y;
    if (name == "L" || name == "BTN_L") return ns::BTN_L;
    if (name == "R" || name == "BTN_R") return ns::BTN_R;
    if (name == "ZL" || name == "BTN_ZL") return ns::BTN_ZL;
    if (name == "ZR" || name == "BTN_ZR") return ns::BTN_ZR;
    if (name == "MINUS" || name == "-" || name == "BTN_MINUS") return ns::BTN_MINUS;
    if (name == "PLUS" || name == "+" || name == "BTN_PLUS") return ns::BTN_PLUS;
    if (name == "LSTICK" || name == "LS" || name == "BTN_LSTICK") return ns::BTN_LSTICK;
    if (name == "RSTICK" || name == "RS" || name == "BTN_RSTICK") return ns::BTN_RSTICK;
    if (name == "HOME" || name == "BTN_HOME") return ns::BTN_HOME;
    if (name == "CAPTURE" || name == "BTN_CAPTURE") return ns::BTN_CAPTURE;
    return 0;
}

static uint16_t macro_parse_buttons(std::string combo) {
    for (char& c : combo) if (c == '+' || c == ',' || c == '|') c = ' ';
    std::istringstream iss(combo);
    std::string tok;
    uint16_t buttons = 0;
    while (iss >> tok) buttons |= macro_button_bit(tok);
    return buttons;
}

static std::vector<MacroStep> macro_parse_text(const std::string& raw_text) {
    std::string text = macro_extract_commands_text(raw_text);
    for (char& c : text) if (c == '\n' || c == '\r') c = ';';
    std::vector<MacroStep> steps;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t semi = text.find(';', pos);
        std::string part = macro_trim(text.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
        pos = (semi == std::string::npos) ? text.size() : semi + 1;
        if (part.empty()) continue;
        std::istringstream iss(part);
        std::string cmd;
        uint32_t ms = 0;
        iss >> cmd >> ms;
        if (cmd.empty() || ms == 0) continue;
        MacroStep st;
        if (macro_upper(cmd) == "WAIT") st.buttons = 0;
        else st.buttons = macro_parse_buttons(cmd);
        st.duration_ms = ms;
        steps.push_back(st);
    }
    return steps;
}

static std::vector<MacroStep> macro_load_file(const std::string& path) {
    return macro_parse_text(macro_read_file(path));
}

static uint64_t macro_total_ms(const std::vector<MacroStep>& steps) {
    uint64_t total = 0;
    for (const auto& s : steps) total += s.duration_ms;
    return total;
}

static bool macro_report_at(const std::vector<MacroStep>& steps, uint64_t elapsed_ms, ns::HIDReport& out) {
    out.reset();
    uint64_t t = 0;
    for (const auto& s : steps) {
        if (elapsed_ms < t + s.duration_ms) {
            out.buttons = s.buttons;
            return true;
        }
        t += s.duration_ms;
    }
    return false;
}


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

// ── Config path helpers ──
static std::string get_config_dir() {
    const char* home = getenv("HOME");
    if (!home) return ".";
    return std::string(home) + "/.config/ns-pc-control";
}

static std::string load_saved_config() {
    std::string path = get_config_dir() + "/config";
    char buf[256]{};
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    if (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
    }
    fclose(f);
    return buf;
}

static void save_config(const char* full) {
    std::string dir = get_config_dir();
    if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) return;
    std::string path = dir + "/config";
    FILE* f = fopen(path.c_str(), "w");
    if (f) { fputs(full, f); fputc('\n', f); fclose(f); }
}

// ── Global state ──
static GtkWidget* ipEntry = nullptr;
static GtkWidget* connectBtn = nullptr;
static GtkWidget* macroBtn = nullptr;
static GtkWidget* statusLabel = nullptr;
static GtkWidget* ctrlLabels[4]; // Labels to display P1 to P4 status

static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_senderRunning{false};
static std::thread g_senderThread;
static uint8_t g_hmacKey[32]{};

// ── Shared Gamepad State (SDL2) ──
static SDL_GameController* g_pads[4] = {nullptr, nullptr, nullptr, nullptr};
static char         g_hw_names[4][128]; // Stored safely for the GTK thread to read
static std::mutex   g_hw_mtx;           // Protects hardware string arrays
static bool         g_pad_accel_enabled[4] = {false, false, false, false};
static bool         g_pad_gyro_enabled[4]  = {false, false, false, false};
static bool         g_legacy_udp = false; // hidden fallback: NSPC_LEGACY_UDP=1


static std::mutex g_macro_mtx;
static std::vector<MacroStep> g_macro_steps;
static bool g_macro_running = false;
static uint64_t g_macro_start_us = 0;
static std::string g_macro_text;
static std::string macros_file_path() { return get_config_dir() + "/macros.json"; }
static void load_macro_text() { g_macro_text = macro_read_file(macros_file_path()); }
static void save_macro_text(const std::string& txt) { std::string dir = get_config_dir(); g_mkdir_with_parents(dir.c_str(), 0755); FILE* f = fopen(macros_file_path().c_str(), "w"); if (f) { fwrite(txt.data(), 1, txt.size(), f); fclose(f); } g_macro_text = txt; }
static void start_macro_text(const std::string& txt) { std::lock_guard<std::mutex> lk(g_macro_mtx); g_macro_steps = macro_parse_text(txt); g_macro_running = !g_macro_steps.empty(); g_macro_start_us = ns::now_us(); }
static bool g_macro_recording = false;
static uint16_t g_macro_record_last_buttons = 0xFFFF;
static uint64_t g_macro_record_last_change_us = 0;
static std::string g_macro_record_commands;
static std::string macro_buttons_to_text(uint16_t buttons) {
    struct BtnName { uint16_t bit; const char* name; } names[] = {
        {ns::BTN_A,"A"},{ns::BTN_B,"B"},{ns::BTN_X,"X"},{ns::BTN_Y,"Y"},
        {ns::BTN_L,"L"},{ns::BTN_R,"R"},{ns::BTN_ZL,"ZL"},{ns::BTN_ZR,"ZR"},
        {ns::BTN_MINUS,"MINUS"},{ns::BTN_PLUS,"PLUS"},{ns::BTN_LSTICK,"LSTICK"},{ns::BTN_RSTICK,"RSTICK"},
        {ns::BTN_HOME,"HOME"},{ns::BTN_CAPTURE,"CAPTURE"},
    };
    std::string out;
    for (const auto& n : names) if (buttons & n.bit) { if (!out.empty()) out += "+"; out += n.name; }
    return out;
}
static void macro_record_append_locked(uint16_t buttons, uint64_t duration_ms) {
    if (duration_ms < 10) return;
    if (!g_macro_record_commands.empty()) g_macro_record_commands += "; ";
    if (buttons == 0) g_macro_record_commands += "WAIT " + std::to_string(duration_ms);
    else g_macro_record_commands += macro_buttons_to_text(buttons) + " " + std::to_string(duration_ms);
}
static void start_macro_recording() {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_recording = true;
    g_macro_record_last_buttons = 0xFFFF;
    g_macro_record_last_change_us = ns::now_us();
    g_macro_record_commands.clear();
}
static std::string stop_macro_recording() {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    if (g_macro_recording && g_macro_record_last_buttons != 0xFFFF)
        macro_record_append_locked(g_macro_record_last_buttons, (ns::now_us() - g_macro_record_last_change_us) / 1000ULL);
    g_macro_recording = false;
    g_macro_record_last_buttons = 0xFFFF;
    std::string commands = g_macro_record_commands.empty() ? "WAIT 200" : g_macro_record_commands;
    return std::string("{\"name\":\"Recorded Macro\",\"commands\":\"") + commands + "\"}";
}
static void sample_macro_recording(const ns::HIDReport& report) {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    if (!g_macro_recording || g_macro_running) return;
    uint64_t now = ns::now_us();
    uint16_t buttons = report.buttons;
    if (g_macro_record_last_buttons == 0xFFFF) {
        g_macro_record_last_buttons = buttons;
        g_macro_record_last_change_us = now;
        return;
    }
    if (buttons != g_macro_record_last_buttons) {
        macro_record_append_locked(g_macro_record_last_buttons, (now - g_macro_record_last_change_us) / 1000ULL);
        g_macro_record_last_buttons = buttons;
        g_macro_record_last_change_us = now;
    }
}
static bool apply_macro_override(ns::HIDReport reports[4], bool present[4]) {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    if (!g_macro_running) return false;
    uint64_t elapsed_ms = (ns::now_us() - g_macro_start_us) / 1000ULL;
    ns::HIDReport mr;
    bool active = macro_report_at(g_macro_steps, elapsed_ms, mr);
    for (int i = 0; i < 4; ++i) { reports[i].reset(); present[i] = false; }
    reports[0] = mr;
    present[0] = true;
    if (!active && elapsed_ms > macro_total_ms(g_macro_steps) + 120) g_macro_running = false;
    return true;
}

// ── Axis conversion ──
static uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    else scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

static int16_t clamp_i16_from_float(float v) {
    if (v < -32768.0f) return -32768;
    if (v >  32767.0f) return  32767;
    return (int16_t)std::lrintf(v);
}

static void set_pad_present_flag(ns::ExtendedHIDReport& r, bool present) {
    // ExtendedHIDReport starts with HIDReport. Byte +7 is the pad-present
    // flag used by the backend/web protocol, so neutral connected pads still
    // claim a Switch port and can receive rumble.
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

// ── SDL2 Discovery, Input, Sensors, Rumble ──

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
#else
    (void)slot;
    (void)pad;
#endif
}

static void scan_for_gamepads() {
    static uint64_t last_scan = 0;
    uint64_t now = ns::now_us();

    SDL_GameControllerUpdate();

    if (now - last_scan < 1'000'000) return;
    last_scan = now;

    int num = SDL_NumJoysticks();
    for (int i = 0; i < num; ++i) {
        if (!SDL_IsGameController(i)) continue;

        // Check by instance ID to avoid double-mapping
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

        // Find a free slot
        for (int p = 0; p < 4; ++p) {
            if (!g_pads[p]) {
                SDL_GameController* pad = SDL_GameControllerOpen(i);
                if (!pad) break;
                {
                    std::lock_guard<std::mutex> lock(g_hw_mtx);
                    g_pads[p] = pad;
                    const char* name = SDL_GameControllerName(pad);
                    strncpy(g_hw_names[p], name ? name : "Unknown", sizeof(g_hw_names[p]) - 1);
                    g_hw_names[p][sizeof(g_hw_names[p]) - 1] = '\0';
                    enable_pad_sensors(p, pad);
                }
                break;
            }
        }
    }
}

// ── Read Controller State (SDL2) ──
static void read_pad(int index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    SDL_GameController* pad = g_pads[index];
    if (!pad) { conn = false; return; }

    if (!SDL_GameControllerGetAttached(pad)) {
        std::lock_guard<std::mutex> lock(g_hw_mtx);
        SDL_GameControllerClose(pad);
        g_pads[index] = nullptr;
        g_hw_names[index][0] = '\0';
        g_pad_accel_enabled[index] = false;
        g_pad_gyro_enabled[index] = false;
        conn = false;
        return;
    }

    conn = true;

    // Standardised button mapping (Xbox physical -> Switch)
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) rep.buttons |= ns::BTN_B;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) rep.buttons |= ns::BTN_A;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) rep.buttons |= ns::BTN_Y;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) rep.buttons |= ns::BTN_X;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) rep.buttons |= ns::BTN_L;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) rep.buttons |= ns::BTN_R;

    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)   > 16000) rep.buttons |= ns::BTN_ZL;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)  > 16000) rep.buttons |= ns::BTN_ZR;

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

    // Analog sticks — SDL sets UP as negative, Switch expects UP as positive
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
            // SDL gyro is rad/s. Keep the scale conservative, matching the other clients.
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

// ── Network Sender Thread ──
static void SenderThread(std::string host, uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        close(sock); return;
    }

    struct sockaddr_in dest{};
    memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    uint32_t seq = 0;
    auto next_tick = std::chrono::steady_clock::now();
    RumbleManager rumble;

    while (g_senderRunning.load()) {
        while (std::chrono::steady_clock::now() < next_tick)
            std::this_thread::sleep_for(std::chrono::microseconds(200));

        scan_for_gamepads();

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

        sample_macro_recording(reports[0]);
        if (apply_macro_override(reports, present)) active_count = 1;

        if (g_legacy_udp) {
            ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet));
            pkt.magic         = ns::PROTO_MAGIC;
            pkt.version       = ns::PROTO_VERSION;
            pkt.flags         = ns::FLAG_NONE;
            pkt.seq           = seq++;
            pkt.ts_us         = ns::now_us();
            pkt.report.reset();

            ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
            for (int i = 0; i < 4; ++i)
                *out_reports[i] = reports[i];

            uint8_t full_hmac[32];
            hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
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
            hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, EXT_UDP_PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);

            sendto(sock, (const char*)&pkt, (int)sizeof(pkt), 0, (struct sockaddr*)&dest, sizeof(dest));

            pump_udp_rumble(sock, rumble, controller_for_slot);
            rumble.update_timeouts(controller_for_slot);
        }

        if (active_count > 0) next_tick += std::chrono::milliseconds(4);
        else next_tick += std::chrono::milliseconds(50);
    }


    for (int i = 0; i < 4; ++i) {
        if (g_pads[i]) {
            set_pad_rumble(i, 0, 0, 0);
            SDL_GameControllerClose(g_pads[i]);
            g_pads[i] = nullptr;
            g_hw_names[i][0] = '\0';
            g_pad_accel_enabled[i] = false;
            g_pad_gyro_enabled[i] = false;
        }
    }
    close(sock);
}



extern "C" void on_macros_clicked(GtkWidget*, gpointer) {
    load_macro_text();
    GtkWidget* dlg = gtk_dialog_new_with_buttons("Macros", nullptr, GTK_DIALOG_MODAL,
        "Run", 1, "Save/Add JSON", 2, "Delete", 3, "Record", 4, "Stop Recording", 5, "Close", GTK_RESPONSE_CLOSE, nullptr);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* label = gtk_label_new("Use JSON like {\"name\":\"Boost\",\"commands\":\"WAIT 200; A 100; B 100\"}, or record live P1 buttons while connected.");
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_container_add(GTK_CONTAINER(area), label);
    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(scroll, 520, 180);
    GtkWidget* text = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(scroll), text);
    gtk_container_add(GTK_CONTAINER(area), scroll);
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
    gtk_text_buffer_set_text(buf, g_macro_text.empty() ? "{\"name\":\"Macro\",\"commands\":\"WAIT 200; A 100; B 100\"}" : g_macro_text.c_str(), -1);
    gtk_widget_show_all(dlg);
    while (true) {
        int r = gtk_dialog_run(GTK_DIALOG(dlg));
        GtkTextIter a,b; gtk_text_buffer_get_bounds(buf, &a, &b);
        char* raw = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
        std::string txt = raw ? raw : ""; if (raw) g_free(raw);
        if (r == 1) start_macro_text(txt);
        else if (r == 2) save_macro_text(txt);
        else if (r == 3) save_macro_text("");
        else if (r == 4) { start_macro_recording(); gtk_text_buffer_set_text(buf, "Recording... play on P1, then press Stop Recording.", -1); }
        else if (r == 5) { std::string rec = stop_macro_recording(); save_macro_text(rec); gtk_text_buffer_set_text(buf, rec.c_str(), -1); }
        else break;
    }
    gtk_widget_destroy(dlg);
}

// ── GTK Callbacks ──
extern "C" void on_connect_clicked(GtkWidget*, gpointer) {
    if (g_connected) {
        g_connected = false;
        g_senderRunning = false;
        if (g_senderThread.joinable()) g_senderThread.join();
        
        gtk_button_set_label(GTK_BUTTON(connectBtn), "Connect");
        gtk_widget_set_sensitive(ipEntry, TRUE);
        gtk_label_set_text(GTK_LABEL(statusLabel), "Disconnected");
        
        for (int i = 0; i < 4; ++i) {
            char buf[64]; snprintf(buf, sizeof(buf), "P%d: Waiting...", i + 1);
            gtk_label_set_text(GTK_LABEL(ctrlLabels[i]), buf);
        }
        return;
    }

    const char* ipStr = gtk_entry_get_text(GTK_ENTRY(ipEntry));
    if (strlen(ipStr) == 0) return;

    char ipBuf[64]; strncpy(ipBuf, ipStr, sizeof(ipBuf) - 1); ipBuf[sizeof(ipBuf) - 1] = '\0';
    int port = ns::DEFAULT_PORT;
    char* colon = strchr(ipBuf, ':');
    if (colon) { *colon = '\0'; port = atoi(colon + 1); if (port <= 0 || port > 65535) port = ns::DEFAULT_PORT; }

    save_config(ipStr);
    derive_key(ns::DEFAULT_SECRET, g_hmacKey);
    g_connected = true;

    for (int i=0; i<4; ++i) {
        if (g_pads[i]) {
            set_pad_rumble(i, 0, 0, 0);
            SDL_GameControllerClose(g_pads[i]);
        }
        g_hw_names[i][0] = '\0';
        g_pads[i] = nullptr;
        g_pad_accel_enabled[i] = false;
        g_pad_gyro_enabled[i] = false;
    }

    g_senderRunning = true;
    g_senderThread = std::thread(SenderThread, std::string(ipBuf), (uint16_t)port);

    gtk_button_set_label(GTK_BUTTON(connectBtn), "Disconnect");
    gtk_widget_set_sensitive(ipEntry, FALSE);

    char status[128]; snprintf(status, sizeof(status), "Connected to %s:%d", ipBuf, port);
    gtk_label_set_text(GTK_LABEL(statusLabel), status);
}

extern "C" gboolean on_timer(gpointer) {
    if (g_connected) {
        std::lock_guard<std::mutex> lock(g_hw_mtx);
        for (int i = 0; i < 4; ++i) {
            char lbl[128];
            if (g_hw_names[i][0] != '\0') snprintf(lbl, sizeof(lbl), "🎮 P%d: %s", i + 1, g_hw_names[i]);
            else snprintf(lbl, sizeof(lbl), "P%d: Waiting...", i + 1);
            gtk_label_set_text(GTK_LABEL(ctrlLabels[i]), lbl);
        }
    } else {
        // Run a silent discovery to preview what's plugged in before connecting
        scan_for_gamepads();
        std::lock_guard<std::mutex> lock(g_hw_mtx);
        for (int i = 0; i < 4; ++i) {
            char lbl[128];
            if (g_hw_names[i][0] != '\0') snprintf(lbl, sizeof(lbl), "🎮 P%d: %s", i + 1, g_hw_names[i]);
            else snprintf(lbl, sizeof(lbl), "P%d: Waiting...", i + 1);
            gtk_label_set_text(GTK_LABEL(ctrlLabels[i]), lbl);
        }
    }
    return G_SOURCE_CONTINUE;
}

static std::string get_exe_dir() {
    char buf[1024]; ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0'; std::string exe(buf);
        size_t pos = exe.find_last_of("/");
        if (pos != std::string::npos) return exe.substr(0, pos);
    }
    return ".";
}

extern "C" void on_window_destroy(GtkWidget*, gpointer) {
    if (g_connected) { g_senderRunning = false; if (g_senderThread.joinable()) g_senderThread.join(); }
    else {
        for (int i = 0; i < 4; ++i) {
            if (g_pads[i]) {
                set_pad_rumble(i, 0, 0, 0);
                SDL_GameControllerClose(g_pads[i]);
                g_pads[i] = nullptr;
                g_hw_names[i][0] = '\0';
                g_pad_accel_enabled[i] = false;
                g_pad_gyro_enabled[i] = false;
            }
        }
    }
    gtk_main_quit();
}

// ── Entry point ──
int main(int argc, char* argv[]) {
    // Elevate priority
    setpriority(PRIO_PROCESS, 0, -20);

    const char* legacy_env = getenv("NSPC_LEGACY_UDP");
    g_legacy_udp = legacy_env && legacy_env[0] && strcmp(legacy_env, "0") != 0;

    // Initialise SDL2 GameController subsystem, plus sensors/haptics when available.
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
        return 1;
    }

    gtk_init(&argc, &argv);

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "NS PC Control");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 280);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), nullptr);
    gtk_window_set_icon_from_file(GTK_WINDOW(window), (get_exe_dir() + "/icon.png").c_str(), nullptr);

    GtkWidget* grid = gtk_grid_new();
    gtk_container_add(GTK_CONTAINER(window), grid);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);

    // Row 0: IP
    GtkWidget* ipLabel = gtk_label_new("Raspberry Pi IP:");
    gtk_widget_set_halign(ipLabel, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), ipLabel, 0, 0, 1, 1);

    ipEntry = gtk_entry_new();
    {
        std::string saved = load_saved_config();
        gtk_entry_set_text(GTK_ENTRY(ipEntry), saved.empty() ? "192.168.1.100" : saved.c_str());
    }
    gtk_grid_attach(GTK_GRID(grid), ipEntry, 1, 0, 3, 1);

    // Row 1: Connect Button
    connectBtn = gtk_button_new_with_label("Connect");
    g_signal_connect(connectBtn, "clicked", G_CALLBACK(on_connect_clicked), nullptr);
    gtk_grid_attach(GTK_GRID(grid), connectBtn, 1, 1, 2, 1);
    macroBtn = gtk_button_new_with_label("Macros...");
    g_signal_connect(macroBtn, "clicked", G_CALLBACK(on_macros_clicked), nullptr);
    gtk_grid_attach(GTK_GRID(grid), macroBtn, 3, 1, 1, 1);

    // Row 2: Separator
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep, 0, 2, 4, 1);

    // Row 3: Status
    statusLabel = gtk_label_new("Ready");
    gtk_widget_set_halign(statusLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), statusLabel, 0, 3, 4, 1);

    // Rows 4-7: P1 to P4 Slots
    for (int i = 0; i < 4; ++i) {
        char buf[64]; snprintf(buf, sizeof(buf), "P%d: Waiting...", i + 1);
        ctrlLabels[i] = gtk_label_new(buf);
        
        // Add some margin for visual indentation
        gtk_widget_set_margin_start(ctrlLabels[i], 10);
        gtk_widget_set_halign(ctrlLabels[i], GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), ctrlLabels[i], 0, 4 + i, 4, 1);
    }

    // Timer for UI updates (100ms)
    g_timeout_add(100, on_timer, nullptr);

    gtk_widget_show_all(window);
    gtk_main();

    SDL_Quit();
    return 0;
}