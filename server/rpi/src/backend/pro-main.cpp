#include "../../include/protocol.hpp"
#include "../../include/sha256.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <sstream>
#include <fstream>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <dirent.h>
#include <ctype.h>

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#include <stdexcept>
#include <limits>
#endif

using namespace ns;
using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

// ── Global flags ──────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static bool g_verbose = false;

// UDP clients on Windows can occasionally stall for a few seconds while the app
// is still open (scheduler hiccup, controller rescan, window drag, debugger,
// etc.).  Do not destroy/remap the whole PC session on a tiny UDP gap.  Instead
// neutralize stale input quickly, and only disconnect after a longer watchdog.
static uint64_t g_client_timeout_us = 30'000'000ULL;       // 30s hard disconnect
static constexpr uint64_t CLIENT_STALE_NEUTRAL_US = 350'000ULL; // 350ms release-to-neutral

// Debug helpers.
static bool g_log_neutral_rumble = false;   // show the neutral carrier in -v logs
static bool g_test_rumble_on_connect = false; // send one synthetic rumble to UDP clients
static bool g_random_identity = false;      // randomize USB serial + controller MACs before gadget creation
static bool g_no_user_imu_cal = false;      // leave 0x8026 magic unset so host falls back to factory IMU cal
static std::string g_usb_serial = "000000000001";

enum class ImuMapMode {
    Direct,
    InvertX,
    InvertY,
    InvertZ,
    SwapYZ,
    Switch1,
    Switch2,
    Switch3,
};

static ImuMapMode g_imu_map = ImuMapMode::Direct;

enum class ImuTestMode {
    Off,
    Roll,
    Pitch,
    Yaw,
    Shake,
    Gentle,
};

static ImuTestMode g_imu_test = ImuTestMode::Off;

enum class ImuWireLayout {
    AccelThenGyro, // bytes 13..24: ax ay az gx gy gz
    GyroThenAccel, // bytes 13..24: gx gy gz ax ay az
};

static ImuWireLayout g_imu_layout = ImuWireLayout::AccelThenGyro;

static const char* imu_layout_name(ImuWireLayout l) {
    switch (l) {
        case ImuWireLayout::AccelThenGyro: return "accel-gyro";
        case ImuWireLayout::GyroThenAccel: return "gyro-accel";
    }
    return "accel-gyro";
}

static bool parse_imu_layout(const std::string& raw) {
    std::string s = raw;
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    if (s == "accel-gyro" || s == "ag" || s == "default") {
        g_imu_layout = ImuWireLayout::AccelThenGyro;
        return true;
    }
    if (s == "gyro-accel" || s == "ga" || s == "gyro-first") {
        g_imu_layout = ImuWireLayout::GyroThenAccel;
        return true;
    }
    return false;
}

static const char* imu_test_name(ImuTestMode m) {
    switch (m) {
        case ImuTestMode::Off:   return "off";
        case ImuTestMode::Roll:  return "roll";
        case ImuTestMode::Pitch: return "pitch";
        case ImuTestMode::Yaw:   return "yaw";
        case ImuTestMode::Shake: return "shake";
        case ImuTestMode::Gentle: return "gentle";
    }
    return "off";
}

static bool parse_u8_arg(const char* raw, uint8_t& out) {
    if (!raw || !*raw) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(raw, &end, 0);
    if (!end || *end != '\0' || v > 0xFFUL) return false;
    out = (uint8_t)v;
    return true;
}

static bool parse_imu_test(const std::string& raw) {
    std::string s = raw;
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    if (s == "off" || s == "none") { g_imu_test = ImuTestMode::Off; return true; }
    if (s == "roll")  { g_imu_test = ImuTestMode::Roll;  return true; }
    if (s == "pitch") { g_imu_test = ImuTestMode::Pitch; return true; }
    if (s == "yaw")   { g_imu_test = ImuTestMode::Yaw;   return true; }
    if (s == "shake")  { g_imu_test = ImuTestMode::Shake;  return true; }
    if (s == "gentle") { g_imu_test = ImuTestMode::Gentle; return true; }
    return false;
}

static const char* imu_map_name(ImuMapMode m) {
    switch (m) {
        case ImuMapMode::Direct:  return "direct";
        case ImuMapMode::InvertX: return "invert-x";
        case ImuMapMode::InvertY: return "invert-y";
        case ImuMapMode::InvertZ: return "invert-z";
        case ImuMapMode::SwapYZ:  return "swap-yz";
        case ImuMapMode::Switch1: return "switch1";
        case ImuMapMode::Switch2: return "switch2";
        case ImuMapMode::Switch3: return "switch3";
    }
    return "direct";
}

static bool parse_imu_map(const std::string& raw) {
    std::string s = raw;
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    if (s == "direct")   { g_imu_map = ImuMapMode::Direct;  return true; }
    if (s == "invert-x") { g_imu_map = ImuMapMode::InvertX; return true; }
    if (s == "invert-y") { g_imu_map = ImuMapMode::InvertY; return true; }
    if (s == "invert-z") { g_imu_map = ImuMapMode::InvertZ; return true; }
    if (s == "swap-yz")  { g_imu_map = ImuMapMode::SwapYZ;  return true; }
    if (s == "switch1")  { g_imu_map = ImuMapMode::Switch1; return true; }
    if (s == "switch2")  { g_imu_map = ImuMapMode::Switch2; return true; }
    if (s == "switch3")  { g_imu_map = ImuMapMode::Switch3; return true; }
    return false;
}

// Built-in USB gadget lifecycle.  ns-backend can now create/bind the
// 4-interface Pro Controller gadget itself on startup and unbind/remove it
// on shutdown, so setup_gadget.sh is no longer needed at runtime.
static bool        g_auto_gadget_setup     = true;
static bool        g_force_gadget_setup    = false;
static bool        g_teardown_gadget_exit  = true;
static std::atomic<bool> g_gadget_setup_attempted{false};

// For gyro debugging, default to a descriptor that matches a real single
// Nintendo Pro Controller: one HID interface.  The old 4-interface composite
// gadget is still available with --gadget-pads 4, but it does not match the
// descriptor captured from real hardware and may be treated differently by HOS.
static int g_hid_port_count = 1;

// HMAC authentication (key derived from DEFAULT_SECRET at startup)
static uint8_t  g_hmac_key[32];

// Per-IP rate limiting (token bucket, 32-entry hash table)
static constexpr uint64_t RATE_WINDOW_US = 1'000'000;  // 1 second
static constexpr uint32_t RATE_MAX_PKT   = 2000;       // max packets/sec per IP
static constexpr int      RATE_TABLE     = 32;

struct RateSlot {
    uint32_t ip;           // IP in network byte order, 0 = empty
    uint32_t count;
    uint64_t window_start; // us
};
static RateSlot g_rate_table[RATE_TABLE];

// ── Multi-Client Session State ───────────────────────────────────────────────
static constexpr int MAX_CLIENTS = 4; // Hard limit matching the 4 physical ports

struct ClientSession {
    bool        active = false;
    sockaddr_in addr{};
    uint64_t    last_rx_us = 0;
    uint32_t    expected_seq = 0;
    bool        first_pkt = true;
    ExtendedMultiReport report{}; // Inputs + optional motion coming from this specific PC/WebSocket

    // Nintendo report 0x30 carries three IMU samples per input report.  Keep a
    // tiny newest-first history per logical pad so the virtual Pro Controller
    // looks like a real controller instead of repeating the same sample 3x.
    MotionReport motion_history[4][3]{};
    uint8_t      motion_history_count[4]{};

    RumblePacket rumble[4]{};
    uint32_t    rumble_seq[4]{};
    bool        rumble_active[4]{};
    bool        debug_rumble_sent[4]{};

    // Extended UDP clients can opt into server->client rumble by using the
    // authenticated extended packet format. Legacy UDP clients remain input-only.
    bool        udp_rumble_enabled = false;
    uint32_t    udp_last_rumble_seq[4]{};

    // Web/mobile packets set this even when the pad is neutral.  Without it,
    // the Switch port only maps after a non-neutral input, so early rumble can
    // be dropped before the browser has a rumble target.
    bool        pad_present[4]{};
    uint64_t    pad_last_present_us[4]{};
    bool        uses_pad_presence = false;
};

static std::mutex    g_mtx[MAX_CLIENTS];
static ClientSession g_clients[MAX_CLIENTS];

static uint64_t elapsed_us_saturated(uint64_t now, uint64_t then) {
    // All runtime timestamps should come from ns::now_us()/steady_clock, but
    // never let a bad/future timestamp wrap an unsigned subtraction into
    // ~UINT64_MAX.  That was causing bogus logs like:
    //   timed out after 18446744073709.6s
    if (then == 0 || then > now) return 0;
    return now - then;
}

static bool elapsed_us_over(uint64_t now, uint64_t then, uint64_t limit) {
    return then != 0 && elapsed_us_saturated(now, then) > limit;
}

static void repair_future_client_timestamp(ClientSession& c, uint64_t now) {
    if (c.active && (c.last_rx_us == 0 || c.last_rx_us > now)) {
        c.last_rx_us = now;
    }
}

static void clear_motion_history(ClientSession& c, int subpad) {
    if (subpad < 0 || subpad >= 4) return;
    for (int i = 0; i < 3; ++i) c.motion_history[subpad][i].reset();
    c.motion_history_count[subpad] = 0;
}

static void clear_all_motion_history(ClientSession& c) {
    for (int s = 0; s < 4; ++s) clear_motion_history(c, s);
}

static void push_motion_history(ClientSession& c, int subpad, const MotionReport& motion) {
    if (subpad < 0 || subpad >= 4) return;
    c.motion_history[subpad][2] = c.motion_history[subpad][1];
    c.motion_history[subpad][1] = c.motion_history[subpad][0];
    c.motion_history[subpad][0] = motion;
    if (c.motion_history_count[subpad] < 3) c.motion_history_count[subpad]++;
}

static int16_t neg_i16_safe(int16_t v) {
    return v == INT16_MIN ? INT16_MAX : (int16_t)-v;
}

static MotionReport remap_motion_for_switch(const MotionReport& in) {
    MotionReport o = in;
    switch (g_imu_map) {
        case ImuMapMode::Direct:
            break;

        case ImuMapMode::InvertX:
            o.ax = neg_i16_safe(in.ax); o.gx = neg_i16_safe(in.gx);
            break;

        case ImuMapMode::InvertY:
            o.ay = neg_i16_safe(in.ay); o.gy = neg_i16_safe(in.gy);
            break;

        case ImuMapMode::InvertZ:
            o.az = neg_i16_safe(in.az); o.gz = neg_i16_safe(in.gz);
            break;

        case ImuMapMode::SwapYZ:
            o.ax = in.ax; o.ay = in.az; o.az = in.ay;
            o.gx = in.gx; o.gy = in.gz; o.gz = in.gy;
            break;

        // These presets are intentionally simple right-handed rotations/sign
        // flips around the SDL-standardized sensor axes.  They let you test the
        // orientation the Switch/Mario Kart expects without recompiling.
        case ImuMapMode::Switch1:
            o.ax = in.ax;              o.ay = in.az;              o.az = neg_i16_safe(in.ay);
            o.gx = in.gx;              o.gy = in.gz;              o.gz = neg_i16_safe(in.gy);
            break;

        case ImuMapMode::Switch2:
            o.ax = in.ax;              o.ay = neg_i16_safe(in.az); o.az = in.ay;
            o.gx = in.gx;              o.gy = neg_i16_safe(in.gz); o.gz = in.gy;
            break;

        case ImuMapMode::Switch3:
            o.ax = in.az;              o.ay = in.ay;              o.az = neg_i16_safe(in.ax);
            o.gx = in.gz;              o.gy = in.gy;              o.gz = neg_i16_safe(in.gx);
            break;
    }
    return o;
}

// Diagnostics
static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};


// ── Server-side macro playback ───────────────────────────────────────────────────────────
// Macro grammar is intentionally strict and shared by CLI/GUI clients:
//   WAIT 100                         -> release macro inputs for 100ms
//   A 100                            -> hold A for 100ms
//   R+LSTICK_LEFT 450                -> hold R and steer left for 450ms
//   LOOP 200                         -> repeat the block since the previous LOOP/start 200 times
// Accepted JSON:
//   {"name":"...","commands":"WAIT 100; A 100"}
//   {"name":"...","commands":["WAIT 100", "A 100"]}
static constexpr size_t MACRO_JSON_MAX_BYTES = 50ULL * 1024ULL * 1024ULL;
static std::string g_macro_last_error;


static constexpr uint32_t MACRO_UDP_MAGIC       = 0x4E534D43u; // 'NSMC' legacy one-datagram upload
static constexpr uint32_t MACRO_UDP_CHUNK_MAGIC = 0x4E534D4Bu; // 'NSMK' chunked upload
static constexpr size_t   MACRO_UDP_TEXT_MAX    = MACRO_JSON_MAX_BYTES;
static constexpr size_t   MACRO_UDP_CHUNK_MAX   = 1200;
static constexpr uint8_t  MACRO_CHUNK_FLAG_LAST = 0x01;

struct ServerMacroStep {
    uint16_t buttons = 0;
    uint8_t hat = HAT_NEUTRAL;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    bool has_lstick = false;
    bool has_rstick = false;
    uint32_t duration_ms = 0;
};

static void macro_set_error(const std::string& e) { g_macro_last_error = e; }
static const std::string& macro_last_error() { return g_macro_last_error; }

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

static bool macro_is_hex4(const std::string& s, size_t pos) {
    if (pos + 4 > s.size()) return false;
    for (size_t i = 0; i < 4; ++i) if (!std::isxdigit((unsigned char)s[pos+i])) return false;
    return true;
}

static bool macro_read_json_string_at(const std::string& raw, size_t& pos, std::string& out, std::string& err) {
    if (pos >= raw.size() || raw[pos] != '"') { err = "expected JSON string"; return false; }
    out.clear();
    ++pos;
    while (pos < raw.size()) {
        char c = raw[pos++];
        if ((unsigned char)c < 0x20) { err = "unescaped control character in JSON string"; return false; }
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (pos >= raw.size()) { err = "unfinished JSON escape"; return false; }
        char e = raw[pos++];
        switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
                if (!macro_is_hex4(raw, pos)) { err = "invalid JSON unicode escape"; return false; }
                // Macro commands are ASCII; preserve unicode names as '?' rather than failing the whole file.
                out += '?'; pos += 4; break;
            default: err = "invalid JSON escape"; return false;
        }
    }
    err = "unterminated JSON string";
    return false;
}

static void macro_skip_ws(const std::string& raw, size_t& pos) {
    while (pos < raw.size() && std::isspace((unsigned char)raw[pos])) ++pos;
}

static bool macro_skip_json_value(const std::string& raw, size_t& pos, std::string& err);

static bool macro_skip_json_array(const std::string& raw, size_t& pos, std::string& err) {
    if (pos >= raw.size() || raw[pos] != '[') { err = "expected JSON array"; return false; }
    ++pos;
    macro_skip_ws(raw, pos);
    if (pos < raw.size() && raw[pos] == ']') { ++pos; return true; }
    while (pos < raw.size()) {
        if (!macro_skip_json_value(raw, pos, err)) return false;
        macro_skip_ws(raw, pos);
        if (pos < raw.size() && raw[pos] == ',') { ++pos; macro_skip_ws(raw, pos); continue; }
        if (pos < raw.size() && raw[pos] == ']') { ++pos; return true; }
        err = "expected ',' or ']' in JSON array"; return false;
    }
    err = "unterminated JSON array"; return false;
}

static bool macro_skip_json_object(const std::string& raw, size_t& pos, std::string& err) {
    if (pos >= raw.size() || raw[pos] != '{') { err = "expected JSON object"; return false; }
    ++pos;
    macro_skip_ws(raw, pos);
    if (pos < raw.size() && raw[pos] == '}') { ++pos; return true; }
    while (pos < raw.size()) {
        std::string key;
        if (!macro_read_json_string_at(raw, pos, key, err)) return false;
        macro_skip_ws(raw, pos);
        if (pos >= raw.size() || raw[pos] != ':') { err = "expected ':' after JSON key"; return false; }
        ++pos;
        macro_skip_ws(raw, pos);
        if (!macro_skip_json_value(raw, pos, err)) return false;
        macro_skip_ws(raw, pos);
        if (pos < raw.size() && raw[pos] == ',') { ++pos; macro_skip_ws(raw, pos); continue; }
        if (pos < raw.size() && raw[pos] == '}') { ++pos; return true; }
        err = "expected ',' or '}' in JSON object"; return false;
    }
    err = "unterminated JSON object"; return false;
}

static bool macro_skip_json_value(const std::string& raw, size_t& pos, std::string& err) {
    macro_skip_ws(raw, pos);
    if (pos >= raw.size()) { err = "missing JSON value"; return false; }
    if (raw[pos] == '"') { std::string tmp; return macro_read_json_string_at(raw, pos, tmp, err); }
    if (raw[pos] == '{') return macro_skip_json_object(raw, pos, err);
    if (raw[pos] == '[') return macro_skip_json_array(raw, pos, err);
    if (raw.compare(pos, 4, "true") == 0) { pos += 4; return true; }
    if (raw.compare(pos, 5, "false") == 0) { pos += 5; return true; }
    if (raw.compare(pos, 4, "null") == 0) { pos += 4; return true; }
    if (raw[pos] == '-' || std::isdigit((unsigned char)raw[pos])) {
        ++pos;
        while (pos < raw.size() && (std::isdigit((unsigned char)raw[pos]) || raw[pos]=='.' || raw[pos]=='e' || raw[pos]=='E' || raw[pos]=='+' || raw[pos]=='-')) ++pos;
        return true;
    }
    err = "invalid JSON value"; return false;
}

static bool macro_extract_commands_text(const std::string& raw_in, std::string& out, std::string& err) {
    if (raw_in.size() > MACRO_JSON_MAX_BYTES) { err = "macro JSON exceeds 50MB limit"; return false; }
    std::string raw = macro_trim(raw_in);
    out.clear();
    if (raw.empty()) { err = "empty macro"; return false; }

    // Raw command text remains supported for CLI convenience.
    if (raw[0] != '{' && raw[0] != '[') { out = raw; return true; }

    size_t pos = 0;
    macro_skip_ws(raw, pos);
    if (raw[pos] == '[') {
        // Accept a bare array of command strings.
        ++pos; macro_skip_ws(raw, pos);
        if (pos < raw.size() && raw[pos] == ']') { err = "commands array is empty"; return false; }
        while (pos < raw.size()) {
            std::string item;
            if (!macro_read_json_string_at(raw, pos, item, err)) return false;
            if (!out.empty()) out += ";";
            out += item;
            macro_skip_ws(raw, pos);
            if (pos < raw.size() && raw[pos] == ',') { ++pos; macro_skip_ws(raw, pos); continue; }
            if (pos < raw.size() && raw[pos] == ']') { ++pos; break; }
            err = "expected ',' or ']' in commands array"; return false;
        }
        macro_skip_ws(raw, pos);
        if (pos != raw.size()) { err = "extra data after JSON array"; return false; }
        return true;
    }

    if (raw[pos] != '{') { err = "macro JSON must be an object or commands array"; return false; }
    ++pos; macro_skip_ws(raw, pos);
    bool found_commands = false;
    if (pos < raw.size() && raw[pos] == '}') { err = "macro object is missing commands"; return false; }
    while (pos < raw.size()) {
        std::string key;
        if (!macro_read_json_string_at(raw, pos, key, err)) return false;
        macro_skip_ws(raw, pos);
        if (pos >= raw.size() || raw[pos] != ':') { err = "expected ':' after JSON key"; return false; }
        ++pos; macro_skip_ws(raw, pos);
        if (key == "commands") {
            found_commands = true;
            if (pos < raw.size() && raw[pos] == '"') {
                if (!macro_read_json_string_at(raw, pos, out, err)) return false;
            } else if (pos < raw.size() && raw[pos] == '[') {
                ++pos; macro_skip_ws(raw, pos);
                if (pos < raw.size() && raw[pos] == ']') { err = "commands array is empty"; return false; }
                while (pos < raw.size()) {
                    std::string item;
                    if (!macro_read_json_string_at(raw, pos, item, err)) { err = "commands array must contain only strings"; return false; }
                    if (!out.empty()) out += ";";
                    out += item;
                    macro_skip_ws(raw, pos);
                    if (pos < raw.size() && raw[pos] == ',') { ++pos; macro_skip_ws(raw, pos); continue; }
                    if (pos < raw.size() && raw[pos] == ']') { ++pos; break; }
                    err = "expected ',' or ']' in commands array"; return false;
                }
            } else { err = "commands must be a string or an array of strings"; return false; }
        } else {
            if (!macro_skip_json_value(raw, pos, err)) return false;
        }
        macro_skip_ws(raw, pos);
        if (pos < raw.size() && raw[pos] == ',') { ++pos; macro_skip_ws(raw, pos); continue; }
        if (pos < raw.size() && raw[pos] == '}') { ++pos; break; }
        err = "expected ',' or '}' in macro object"; return false;
    }
    macro_skip_ws(raw, pos);
    if (pos != raw.size()) { err = "extra data after JSON object"; return false; }
    if (!found_commands) { err = "macro object is missing commands"; return false; }
    return true;
}

static bool macro_parse_uint32_strict(const std::string& s, uint32_t& out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (!std::isdigit((unsigned char)c)) return false;
        v = v * 10 + (uint64_t)(c - '0');
        if (v > 0xFFFFFFFFULL) return false;
    }
    if (v == 0) return false;
    out = (uint32_t)v;
    return true;
}

static uint16_t macro_button_bit(const std::string& token) {
    std::string name = macro_upper(macro_trim(token));
    if (name == "A" || name == "BTN_A") return BTN_A;
    if (name == "B" || name == "BTN_B") return BTN_B;
    if (name == "X" || name == "BTN_X") return BTN_X;
    if (name == "Y" || name == "BTN_Y") return BTN_Y;
    if (name == "L" || name == "BTN_L") return BTN_L;
    if (name == "R" || name == "BTN_R") return BTN_R;
    if (name == "ZL" || name == "BTN_ZL") return BTN_ZL;
    if (name == "ZR" || name == "BTN_ZR") return BTN_ZR;
    if (name == "MINUS" || name == "-" || name == "BTN_MINUS") return BTN_MINUS;
    if (name == "PLUS" || name == "+" || name == "BTN_PLUS") return BTN_PLUS;
    if (name == "LSTICK" || name == "LS" || name == "BTN_LSTICK") return BTN_LSTICK;
    if (name == "RSTICK" || name == "RS" || name == "BTN_RSTICK") return BTN_RSTICK;
    if (name == "HOME" || name == "BTN_HOME") return BTN_HOME;
    if (name == "CAPTURE" || name == "BTN_CAPTURE") return BTN_CAPTURE;
    return 0;
}

static bool macro_apply_token(const std::string& raw_tok, ServerMacroStep& st, std::string& err,
                              bool& du, bool& dd, bool& dl, bool& dr,
                              bool& llu, bool& lld, bool& lll, bool& llr,
                              bool& rru, bool& rrd, bool& rrl, bool& rrr) {
    std::string tok = macro_upper(macro_trim(raw_tok));
    if (tok.empty()) return true;
    uint16_t bit = macro_button_bit(tok);
    if (bit) { st.buttons |= bit; return true; }

    if (tok == "DPAD_UP" || tok == "DUP" || tok == "UP") { du = true; return true; }
    if (tok == "DPAD_DOWN" || tok == "DDOWN" || tok == "DOWN") { dd = true; return true; }
    if (tok == "DPAD_LEFT" || tok == "DLEFT" || tok == "LEFT") { dl = true; return true; }
    if (tok == "DPAD_RIGHT" || tok == "DRIGHT" || tok == "RIGHT") { dr = true; return true; }

    if (tok == "LSTICK_UP" || tok == "LS_UP") { llu = true; st.has_lstick = true; return true; }
    if (tok == "LSTICK_DOWN" || tok == "LS_DOWN") { lld = true; st.has_lstick = true; return true; }
    if (tok == "LSTICK_LEFT" || tok == "LS_LEFT") { lll = true; st.has_lstick = true; return true; }
    if (tok == "LSTICK_RIGHT" || tok == "LS_RIGHT") { llr = true; st.has_lstick = true; return true; }

    if (tok == "RSTICK_UP" || tok == "RS_UP") { rru = true; st.has_rstick = true; return true; }
    if (tok == "RSTICK_DOWN" || tok == "RS_DOWN") { rrd = true; st.has_rstick = true; return true; }
    if (tok == "RSTICK_LEFT" || tok == "RS_LEFT") { rrl = true; st.has_rstick = true; return true; }
    if (tok == "RSTICK_RIGHT" || tok == "RS_RIGHT") { rrr = true; st.has_rstick = true; return true; }

    err = "unknown macro input: " + raw_tok;
    return false;
}

static bool macro_parse_one_command(const std::string& part, ServerMacroStep& st, std::string& err) {
    size_t last_space = part.find_last_of(" \t");
    if (last_space == std::string::npos) { err = "missing duration in command: " + part; return false; }
    std::string cmd = macro_trim(part.substr(0, last_space));
    std::string ms_s = macro_trim(part.substr(last_space + 1));
    uint32_t ms = 0;
    if (!macro_parse_uint32_strict(ms_s, ms)) { err = "invalid duration in command: " + part; return false; }
    st = ServerMacroStep{};
    st.duration_ms = ms;
    std::string up = macro_upper(cmd);
    if (up == "WAIT") return true;
    if (cmd.empty()) { err = "missing input before duration in command: " + part; return false; }

    for (char& c : cmd) if (c == '+' || c == ',' || c == '|') c = ' ';
    std::istringstream iss(cmd);
    std::string tok;
    bool du=false,dd=false,dl=false,dr=false, llu=false,lld=false,lll=false,llr=false, rru=false,rrd=false,rrl=false,rrr=false;
    int token_count = 0;
    while (iss >> tok) {
        ++token_count;
        if (!macro_apply_token(tok, st, err, du,dd,dl,dr, llu,lld,lll,llr, rru,rrd,rrl,rrr)) return false;
    }
    if (token_count == 0) { err = "empty input in command: " + part; return false; }
    if (du && dd) { err = "DPAD_UP and DPAD_DOWN conflict in command: " + part; return false; }
    if (dl && dr) { err = "DPAD_LEFT and DPAD_RIGHT conflict in command: " + part; return false; }
    if (llu && lld) { err = "LSTICK_UP and LSTICK_DOWN conflict in command: " + part; return false; }
    if (lll && llr) { err = "LSTICK_LEFT and LSTICK_RIGHT conflict in command: " + part; return false; }
    if (rru && rrd) { err = "RSTICK_UP and RSTICK_DOWN conflict in command: " + part; return false; }
    if (rrl && rrr) { err = "RSTICK_LEFT and RSTICK_RIGHT conflict in command: " + part; return false; }

    if (du && dr) st.hat = HAT_NE;
    else if (du && dl) st.hat = HAT_NW;
    else if (dd && dr) st.hat = HAT_SE;
    else if (dd && dl) st.hat = HAT_SW;
    else if (du) st.hat = HAT_N;
    else if (dd) st.hat = HAT_S;
    else if (dr) st.hat = HAT_E;
    else if (dl) st.hat = HAT_W;

    if (st.has_lstick) { st.lx = lll ? 0 : (llr ? 255 : 128); st.ly = llu ? 0 : (lld ? 255 : 128); }
    if (st.has_rstick) { st.rx = rrl ? 0 : (rrr ? 255 : 128); st.ry = rru ? 0 : (rrd ? 255 : 128); }
    return true;
}

static bool server_macro_validate_text(const std::string& raw_text, std::vector<ServerMacroStep>& steps, std::vector<std::string>* normalized = nullptr) {
    g_macro_last_error.clear();
    steps.clear();
    if (normalized) normalized->clear();
    std::string text, err;
    if (!macro_extract_commands_text(raw_text, text, err)) { macro_set_error(err); return false; }
    for (char& c : text) if (c == '\n' || c == '\r') c = ';';
    size_t pos = 0;
    size_t loop_block_start = 0;
    static constexpr size_t MACRO_MAX_EXPANDED_STEPS = 1000000;
    while (pos < text.size()) {
        size_t semi = text.find(';', pos);
        std::string part = macro_trim(text.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
        pos = (semi == std::string::npos) ? text.size() : semi + 1;
        if (part.empty() || part[0] == '#') continue;

        size_t last_space = part.find_last_of(" 	");
        std::string maybe_cmd = last_space == std::string::npos ? macro_upper(part) : macro_upper(macro_trim(part.substr(0, last_space)));
        if (maybe_cmd == "LOOP") {
            if (last_space == std::string::npos) { macro_set_error("missing count in LOOP command: " + part); return false; }
            uint32_t count = 0;
            if (!macro_parse_uint32_strict(macro_trim(part.substr(last_space + 1)), count)) { macro_set_error("invalid LOOP count in command: " + part); return false; }
            if (steps.size() == loop_block_start) { macro_set_error("LOOP has no previous commands to repeat: " + part); return false; }
            const size_t block_len = steps.size() - loop_block_start;
            if (count > 1 && block_len > (MACRO_MAX_EXPANDED_STEPS - steps.size()) / (count - 1)) {
                macro_set_error("LOOP expansion is too large; reduce LOOP count or split the macro");
                return false;
            }
            std::vector<ServerMacroStep> block(steps.begin() + (std::ptrdiff_t)loop_block_start, steps.end());
            for (uint32_t i = 1; i < count; ++i) steps.insert(steps.end(), block.begin(), block.end());
            loop_block_start = steps.size();
            if (normalized) normalized->push_back(part);
            continue;
        }

        ServerMacroStep st;
        if (!macro_parse_one_command(part, st, err)) { macro_set_error(err); return false; }
        steps.push_back(st);
        if (normalized) normalized->push_back(part);
    }
    if (steps.empty()) { macro_set_error("no valid macro commands found"); return false; }
    return true;
}

static std::vector<ServerMacroStep> server_macro_parse_text(const std::string& raw_text) {
    std::vector<ServerMacroStep> steps;
    server_macro_validate_text(raw_text, steps, nullptr);
    return steps;
}

static std::string macro_read_file(const std::string& path) {
    g_macro_last_error.clear();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { macro_set_error("cannot open macro file"); return ""; }
    std::streamoff len = f.tellg();
    if (len < 0) { macro_set_error("cannot read macro file size"); return ""; }
    if ((uint64_t)len > MACRO_JSON_MAX_BYTES) { macro_set_error("macro JSON exceeds 50MB limit"); return ""; }
    f.seekg(0, std::ios::beg);
    std::string s((size_t)len, '\0');
    if (len > 0) f.read(&s[0], len);
    if (!f && len > 0) { macro_set_error("failed while reading macro file"); return ""; }
    return s;
}

[[maybe_unused]] static std::vector<ServerMacroStep> server_macro_load_file(const std::string& path) {
    std::string txt = macro_read_file(path);
    if (txt.empty()) return {};
    return server_macro_parse_text(txt);
}

static std::string macro_escape_json(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += (char)c;
    }
    return out;
}

static std::string macro_extract_name_or_default(const std::string& raw, const std::string& fallback_name) {
    std::string t = macro_trim(raw);
    if (t.empty() || t[0] != '{') return fallback_name;
    size_t pos = 1; std::string err;
    macro_skip_ws(t, pos);
    while (pos < t.size() && t[pos] != '}') {
        std::string key;
        if (!macro_read_json_string_at(t, pos, key, err)) return fallback_name;
        macro_skip_ws(t, pos);
        if (pos >= t.size() || t[pos] != ':') return fallback_name;
        ++pos; macro_skip_ws(t, pos);
        if (key == "name" && pos < t.size() && t[pos] == '"') {
            std::string name;
            if (macro_read_json_string_at(t, pos, name, err) && !macro_trim(name).empty()) return name;
            return fallback_name;
        }
        if (!macro_skip_json_value(t, pos, err)) return fallback_name;
        macro_skip_ws(t, pos);
        if (pos < t.size() && t[pos] == ',') { ++pos; macro_skip_ws(t, pos); }
    }
    return fallback_name;
}

static std::string macro_pretty_json(const std::string& raw_text, const std::string& fallback_name = "Macro") {
    std::vector<ServerMacroStep> steps;
    std::vector<std::string> lines;
    if (!server_macro_validate_text(raw_text, steps, &lines)) {
        lines = {"WAIT 200"};
    }
    std::string name = macro_extract_name_or_default(raw_text, fallback_name);
    std::string out;
    out += "{\n";
    out += "  \"name\": \"" + macro_escape_json(name) + "\",\n";
    out += "  \"commands\": [\n";
    for (size_t i = 0; i < lines.size(); ++i) {
        out += "    \"" + macro_escape_json(lines[i]) + "\"";
        if (i + 1 < lines.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n";
    out += "}";
    return out;
}

[[maybe_unused]] static bool macro_validate_to_pretty_json(const std::string& raw_text, std::string& pretty, std::string& err, const std::string& fallback_name = "Macro") {
    std::vector<ServerMacroStep> steps;
    if (!server_macro_validate_text(raw_text, steps, nullptr)) { err = macro_last_error(); return false; }
    pretty = macro_pretty_json(raw_text, fallback_name);
    err.clear();
    return true;
}

static uint64_t server_macro_total_ms(const std::vector<ServerMacroStep>& steps) {
    uint64_t total = 0;
    for (const auto& s : steps) {
        if (UINT64_MAX - total < s.duration_ms) return UINT64_MAX;
        total += s.duration_ms;
    }
    return total;
}

[[maybe_unused]] static bool server_macro_report_at(const std::vector<ServerMacroStep>& steps, uint64_t elapsed_ms, HIDReport& out) {
    out.reset();
    uint64_t t = 0;
    for (const auto& s : steps) {
        uint64_t next = t + s.duration_ms;
        if (elapsed_ms < next) {
            out.buttons = s.buttons;
            out.hat = s.hat;
            if (s.has_lstick) { out.lx = s.lx; out.ly = s.ly; }
            if (s.has_rstick) { out.rx = s.rx; out.ry = s.ry; }
            return true;
        }
        t = next;
    }
    return false;
}


static bool server_macro_step_at(const std::vector<ServerMacroStep>& steps, uint64_t elapsed_ms, ServerMacroStep& out) {
    uint64_t t = 0;
    for (const auto& s : steps) {
        uint64_t next = t + s.duration_ms;
        if (elapsed_ms < next) { out = s; return true; }
        t = next;
    }
    return false;
}


struct ServerMacroRuntime {
    std::vector<ServerMacroStep> steps;
    bool running = false;
    uint64_t start_us = 0;
};

static std::mutex g_server_macro_mtx;
static ServerMacroRuntime g_server_macros[MAX_CLIENTS][4];

#define NS_MACRO_PACKED __attribute__((packed))
struct NS_MACRO_PACKED MacroUdpHeaderWire {
    uint32_t magic;
    uint8_t version;
    uint8_t subpad;
    uint32_t text_len;
    uint32_t seq;
};
static constexpr size_t MACRO_UDP_HEADER_SIZE = sizeof(MacroUdpHeaderWire);
static constexpr size_t MACRO_UDP_AUTH_SIZE(size_t text_len) { return MACRO_UDP_HEADER_SIZE + text_len; }

struct NS_MACRO_PACKED MacroUdpChunkHeaderWire {
    uint32_t magic;
    uint8_t version;
    uint8_t subpad;
    uint8_t flags;
    uint8_t reserved;
    uint32_t upload_id;
    uint32_t chunk_index;
    uint32_t chunk_count;
    uint32_t total_len;
    uint16_t chunk_len;
    uint32_t seq;
};
static constexpr size_t MACRO_CHUNK_HEADER_SIZE = sizeof(MacroUdpChunkHeaderWire);
static_assert(MACRO_CHUNK_HEADER_SIZE == 30, "Macro chunk header must stay 30 bytes");

struct ServerMacroUploadRuntime {
    bool active = false;
    sockaddr_in sender{};
    uint32_t upload_id = 0;
    uint8_t subpad = 0;
    uint32_t total_len = 0;
    uint32_t chunk_count = 0;
    uint32_t received_count = 0;
    uint64_t last_rx_us = 0;
    std::vector<std::string> chunks;
    std::vector<uint8_t> got;
};

static std::mutex g_server_macro_upload_mtx;
static ServerMacroUploadRuntime g_server_macro_uploads[MAX_CLIENTS];

static bool rate_allow(uint32_t ip);
static bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands);

static int server_macro_client_for_sender(const sockaddr_in& sender) {
    uint32_t src_ip = sender.sin_addr.s_addr;
    uint64_t now = now_us();
    int client_idx = -1;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_mtx[i]);
        if (g_clients[i].active && g_clients[i].addr.sin_addr.s_addr == src_ip && g_clients[i].addr.sin_port == sender.sin_port) { client_idx = i; break; }
    }
    if (client_idx == -1) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            std::lock_guard<std::mutex> lk(g_mtx[i]);
            repair_future_client_timestamp(g_clients[i], now);
            if (!g_clients[i].active || elapsed_us_over(now, g_clients[i].last_rx_us, g_client_timeout_us)) {
                client_idx = i;
                g_clients[i].active = true;
                g_clients[i].addr = sender;
                g_clients[i].first_pkt = true;
                g_clients[i].expected_seq = 0;
                g_clients[i].report.reset();
                clear_all_motion_history(g_clients[i]);
                g_clients[i].last_rx_us = now;
                break;
            }
        }
    }
    if (client_idx >= 0) {
        std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
        g_clients[client_idx].active = true;
        g_clients[client_idx].addr = sender;
        g_clients[client_idx].last_rx_us = now;
    }
    return client_idx;
}

static bool server_macro_handle_chunk_packet(const uint8_t* data, size_t bytes, const sockaddr_in& sender) {
    if (bytes < MACRO_CHUNK_HEADER_SIZE + HMAC_TAG_SIZE) return false;
    MacroUdpChunkHeaderWire h{};
    memcpy(&h, data, sizeof(h));
    if (h.magic != MACRO_UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION) { if (g_verbose) puts("bad macro chunk version, dropped"); return true; }
    if (h.total_len > MACRO_UDP_TEXT_MAX) { if (g_verbose) puts("macro chunk total over 50MB, dropped"); return true; }
    if (h.chunk_len > MACRO_UDP_CHUNK_MAX) { if (g_verbose) puts("macro chunk too large, dropped"); return true; }
    if (h.chunk_count == 0 || h.chunk_index >= h.chunk_count) { if (g_verbose) puts("bad macro chunk index/count, dropped"); return true; }
    if (bytes != MACRO_CHUNK_HEADER_SIZE + (size_t)h.chunk_len + HMAC_TAG_SIZE) { if (g_verbose) puts("bad macro chunk packet size, dropped"); return true; }
    const uint8_t* recv_hmac = data + MACRO_CHUNK_HEADER_SIZE + h.chunk_len;
    if (hmac_verify(g_hmac_key, 32, data, MACRO_CHUNK_HEADER_SIZE + h.chunk_len, recv_hmac, HMAC_TAG_SIZE) != 0) { if (g_verbose) puts("bad macro chunk HMAC, dropped"); return true; }
    if (!rate_allow(sender.sin_addr.s_addr)) return true;

    uint64_t now = now_us();
    int client_idx = server_macro_client_for_sender(sender);
    if (client_idx < 0) return true;

    std::string completed;
    uint8_t completed_subpad = h.subpad < 4 ? h.subpad : 0;
    {
        std::lock_guard<std::mutex> lk(g_server_macro_upload_mtx);
        ServerMacroUploadRuntime& up = g_server_macro_uploads[client_idx];
        bool same = up.active && up.upload_id == h.upload_id &&
                    up.sender.sin_addr.s_addr == sender.sin_addr.s_addr &&
                    up.sender.sin_port == sender.sin_port;
        if (!same) {
            up = ServerMacroUploadRuntime{};
            up.active = true;
            up.sender = sender;
            up.upload_id = h.upload_id;
            up.subpad = h.subpad < 4 ? h.subpad : 0;
            up.total_len = h.total_len;
            up.chunk_count = h.chunk_count;
            try {
                up.chunks.assign(h.chunk_count, std::string());
                up.got.assign(h.chunk_count, 0);
            } catch (...) {
                up = ServerMacroUploadRuntime{};
                if (g_verbose) puts("macro chunk allocation failed");
                return true;
            }
        }
        if (up.total_len != h.total_len || up.chunk_count != h.chunk_count) { if (g_verbose) puts("macro chunk metadata mismatch, dropped"); return true; }
        up.last_rx_us = now;
        if (!up.got[h.chunk_index]) {
            up.chunks[h.chunk_index].assign(reinterpret_cast<const char*>(data + MACRO_CHUNK_HEADER_SIZE), h.chunk_len);
            up.got[h.chunk_index] = 1;
            up.received_count++;
        }
        if (up.received_count == up.chunk_count) {
            size_t total = 0;
            for (const auto& c : up.chunks) total += c.size();
            if (total != up.total_len) { if (g_verbose) puts("macro chunk final size mismatch"); up = ServerMacroUploadRuntime{}; return true; }
            completed.reserve(total);
            for (const auto& c : up.chunks) completed += c;
            completed_subpad = up.subpad;
            up = ServerMacroUploadRuntime{};
        }
    }
    if (!completed.empty()) {
        if (g_verbose) std::printf("[macro] received chunked macro %zu bytes\n", completed.size());
        server_macro_start(client_idx, completed_subpad, completed);
    }
    return true;
}


static bool server_macro_handle_ws_chunk_packet(int client_idx, const uint8_t* data, size_t bytes) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return false;
    if (bytes < MACRO_CHUNK_HEADER_SIZE) return false;
    MacroUdpChunkHeaderWire h{};
    memcpy(&h, data, sizeof(h));
    if (h.magic != MACRO_UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION) return true;
    if (h.total_len > MACRO_UDP_TEXT_MAX || h.chunk_len > MACRO_UDP_CHUNK_MAX || h.chunk_count == 0 || h.chunk_index >= h.chunk_count) return true;
    if (bytes != MACRO_CHUNK_HEADER_SIZE + (size_t)h.chunk_len) return true;
    uint64_t now = now_us();
    std::string completed;
    uint8_t completed_subpad = h.subpad < 4 ? h.subpad : 0;
    {
        std::lock_guard<std::mutex> lk(g_server_macro_upload_mtx);
        ServerMacroUploadRuntime& up = g_server_macro_uploads[client_idx];
        bool same = up.active && up.upload_id == h.upload_id;
        if (!same) {
            up = ServerMacroUploadRuntime{};
            up.active = true;
            up.upload_id = h.upload_id;
            up.subpad = h.subpad < 4 ? h.subpad : 0;
            up.total_len = h.total_len;
            up.chunk_count = h.chunk_count;
            try { up.chunks.assign(h.chunk_count, std::string()); up.got.assign(h.chunk_count, 0); }
            catch (...) { up = ServerMacroUploadRuntime{}; return true; }
        }
        if (up.total_len != h.total_len || up.chunk_count != h.chunk_count) return true;
        up.last_rx_us = now;
        if (!up.got[h.chunk_index]) {
            up.chunks[h.chunk_index].assign(reinterpret_cast<const char*>(data + MACRO_CHUNK_HEADER_SIZE), h.chunk_len);
            up.got[h.chunk_index] = 1;
            up.received_count++;
        }
        if (up.received_count == up.chunk_count) {
            size_t total = 0; for (const auto& c : up.chunks) total += c.size();
            if (total != up.total_len) { up = ServerMacroUploadRuntime{}; return true; }
            completed.reserve(total); for (const auto& c : up.chunks) completed += c;
            completed_subpad = up.subpad;
            up = ServerMacroUploadRuntime{};
        }
    }
    if (!completed.empty()) server_macro_start(client_idx, completed_subpad, completed);
    return true;
}

static bool server_macro_running(int client_idx, int subpad) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return false;
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    ServerMacroRuntime& rt = g_server_macros[client_idx][subpad];
    if (!rt.running) return false;
    uint64_t elapsed_ms = (now_us() - rt.start_us) / 1000ULL;
    if (elapsed_ms > server_macro_total_ms(rt.steps) + 120) { rt.running = false; return false; }
    return true;
}

static void server_macro_apply(int client_idx, int subpad, HIDReport& live) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return;
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    ServerMacroRuntime& rt = g_server_macros[client_idx][subpad];
    if (!rt.running) return;
    uint64_t elapsed_ms = (now_us() - rt.start_us) / 1000ULL;
    ServerMacroStep step{};
    if (!server_macro_step_at(rt.steps, elapsed_ms, step)) {
        rt.running = false;
        return;
    }
    live.buttons |= step.buttons;
    if (step.hat != HAT_NEUTRAL && live.hat == HAT_NEUTRAL) live.hat = step.hat;
    if (step.has_lstick) { live.lx = step.lx; live.ly = step.ly; }
    if (step.has_rstick) { live.rx = step.rx; live.ry = step.ry; }
}

static bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return false;
    if (subpad < 0 || subpad >= 4) subpad = 0;
    std::vector<ServerMacroStep> steps;
    if (!server_macro_validate_text(json_or_commands, steps, nullptr)) {
        if (g_verbose) std::printf("[macro] rejected: %s\n", macro_last_error().c_str());
        return false;
    }
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    ServerMacroRuntime& rt = g_server_macros[client_idx][subpad];
    rt.steps = std::move(steps);
    rt.running = true;
    rt.start_us = now_us();
    if (g_verbose) std::printf("[macro] started server macro slot=%d pad=%d\n", client_idx + 1, subpad + 1);
    return true;
}

[[maybe_unused]] static void server_macro_stop_all_for_client(int client_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    for (int s = 0; s < 4; ++s) g_server_macros[client_idx][s].running = false;
}

// ── Signal ────────────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

static uint8_t pro_timer_from_us(uint64_t t_us) {
    // The byte after report ID 0x30/0x21 is a small controller timer.  Real
    // controllers advance it with time, not merely by "one per report"; using
    // a 5ms unit matches the 3x IMU sample spacing most software expects.
    return (uint8_t)((t_us / 5000ULL) & 0xFF);
}

static MotionReport synthetic_imu_sample(uint64_t t_us, int sample_index) {
    // Deliberately large but valid Pro Controller-like IMU values.
    // This is diagnostic: if --test-imu roll/pitch/yaw does not move anything
    // in a motion-aware Switch screen/game, the issue is not SDL3 or axis maps;
    // the console is not accepting/using IMU from this USB gadget.
    const int phase = (int)(((t_us / 5000ULL) + (uint64_t)sample_index) & 0x3F);
    const bool hi = (phase & 0x20) != 0;
    const int s = hi ? 1 : -1;

    MotionReport m{};
    // Keep about 1g present so software that cares about gravity/orientation
    // does not see a physically impossible zero-G controller.
    m.ax = 0;
    m.ay = 4096;
    m.az = 0;

    switch (g_imu_test) {
        case ImuTestMode::Roll:
            m.gx = (int16_t)(s * 9000);
            m.ax = (int16_t)(s * 2800);
            m.ay = 2800;
            break;
        case ImuTestMode::Pitch:
            m.gy = (int16_t)(s * 9000);
            m.az = (int16_t)(s * 2800);
            m.ay = 2800;
            break;
        case ImuTestMode::Yaw:
            m.gz = (int16_t)(s * 9000);
            break;
        case ImuTestMode::Shake:
            m.gx = (int16_t)(s * 12000);
            m.gy = (int16_t)(-s * 9000);
            m.gz = (int16_t)(s * 6000);
            m.ax = (int16_t)(s * 5000);
            m.ay = (int16_t)(4096 + s * 1500);
            m.az = (int16_t)(-s * 3500);
            break;
        case ImuTestMode::Gentle:
            // More physically plausible than shake: steady gravity plus
            // a moderate angular velocity. Some consumers reject absurd IMU.
            m.ax = 0;
            m.ay = 4096;
            m.az = 0;
            m.gx = (int16_t)(s * 1800);
            m.gy = (int16_t)(-s * 900);
            m.gz = (int16_t)(s * 450);
            break;
        case ImuTestMode::Off:
            break;
    }
    return m;
}



// ── Nintendo Pro Controller USB protocol support ─────────────────────────────
static constexpr size_t PRO_REPORT_SIZE = 64;
// Real Nintendo full input reports are not spammed at the writer loop rate.
// hid-nintendo documents Pro Controller USB report 0x30 at about every 15ms,
// with 3 IMU samples spaced roughly 5ms apart.  Keep this configurable for
// testing, but default to the real cadence instead of 250Hz.
static uint64_t g_pro_report_interval_us = 15'000ULL;
// Standard input byte 2 is "battery + connection info". The old value 0x8E
// reports a low connection nibble with LSB 0. SDL/hid-nintendo notes that this
// byte carries connection/USB status, so keep it configurable while defaulting
// to a more normal wired/full battery style 0x81.
static uint8_t  g_pro_bat_con = 0x81;
// Byte 12 of report 0x30/0x21 is the controller's vibrator input/status byte.
// Usually 0 is fine, but keep it configurable for exact-protocol experiments.
static uint8_t  g_pro_vibrator_report = 0x00;
static bool     g_log_raw_30 = false;
static constexpr int PRO_IDLE_REPORT_HZ = 30;
static constexpr uint64_t PRO_IDLE_REPORT_INTERVAL_US = 1'000'000ULL / PRO_IDLE_REPORT_HZ;
static constexpr uint64_t PRO_RELEASE_NEUTRAL_US = 250'000ULL;
// Web/Gamepad APIs can occasionally report a connected pad as absent for one
// frame (especially while another tab/client is closing).  Do not release the
// Switch port instantly on a single absent sample, or held buttons like R get
// chopped into tiny taps.
static constexpr uint64_t WEB_PAD_ABSENT_RELEASE_US = 750'000ULL;
static constexpr uint8_t RID_INPUT_STANDARD = 0x30;
static constexpr uint8_t RID_INPUT_SUBCMD   = 0x21;
static constexpr uint8_t RID_OUTPUT_RUMBLE  = 0x10;
static constexpr uint8_t RID_OUTPUT_CMD     = 0x01;

static constexpr uint8_t CMD_BT_MANUAL_PAIRING   = 0x01;
static constexpr uint8_t CMD_GET_DEVICE_INFO     = 0x02;
static constexpr uint8_t CMD_SET_DATA_FORMAT     = 0x03;
static constexpr uint8_t CMD_TRIGGER_BUTTONS     = 0x04;
static constexpr uint8_t CMD_SET_SHIP_MODE       = 0x08;
static constexpr uint8_t CMD_SPI_FLASH_READ      = 0x10;
static constexpr uint8_t CMD_SPI_FLASH_WRITE     = 0x11;
static constexpr uint8_t CMD_SET_NFC_IR_CONFIG   = 0x21;
static constexpr uint8_t CMD_SET_PLAYER_LIGHTS   = 0x30;
static constexpr uint8_t CMD_ENABLE_IMU          = 0x40;
static constexpr uint8_t CMD_SET_IMU_SENS        = 0x41;
static constexpr uint8_t CMD_ENABLE_VIBRATION    = 0x48;

#define NS_LOCAL_PACKED __attribute__((packed))

struct NS_LOCAL_PACKED ProInputReport30 {
    uint8_t id;
    uint8_t timer;
    uint8_t conn_info;
    uint8_t buttons[3];
    uint8_t left_stick[3];
    uint8_t right_stick[3];
    uint8_t vibrator;
    int16_t accel_x_0, accel_y_0, accel_z_0;
    int16_t gyro_x_0,  gyro_y_0,  gyro_z_0;
    int16_t accel_x_1, accel_y_1, accel_z_1;
    int16_t gyro_x_1,  gyro_y_1,  gyro_z_1;
    int16_t accel_x_2, accel_y_2, accel_z_2;
    int16_t gyro_x_2,  gyro_y_2,  gyro_z_2;
    uint8_t vendor_rest[15];
};
static_assert(sizeof(ProInputReport30) == PRO_REPORT_SIZE, "ProInputReport30 must be 64 bytes");

struct NS_LOCAL_PACKED ProInputReport21 {
    uint8_t id;
    uint8_t timer;
    uint8_t conn_info;
    uint8_t buttons[3];
    uint8_t left_stick[3];
    uint8_t right_stick[3];
    uint8_t vibrator;
    uint8_t ack;
    uint8_t subcmd_id;
    uint8_t reply_data[49];
};
static_assert(sizeof(ProInputReport21) == PRO_REPORT_SIZE, "ProInputReport21 must be 64 bytes");

// Fresh virtual identities.  The old build used 98:B6:E9:11:22:33..36
// and matching serial strings; changing both forces the Switch to stop using
// any cached calibration/association for the previous fake controllers.
// 02 is a locally-administered unicast address, so these are intentionally
// private/stable virtual MACs rather than pretending to be real hardware.
static uint8_t CTRL_MAC_BE[4][6] = {
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA0},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA1},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA2},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA3},
};

static std::string CTRL_SERIAL[4] = {
    "NSGP260606A0", "NSGP260606A1", "NSGP260606A2", "NSGP260606A3"
};

static bool read_random_bytes(uint8_t* dst, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t r = read(fd, dst + off, len - off);
            if (r <= 0) break;
            off += (size_t)r;
        }
        close(fd);
        if (off == len) return true;
    }

    uint64_t seed = (uint64_t)now_us() ^ ((uint64_t)getpid() << 32);
    for (size_t i = 0; i < len; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        dst[i] = (uint8_t)(seed & 0xFF);
    }
    return true;
}

static void randomize_controller_identity() {
    uint8_t rnd[16]{};
    read_random_bytes(rnd, sizeof(rnd));

    // Locally administered unicast MACs. Keep 02:4E:53 ("NS") as a stable
    // virtual vendor prefix and randomize the low bytes so HOS cannot reuse
    // cached calibration/association for the previous fake controller.
    for (int i = 0; i < 4; ++i) {
        CTRL_MAC_BE[i][0] = 0x02;
        CTRL_MAC_BE[i][1] = 0x4E;
        CTRL_MAC_BE[i][2] = 0x53;
        CTRL_MAC_BE[i][3] = rnd[(i * 3 + 0) % sizeof(rnd)];
        CTRL_MAC_BE[i][4] = rnd[(i * 3 + 1) % sizeof(rnd)];
        CTRL_MAC_BE[i][5] = (uint8_t)(rnd[(i * 3 + 2) % sizeof(rnd)] + i);
        CTRL_SERIAL[i] = "NSGP";
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "%02X%02X%02X%02X",
                      CTRL_MAC_BE[i][2], CTRL_MAC_BE[i][3], CTRL_MAC_BE[i][4], CTRL_MAC_BE[i][5]);
        CTRL_SERIAL[i] += suffix;
    }

    char usb_ser[32];
    std::snprintf(usb_ser, sizeof(usb_ser), "%02X%02X%02X%02X%02X%02X",
                  rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
    g_usb_serial = usb_ser;
}

static void print_controller_identity() {
    if (!g_verbose) return;
    std::printf("[identity] usb_serial=%s\n", g_usb_serial.c_str());
    for (int i = 0; i < g_hid_port_count; ++i) {
        std::printf("[identity] pad%d MAC=%02X:%02X:%02X:%02X:%02X:%02X serial=%s\n",
                    i + 1,
                    CTRL_MAC_BE[i][0], CTRL_MAC_BE[i][1], CTRL_MAC_BE[i][2],
                    CTRL_MAC_BE[i][3], CTRL_MAC_BE[i][4], CTRL_MAC_BE[i][5],
                    CTRL_SERIAL[i].c_str());
    }
}

static constexpr size_t SPI_FLASH_SIZE = 0x10000;
static uint8_t g_spi_flash[4][SPI_FLASH_SIZE];
static bool g_spi_initialized[4] = {};

struct ControllerRuntime {
    int fd = -1;
    int ctrl = 0;
    uint8_t timer = 0;
    bool full_report_enabled = false;
    bool imu_enabled = false;
    bool vibration_enabled = false;
    bool usb_seen_mac = false;
    bool usb_handshake_done = false;
    bool usb_baudrate_set = false;
    bool usb_timeout_disabled = false;
    bool pending_subcmd_reply = false;
    uint64_t last_standard_report_us = 0;
    uint64_t last_idle_neutral_us = 0;
    uint64_t neutral_burst_until_us = 0;
    ProInputReport21 pending_reply{};
};

[[maybe_unused]] static int16_t clamp_i16(int v) {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
}

[[maybe_unused]] static void pack12(uint16_t val, uint8_t& b0, uint8_t& b1) {
    b0 = val & 0xFF;
    b1 = (b1 & 0xF0) | ((val >> 8) & 0x0F);
}

static void init_spi_flash(int ctrl) {
    if (ctrl < 0 || ctrl >= 4 || g_spi_initialized[ctrl]) return;

    uint8_t* flash = g_spi_flash[ctrl];
    memset(flash, 0xFF, SPI_FLASH_SIZE);

    // Do NOT put the serial at 0x6080.  The Switch/Chromium/Linux init
    // sequence reads 0x6080 as IMU horizontal offsets and 0x6086 as analog
    // stick parameters.  The previous build wrote the ASCII serial there,
    // which produced a huge bogus stick deadzone/range block and could make
    // the left stick look completely dead even though reports contained lx/ly.
    flash[0x6012] = 0x03;
    flash[0x6013] = 0xA0;
    flash[0x601B] = 0x02;

    // Stick calibration must be valid for BOTH sticks.  The Switch reads
    // 9 bytes per stick.  Left and right use different field orders:
    //   left:  max-above-center, center, min-below-center
    //   right: center, min-below-center, max-above-center
    // Also expose the same values as USER calibration.  This avoids a bad
    // cached/old factory-cal path making one analogue stick get flattened.
    auto pack_stick_cal_pair = [](uint8_t* dst, uint16_t x, uint16_t y) {
        x &= 0x0FFF;
        y &= 0x0FFF;
        dst[0] = x & 0xFF;
        dst[1] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4);
        dst[2] = (y >> 4) & 0xFF;
    };

    static constexpr uint16_t STICK_CENTER = 0x800;
    static constexpr uint16_t STICK_RANGE  = 0x600;

    uint8_t left_cal[9] = {};
    uint8_t right_cal[9] = {};

    // Left stick: range above center, center, range below center.
    pack_stick_cal_pair(left_cal + 0, STICK_RANGE,  STICK_RANGE);
    pack_stick_cal_pair(left_cal + 3, STICK_CENTER, STICK_CENTER);
    pack_stick_cal_pair(left_cal + 6, STICK_RANGE,  STICK_RANGE);

    // Right stick: center, range below center, range above center.
    pack_stick_cal_pair(right_cal + 0, STICK_CENTER, STICK_CENTER);
    pack_stick_cal_pair(right_cal + 3, STICK_RANGE,  STICK_RANGE);
    pack_stick_cal_pair(right_cal + 6, STICK_RANGE,  STICK_RANGE);

    // Factory calibration addresses.
    memcpy(flash + 0x603D, left_cal,  sizeof(left_cal));
    memcpy(flash + 0x6046, right_cal, sizeof(right_cal));

    // User calibration magic + data.  The Switch/Linux driver checks 0xB2 0xA1
    // before using the user calibration block.
    flash[0x8010] = 0xB2; flash[0x8011] = 0xA1;
    memcpy(flash + 0x8012, left_cal, sizeof(left_cal));
    flash[0x801B] = 0xB2; flash[0x801C] = 0xA1;
    memcpy(flash + 0x801D, right_cal, sizeof(right_cal));

    // SPI 0x6080..0x6085: IMU horizontal offsets, 3 little-endian int16s.
    // Zero is the neutral/safe value for a virtual controller.
    memset(flash + 0x6080, 0x00, 6);

    // SPI 0x6086..0x6097: analog stick parameters.  Known consumers unpack
    // bytes 3..5 as two packed 12-bit values: deadzone and range ratio.
    // Keep the deadzone small and explicit instead of leaving 0xFF or serial
    // text here, because that can make real stick movement get snapped to
    // center.
    uint8_t stick_params[18] = {};
    static constexpr uint16_t STICK_DEADZONE    = 0x0A0; // 160
    static constexpr uint16_t STICK_RANGE_RATIO = 0x100;
    pack_stick_cal_pair(stick_params + 3, STICK_DEADZONE, STICK_RANGE_RATIO);
    memcpy(flash + 0x6086, stick_params, sizeof(stick_params));

    // Factory IMU calibration block.
    // Some Switch init paths read 0x6020 before/alongside 0x6098. Leaving this
    // as 0xFF can make the console accept 0x30 reports but ignore or badly
    // scale the IMU samples. Mirror a sane virtual calibration here too:
    //   gyro offsets  = 0
    //   accel offsets = 0
    //   accel scale   = 0x1000
    //   gyro scale    = 0x33D0
    auto put_i16_le = [&](uint16_t addr, int16_t val) {
        flash[addr]     = (uint8_t)(val & 0xFF);
        flash[addr + 1] = (uint8_t)((val >> 8) & 0xFF);
    };

    // hid-nintendo parses the 24-byte IMU calibration block as:
    //   +0:  accel offsets XYZ
    //   +6:  accel scales  XYZ
    //   +12: gyro offsets  XYZ
    //   +18: gyro scales   XYZ
    //
    // The previous build accidentally wrote gyro offsets where accel scales
    // belong, making accel scale read as zero and gyro offset read as 0x1000.
    // That is poison for any host that validates/calibrates IMU before using it.
    static constexpr int16_t IMU_ACCEL_OFFSET = 0;
    static constexpr int16_t IMU_ACCEL_SCALE  = 16384; // hid-nintendo default, 0x4000
    static constexpr int16_t IMU_GYRO_OFFSET  = 0;
    static constexpr int16_t IMU_GYRO_SCALE   = 13371; // hid-nintendo default

    const int16_t imu_vals[12] = {
        IMU_ACCEL_OFFSET, IMU_ACCEL_OFFSET, IMU_ACCEL_OFFSET,
        IMU_ACCEL_SCALE,  IMU_ACCEL_SCALE,  IMU_ACCEL_SCALE,
        IMU_GYRO_OFFSET,  IMU_GYRO_OFFSET,  IMU_GYRO_OFFSET,
        IMU_GYRO_SCALE,   IMU_GYRO_SCALE,   IMU_GYRO_SCALE
    };
    for (int i = 0; i < 12; ++i)
        put_i16_le((uint16_t)(0x6020 + i * 2), imu_vals[i]);

    // Body/grip colors per virtual Pro Controller:
    //   1 = red, 2 = yellow, 3 = blue, 4 = green.
    // Buttons stay white for readability in the Switch UI.
    static const uint8_t BODY_RGB[4][3] = {
        {0xE6, 0x00, 0x12}, // red
        {0xFF, 0xCC, 0x00}, // yellow
        {0x00, 0x64, 0xFF}, // blue
        {0x00, 0xC8, 0x53}, // green
    };
    const uint8_t* body = BODY_RGB[ctrl];
    flash[0x6050] = body[0]; flash[0x6051] = body[1]; flash[0x6052] = body[2];
    flash[0x6053] = 0xFF;    flash[0x6054] = 0xFF;    flash[0x6055] = 0xFF;
    flash[0x6056] = body[0]; flash[0x6057] = body[1]; flash[0x6058] = body[2];
    flash[0x6059] = 0xFF;    flash[0x605A] = 0xFF;    flash[0x605B] = 0xFF;
    flash[0x605C] = 0x00;

    // Important: 0x6098 is NOT IMU calibration.  Joy-Con/Pro-controller
    // tools read it as the second half of the 0x6086..0x60A9 stick-parameter
    // model block.  The previous test build mirrored IMU calibration here,
    // which made the console read nonsense stick-model parameters.  Keep it
    // neutral/sane instead.
    memset(flash + 0x6098, 0x00, 0x12);

    // User IMU calibration lives at 0x8026/0x8028 according to hid-nintendo.
    // Make it optional: if disabled, the magic stays non-matching and the host
    // should fall back to factory IMU calibration at 0x6020.
    if (!g_no_user_imu_cal) {
        flash[0x8026] = 0xB2;
        flash[0x8027] = 0xA1;
        for (int i = 0; i < 12; ++i)
            put_i16_le((uint16_t)(0x8028 + i * 2), imu_vals[i]);
    }

    g_spi_initialized[ctrl] = true;
}

static void set_identity_in_0x81(uint8_t* resp_81, int ctrl) {
    const uint8_t* mac = CTRL_MAC_BE[ctrl];
    // USB 0x81 MAC reply stores MAC little-endian in bytes 4..9, matching
    // Chromium's MacAddressReport/UnpackSwitchMacAddress handling.
    resp_81[4] = mac[5]; resp_81[5] = mac[4]; resp_81[6] = mac[3];
    resp_81[7] = mac[2]; resp_81[8] = mac[1]; resp_81[9] = mac[0];
}

static size_t build_usb_81_response(uint8_t* out, uint8_t subtype, int ctrl) {
    memset(out, 0, PRO_REPORT_SIZE);
    out[0] = 0x81;
    out[1] = subtype;
    switch (subtype) {
    case 0x01: // request MAC/address/device type
        out[2] = 0x00; // padding
        out[3] = 0x03; // Pro Controller
        set_identity_in_0x81(out, ctrl);
        break;
    case 0x02: // USB handshake
    case 0x03: // set UART/baudrate
    case 0x04: // disable USB timeout; real devices may not ACK, but an ACK is accepted
    case 0x05: // enable USB timeout
    default:
        // Chromium and hid-nintendo only require report 0x81 subtype to advance
        // these USB-init steps. Keep remaining bytes zero, like a minimal ACK.
        break;
    }
    return PRO_REPORT_SIZE;
}

static void build_get_device_info_response(uint8_t* out, int ctrl) {
    memset(out, 0, 36);
    out[0] = 0x03; // firmware major
    out[1] = 0x48; // firmware minor
    out[2] = 0x03; // Pro Controller
    out[3] = 0x02; // hardware model

    const uint8_t* mac = CTRL_MAC_BE[ctrl];
    out[4] = mac[5]; out[5] = mac[4]; out[6] = mac[3];
    out[7] = mac[2]; out[8] = mac[1]; out[9] = mac[0];

    out[10] = 0x03;
    out[11] = 0x01;
}

static void fill_neutral_controls(ProInputReport30& r) {
    r.conn_info = g_pro_bat_con;
    r.left_stick[0]  = 0x00; r.left_stick[1]  = 0x08; r.left_stick[2]  = 0x80;
    r.right_stick[0] = 0x00; r.right_stick[1] = 0x08; r.right_stick[2] = 0x80;
    r.vibrator = g_pro_vibrator_report;
}

static void fill_neutral_controls(ProInputReport21& r) {
    r.conn_info = g_pro_bat_con;
    r.left_stick[0]  = 0x00; r.left_stick[1]  = 0x08; r.left_stick[2]  = 0x80;
    r.right_stick[0] = 0x00; r.right_stick[1] = 0x08; r.right_stick[2] = 0x80;
    r.vibrator = g_pro_vibrator_report;
}

static uint16_t axis8_to_12(uint8_t v) {
    // Match the fake calibration above: center 0x800 with about ±0x600 range.
    // Sending the full 0x000..0xFFF range can sit outside the advertised
    // calibration and some Switch paths appear to flatten/ignore the stick.
    if (v == 128) return 0x800;

    int32_t delta = (int32_t)v - 128;
    int32_t raw;
    if (delta > 0)
        raw = 0x800 + (delta * 0x600) / 127;
    else
        raw = 0x800 + (delta * 0x600) / 128;

    if (raw < 0x200) raw = 0x200;
    if (raw > 0xE00) raw = 0xE00;
    return (uint16_t)raw;
}

static uint8_t invert_axis8_centered(uint8_t v) {
    // 0 and 255 should swap, but keep the protocol's exact neutral value
    // neutral.  A raw 255-v inversion turns 128 into 127, which creates a tiny
    // permanent off-center Y value.
    return v == 128 ? 128 : (uint8_t)(255 - v);
}

static void pack_stick_12(uint8_t out[3], uint8_t x8, uint8_t y8) {
    // Input protocol uses 0 = up/left and 255 = down/right.  The Switch raw
    // stick format has Y in the opposite direction, so invert Y once here for
    // both sticks.
    uint16_t x = axis8_to_12(x8);
    uint16_t y = axis8_to_12(invert_axis8_centered(y8));
    out[0] = x & 0xFF;
    out[1] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4);
    out[2] = (y >> 4) & 0xFF;
}

static bool input_is_neutral(const HIDReport& r) {
    return r.buttons == 0 && r.hat == HAT_NEUTRAL &&
           r.lx == 128 && r.ly == 128 && r.rx == 128 && r.ry == 128;
}

static bool motion_is_neutral(const MotionReport& m) {
    return std::abs((int)m.ax) < 64 && std::abs((int)m.ay) < 64 && std::abs((int)m.az) < 64 &&
           std::abs((int)m.gx) < 64 && std::abs((int)m.gy) < 64 && std::abs((int)m.gz) < 64;
}

static bool extended_is_neutral(const ExtendedHIDReport& r) {
    return input_is_neutral(r.input) && (!r.has_motion || motion_is_neutral(r.motion));
}

static void hat_to_pro_buttons(uint8_t hat, uint8_t buttons[3]) {
    bool up = false, down = false, left = false, right = false;
    switch (hat) {
        case HAT_N:  up = true; break;
        case HAT_NE: up = true; right = true; break;
        case HAT_E:  right = true; break;
        case HAT_SE: down = true; right = true; break;
        case HAT_S:  down = true; break;
        case HAT_SW: down = true; left = true; break;
        case HAT_W:  left = true; break;
        case HAT_NW: up = true; left = true; break;
        default: break;
    }
    if (down)  buttons[2] |= 0x01;
    if (up)    buttons[2] |= 0x02;
    if (right) buttons[2] |= 0x04;
    if (left)  buttons[2] |= 0x08;
}

static void apply_input_controls_to_pro21(const ExtendedHIDReport& src, ProInputReport21& out) {
    // Subcommand replies (report 0x21) contain the same button/stick snapshot
    // fields as standard input reports.  Keep them in sync with the currently
    // held web/UDP input; otherwise frequent Switch output/subcommand traffic
    // injects neutral frames between normal 0x30 reports, which makes held
    // buttons such as R/ZR flicker in-game.
    out.conn_info = g_pro_bat_con;
    memset(out.buttons, 0, sizeof(out.buttons));
    out.vibrator = g_pro_vibrator_report;

    const HIDReport& in = src.input;
    if (in.buttons & BTN_Y)       out.buttons[0] |= 0x01;
    if (in.buttons & BTN_X)       out.buttons[0] |= 0x02;
    if (in.buttons & BTN_B)       out.buttons[0] |= 0x04;
    if (in.buttons & BTN_A)       out.buttons[0] |= 0x08;
    if (in.buttons & BTN_R)       out.buttons[0] |= 0x40;
    if (in.buttons & BTN_ZR)      out.buttons[0] |= 0x80;

    if (in.buttons & BTN_MINUS)   out.buttons[1] |= 0x01;
    if (in.buttons & BTN_PLUS)    out.buttons[1] |= 0x02;
    if (in.buttons & BTN_RSTICK)  out.buttons[1] |= 0x04;
    if (in.buttons & BTN_LSTICK)  out.buttons[1] |= 0x08;
    if (in.buttons & BTN_HOME)    out.buttons[1] |= 0x10;
    if (in.buttons & BTN_CAPTURE) out.buttons[1] |= 0x20;

    hat_to_pro_buttons(in.hat, out.buttons);
    if (in.buttons & BTN_L)       out.buttons[2] |= 0x40;
    if (in.buttons & BTN_ZL)      out.buttons[2] |= 0x80;

    pack_stick_12(out.left_stick,  in.lx, in.ly);
    pack_stick_12(out.right_stick, in.rx, in.ry);
}

static void build_standard_report(const ExtendedHIDReport& src,
                                  const MotionReport motion_history[3],
                                  uint8_t motion_history_count,
                                  bool imu_enabled,
                                  uint8_t timer,
                                  ProInputReport30& out) {
    memset(&out, 0, sizeof(out));
    out.id = RID_INPUT_STANDARD;
    out.timer = timer;
    fill_neutral_controls(out);

    const HIDReport& in = src.input;
    if (in.buttons & BTN_Y)       out.buttons[0] |= 0x01;
    if (in.buttons & BTN_X)       out.buttons[0] |= 0x02;
    if (in.buttons & BTN_B)       out.buttons[0] |= 0x04;
    if (in.buttons & BTN_A)       out.buttons[0] |= 0x08;
    if (in.buttons & BTN_R)       out.buttons[0] |= 0x40;
    if (in.buttons & BTN_ZR)      out.buttons[0] |= 0x80;

    if (in.buttons & BTN_MINUS)   out.buttons[1] |= 0x01;
    if (in.buttons & BTN_PLUS)    out.buttons[1] |= 0x02;
    if (in.buttons & BTN_RSTICK)  out.buttons[1] |= 0x04;
    if (in.buttons & BTN_LSTICK)  out.buttons[1] |= 0x08;
    if (in.buttons & BTN_HOME)    out.buttons[1] |= 0x10;
    if (in.buttons & BTN_CAPTURE) out.buttons[1] |= 0x20;

    hat_to_pro_buttons(in.hat, out.buttons);
    if (in.buttons & BTN_L)       out.buttons[2] |= 0x40;
    if (in.buttons & BTN_ZL)      out.buttons[2] |= 0x80;

    pack_stick_12(out.left_stick,  in.lx, in.ly);
    pack_stick_12(out.right_stick, in.rx, in.ry);

    MotionReport imu[3]{};
    bool has_imu = false;
    if (imu_enabled && g_imu_test != ImuTestMode::Off) {
        has_imu = true;
        uint64_t t = now_us();
        for (int i = 0; i < 3; ++i)
            imu[i] = remap_motion_for_switch(synthetic_imu_sample(t, i));
    } else {
        has_imu = imu_enabled && motion_history && motion_history_count > 0;
        if (has_imu) {
            for (int i = 0; i < 3; ++i) {
                if (i < motion_history_count) imu[i] = remap_motion_for_switch(motion_history[i]);
                else imu[i] = remap_motion_for_switch(motion_history[motion_history_count - 1]);
            }
        }
    }

    if (g_verbose && (!input_is_neutral(in) || has_imu)) {
        static uint64_t last_log_us = 0;
        uint64_t t = now_us();
        if (t - last_log_us > 250000) {
            last_log_us = t;
            std::printf("[input] lx=%3u ly=%3u rx=%3u ry=%3u | L=%02X %02X %02X R=%02X %02X %02X | imu=%s map=%s layout=%s test=%s samples=%u ax=%6d ay=%6d az=%6d gx=%6d gy=%6d gz=%6d\n",
                        in.lx, in.ly, in.rx, in.ry,
                        out.left_stick[0], out.left_stick[1], out.left_stick[2],
                        out.right_stick[0], out.right_stick[1], out.right_stick[2],
                        has_imu ? "yes" : "no",
                        imu_map_name(g_imu_map),
                        imu_layout_name(g_imu_layout),
                        imu_test_name(g_imu_test),
                        (unsigned)(has_imu ? (g_imu_test != ImuTestMode::Off ? 3 : motion_history_count) : 0),
                        (int)imu[0].ax, (int)imu[0].ay, (int)imu[0].az,
                        (int)imu[0].gx, (int)imu[0].gy, (int)imu[0].gz);
        }
    }

    auto store_imu_sample = [](ProInputReport30& dst, int idx, const MotionReport& m) {
        int16_t first0, first1, first2, second0, second1, second2;
        if (g_imu_layout == ImuWireLayout::AccelThenGyro) {
            first0 = m.ax; first1 = m.ay; first2 = m.az;
            second0 = m.gx; second1 = m.gy; second2 = m.gz;
        } else {
            first0 = m.gx; first1 = m.gy; first2 = m.gz;
            second0 = m.ax; second1 = m.ay; second2 = m.az;
        }

        // ProInputReport30 is packed, so GCC does not allow binding its fields
        // to int16_t&.  Assign fields directly instead.
        if (idx == 0) {
            dst.accel_x_0 = first0;  dst.accel_y_0 = first1;  dst.accel_z_0 = first2;
            dst.gyro_x_0  = second0; dst.gyro_y_0  = second1; dst.gyro_z_0  = second2;
        } else if (idx == 1) {
            dst.accel_x_1 = first0;  dst.accel_y_1 = first1;  dst.accel_z_1 = first2;
            dst.gyro_x_1  = second0; dst.gyro_y_1  = second1; dst.gyro_z_1  = second2;
        } else {
            dst.accel_x_2 = first0;  dst.accel_y_2 = first1;  dst.accel_z_2 = first2;
            dst.gyro_x_2  = second0; dst.gyro_y_2  = second1; dst.gyro_z_2  = second2;
        }
    };

    store_imu_sample(out, 0, imu[0]);
    store_imu_sample(out, 1, imu[1]);
    store_imu_sample(out, 2, imu[2]);
}

static int handle_subcommand(ControllerRuntime& rt, uint8_t subcmd, const uint8_t* cmd_data, size_t cmd_len, ProInputReport21* reply) {
    memset(reply->reply_data, 0, sizeof(reply->reply_data));
    reply->ack = 0x80;
    reply->subcmd_id = subcmd;

    switch (subcmd) {
    case CMD_BT_MANUAL_PAIRING:
        reply->ack = 0x81;
        reply->reply_data[0] = 0x03;
        reply->reply_data[1] = 0x01;
        return 2;

    case CMD_GET_DEVICE_INFO: {
        uint8_t info[36];
        build_get_device_info_response(info, rt.ctrl);
        reply->ack = 0x82;
        memcpy(reply->reply_data, info, 36);
        return 36;
    }

    case CMD_SET_DATA_FORMAT:
        rt.full_report_enabled = true;
        reply->ack = 0x80;
        reply->reply_data[0] = cmd_len > 0 ? cmd_data[0] : 0x30;
        return 1;

    case CMD_TRIGGER_BUTTONS:
    case CMD_SET_SHIP_MODE:
    case CMD_SPI_FLASH_WRITE:
    case CMD_SET_IMU_SENS:
        reply->ack = 0x80;
        return 0;

    case CMD_SPI_FLASH_READ: {
        if (cmd_len < 5) {
            reply->ack = 0x00;
            return 0;
        }
        uint32_t addr = ((uint32_t)cmd_data[0]) |
                        ((uint32_t)cmd_data[1] << 8) |
                        ((uint32_t)cmd_data[2] << 16) |
                        ((uint32_t)cmd_data[3] << 24);
        uint8_t size = cmd_data[4];
        if (size > 44) size = 44;

        reply->ack = 0x90;
        reply->reply_data[0] = cmd_data[0];
        reply->reply_data[1] = cmd_data[1];
        reply->reply_data[2] = cmd_data[2];
        reply->reply_data[3] = cmd_data[3];
        reply->reply_data[4] = size;

        uint8_t* flash = g_spi_flash[rt.ctrl];
        if (addr < SPI_FLASH_SIZE) {
            size_t to_copy = std::min((size_t)size, (size_t)(SPI_FLASH_SIZE - addr));
            memcpy(reply->reply_data + 5, flash + addr, to_copy);
            if (to_copy < size) memset(reply->reply_data + 5 + to_copy, 0xFF, size - to_copy);
        } else {
            memset(reply->reply_data + 5, 0xFF, size);
        }
        if (g_verbose) {
            std::printf("[pro%d] SPI read addr=0x%04X size=%u", rt.ctrl + 1, addr, size);
            if (addr == 0x6020 || addr == 0x8026 || addr == 0x8028 || addr == 0x6080 || addr == 0x6086 || addr == 0x6098) {
                std::printf(" data=");
                for (uint8_t i = 0; i < size && i < 32 && addr + i < SPI_FLASH_SIZE; ++i)
                    std::printf("%02X%s", flash[addr + i], (i + 1 < size && i + 1 < 32 && addr + i + 1 < SPI_FLASH_SIZE) ? " " : "");
            }
            std::printf("\n");
        }
        return 5 + size;
    }

    case CMD_SET_NFC_IR_CONFIG:
        // The Switch may poll the NFC/IR MCU configuration even for a normal
        // Pro Controller.  The previous fake data reply made some consoles keep
        // repeating subcommand 0x21 forever, which flooded report 0x21 replies.
        // A plain successful ACK is enough for a controller without NFC/IR and
        // lets the console settle back to steady 0x30 input reports with IMU data.
        reply->ack = 0x80;
        return 0;

    case CMD_SET_PLAYER_LIGHTS:
        reply->ack = 0x80;
        reply->reply_data[0] = cmd_len > 0 ? cmd_data[0] : 0;
        return 1;

    case 0x33:
        // Chromium sends this as an acknowledged no-op immediately after
        // disabling the USB timeout. Real hardware accepts it as a normal
        // subcommand; treating it as failure can stall init sequences.
        reply->ack = 0x80;
        return 0;

    case CMD_ENABLE_IMU:
        rt.imu_enabled = (cmd_len == 0) || cmd_data[0] != 0;
        reply->ack = 0x80;
        reply->reply_data[0] = cmd_len > 0 ? cmd_data[0] : 1;
        return 1;

    case CMD_ENABLE_VIBRATION:
        rt.vibration_enabled = (cmd_len == 0) || cmd_data[0] != 0;
        reply->ack = 0x80;
        reply->reply_data[0] = cmd_len > 0 ? cmd_data[0] : 1;
        return 1;

    default:
        // Real controllers tend to ACK unknown-but-well-formed subcommands
        // rather than poison the init state machine. Keep success ACK so the
        // Switch can continue if it sends a harmless command we do not parse.
        reply->ack = 0x80;
        return 0;
    }
}

static void publish_rumble_event(int client_idx, int sub_idx, const uint8_t* packet, ssize_t len, bool publish_neutral) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4 || len < 10)
        return;

    static const uint8_t neutral_rumble[8] = {0x00,0x01,0x40,0x40,0x00,0x01,0x40,0x40};
    const uint8_t* rb = packet + 2;
    bool all_zero = true;
    for (int i = 0; i < 8; ++i) if (rb[i] != 0) { all_zero = false; break; }
    bool neutral = all_zero || memcmp(rb, neutral_rumble, 8) == 0;
    if (neutral && !publish_neutral && !g_log_neutral_rumble)
        return;

    uint8_t low = 0, high = 0;
    if (!neutral) {
        // Nintendo HD-rumble is a pair of packed 4-byte LRA frames. SDL clients
        // expose classic low/high magnitudes, so collapse the Switch frame into
        // a conservative strength by measuring distance from the known neutral
        // carrier on each side. This avoids treating the neutral carrier itself
        // as vibration.
        int diff_left = 0, diff_right = 0;
        for (int i = 0; i < 4; ++i) {
            diff_left  = std::max(diff_left,  std::abs((int)rb[i]     - (int)neutral_rumble[i]));
            diff_right = std::max(diff_right, std::abs((int)rb[i + 4] - (int)neutral_rumble[i + 4]));
        }
        low  = (uint8_t)std::clamp(64 + diff_left  * 3, 80, 255);
        high = (uint8_t)std::clamp(64 + diff_right * 3, 80, 255);
    }

    std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
    if (neutral && !g_clients[client_idx].rumble_active[sub_idx]) {
        // The Switch sends the neutral carrier constantly. Forwarding every
        // neutral frame creates haptic spam and hides real rumble debugging.
        if (g_verbose && g_log_neutral_rumble) {
            static uint64_t last_neutral_log_us[MAX_CLIENTS][4] = {};
            uint64_t t = now_us();
            if (t - last_neutral_log_us[client_idx][sub_idx] > 1'000'000ULL) {
                last_neutral_log_us[client_idx][sub_idx] = t;
                std::printf("[rumble-neutral] client=%d pad=%d raw=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                            client_idx + 1, sub_idx + 1,
                            rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7]);
            }
        }
        return;
    }

    RumblePacket& ev = g_clients[client_idx].rumble[sub_idx];
    ev.magic = RUMBLE_MAGIC;
    ev.subpad = (uint8_t)sub_idx;
    ev.low_freq = neutral ? 0 : low;
    ev.high_freq = neutral ? 0 : high;
    ev.duration_10ms = neutral ? 0 : 6;
    g_clients[client_idx].rumble_active[sub_idx] = !neutral;
    g_clients[client_idx].rumble_seq[sub_idx]++;

    if (g_verbose) {
        std::printf("[rumble] client=%d pad=%d low=%u high=%u duration=%u neutral=%s raw=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                    client_idx + 1, sub_idx + 1, ev.low_freq, ev.high_freq,
                    ev.duration_10ms, neutral ? "yes" : "no",
                    rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7]);
    }
}

static void queue_debug_test_rumble_if_needed(int client_idx, int sub_idx) {
    if (!g_test_rumble_on_connect) return;
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4) return;

    std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
    ClientSession& c = g_clients[client_idx];
    if (!c.active || c.debug_rumble_sent[sub_idx]) return;

    RumblePacket& ev = c.rumble[sub_idx];
    ev.magic = RUMBLE_MAGIC;
    ev.subpad = (uint8_t)sub_idx;
    ev.low_freq = 180;
    ev.high_freq = 220;
    ev.duration_10ms = 25; // 250 ms
    c.rumble_active[sub_idx] = true;
    c.debug_rumble_sent[sub_idx] = true;
    c.rumble_seq[sub_idx]++;

    if (g_verbose)
        std::printf("[rumble-test] queued client=%d pad=%d low=%u high=%u duration=%u\n",
                    client_idx + 1, sub_idx + 1, ev.low_freq, ev.high_freq, ev.duration_10ms);
}



static constexpr const char* GADGET_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl";
static constexpr const char* CONFIG_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1";

// Nintendo Switch Pro Controller descriptor, same 64-byte input/output report
// descriptor previously written by setup_gadget.sh.
static const uint8_t PRO_CONTROLLER_REPORT_DESC[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0A, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0A, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0B, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0B, 0x01, 0x00, 0x01, 0x00, 0xA1, 0x00, 0x0B, 0x30, 0x00,
    0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00, 0x0B, 0x32, 0x00, 0x01, 0x00, 0x0B, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xC0, 0x0B,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19, 0x0F, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x34, 0x81, 0x03, 0x06, 0x00, 0xFF, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, 0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0xC0
};

static bool hidg_nodes_ready() {
    for (int i = 0; i < g_hid_port_count; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/hidg%d", i);
        if (access(path, R_OK | W_OK) != 0)
            return false;
    }
    return true;
}

static bool dir_exists(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_exists(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0;
}

static bool mkdir_if_needed(const char* path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return true;
    std::fprintf(stderr, "[gadget] mkdir %s failed: %s\n", path, std::strerror(errno));
    return false;
}

static bool write_all_fd(int fd, const void* data, size_t len, const char* path) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "[gadget] write %s failed: %s\n", path, std::strerror(errno));
            return false;
        }
        if (w == 0) {
            std::fprintf(stderr, "[gadget] write %s wrote 0 bytes\n", path);
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    return true;
}

static bool write_bytes_file(const char* path, const void* data, size_t len) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        std::fprintf(stderr, "[gadget] open %s failed: %s\n", path, std::strerror(errno));
        return false;
    }
    bool ok = write_all_fd(fd, data, len, path);
    close(fd);
    return ok;
}

static bool write_text_file(const char* path, const char* text) {
    return write_bytes_file(path, text, std::strlen(text));
}

static void remove_link_if_exists(const char* path) {
    if (unlink(path) != 0) {
        if (errno != ENOENT && errno != EISDIR && errno != EPERM) {
            if (g_verbose) {
                std::fprintf(stderr, "[gadget] unlink %s failed: %s\n",
                             path, std::strerror(errno));
            }
        }
    }
}

static void rmdir_if_exists(const char* path) {
    if (rmdir(path) != 0 && errno != ENOENT) {
        if (g_verbose)
            std::fprintf(stderr, "[gadget] rmdir %s failed: %s\n", path, std::strerror(errno));
    }
}

static std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty() || a.back() == '/') return a + b;
    return a + "/" + b;
}

static std::string first_udc_name() {
    DIR* d = opendir("/sys/class/udc");
    if (!d) return "";
    std::string out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        out = e->d_name;
        break;
    }
    closedir(d);
    return out;
}

static bool create_hid_function(int id) {
    char func[256];
    std::snprintf(func, sizeof(func), "%s/functions/hid.usb%d", GADGET_DIR, id);
    if (!mkdir_if_needed(func)) return false;

    char path[320];
    std::snprintf(path, sizeof(path), "%s/protocol", func);
    if (!write_text_file(path, "0")) return false;

    std::snprintf(path, sizeof(path), "%s/subclass", func);
    if (!write_text_file(path, "0")) return false;

    std::snprintf(path, sizeof(path), "%s/report_length", func);
    if (!write_text_file(path, "64")) return false;

    std::snprintf(path, sizeof(path), "%s/report_desc", func);
    if (!write_bytes_file(path, PRO_CONTROLLER_REPORT_DESC, sizeof(PRO_CONTROLLER_REPORT_DESC))) return false;

    char link_path[320];
    std::snprintf(link_path, sizeof(link_path), "%s/hid.usb%d", CONFIG_DIR, id);
    unlink(link_path);
    if (symlink(func, link_path) != 0) {
        std::fprintf(stderr, "[gadget] symlink %s -> %s failed: %s\n",
                     link_path, func, std::strerror(errno));
        return false;
    }

    return true;
}

static void teardown_gadget() {
    if (!path_exists(GADGET_DIR)) return;

    std::puts("[gadget] Closing USB gadget...");

    // Unbind first.  This disconnects the virtual controllers from the Switch.
    std::string udc_path = join_path(GADGET_DIR, "UDC");
    write_text_file(udc_path.c_str(), "");

    // Remove config links before removing functions, mirroring setup_gadget.sh.
    for (int i = 0; i < 4; ++i) {
        char link_path[320];
        std::snprintf(link_path, sizeof(link_path), "%s/hid.usb%d", CONFIG_DIR, i);
        remove_link_if_exists(link_path);
    }

    // Configfs object directories are removed with rmdir; their pseudo-attribute
    // files must not be unlinked manually.
    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1/strings/0x409");
    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1");

    for (int i = 0; i < 4; ++i) {
        char func[256];
        std::snprintf(func, sizeof(func), "%s/functions/hid.usb%d", GADGET_DIR, i);
        rmdir_if_exists(func);
    }

    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/strings/0x409");
    rmdir_if_exists(GADGET_DIR);

    std::puts("[gadget] USB gadget closed");
}

static int run_shell_best_effort(const char* cmd) {
    int rc = std::system(cmd);
    return rc;
}

static bool setup_gadget_builtin(bool force, const char* reason) {
    if (!g_auto_gadget_setup && !force)
        return hidg_nodes_ready();

    // Non-forced calls are still cheap/retry-safe for internal recovery paths,
    // but startup passes force=true so the gadget is always torn down and
    // recreated from a known-good state.
    if (!force && hidg_nodes_ready())
        return true;

    if (!force && g_gadget_setup_attempted.exchange(true))
        return hidg_nodes_ready();
    if (force)
        g_gadget_setup_attempted.store(true);

    if (geteuid() != 0) {
        std::fprintf(stderr,
            "[gadget] requested /dev/hidg* nodes are not ready and built-in setup needs root.\n"
            "[gadget] Run: sudo ./ns-backend ...\n");
        return false;
    }

    std::printf("[gadget] %s; creating built-in %d-Pro-Controller gadget\n",
                reason ? reason : "HID gadget not ready", g_hid_port_count);

    // Try to load and mount configfs.  Ignore failures here because both may
    // already be active on systems that previously used setup_gadget.sh.
    (void)run_shell_best_effort("modprobe libcomposite >/dev/null 2>&1 || true");
    (void)run_shell_best_effort("mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config >/dev/null 2>&1 || true");

    if (!dir_exists("/sys/kernel/config/usb_gadget")) {
        std::fprintf(stderr,
            "[gadget] /sys/kernel/config/usb_gadget is unavailable.\n"
            "[gadget] Check libcomposite/configfs and dtoverlay=dwc2.\n");
        return false;
    }

    // Always remove our previous gadget object before creating a new one when
    // force=true.  This protects normal startup from stale configfs state left
    // by crashes, kill -9, power loss, or older versions of setup_gadget.sh.
    if (path_exists(GADGET_DIR)) {
        if (force || !hidg_nodes_ready()) {
            teardown_gadget();
            std::this_thread::sleep_for(ms(300));
        } else {
            return true;
        }
    }

    if (!mkdir_if_needed(GADGET_DIR)) return false;

    std::string strings_dir = join_path(GADGET_DIR, "strings/0x409");
    std::string configs_dir = join_path(GADGET_DIR, "configs/c.1");
    std::string config_strings_dir = join_path(configs_dir, "strings/0x409");
    std::string functions_dir = join_path(GADGET_DIR, "functions");

    if (!mkdir_if_needed(join_path(GADGET_DIR, "strings").c_str())) return false;
    if (!mkdir_if_needed(strings_dir.c_str())) return false;
    if (!mkdir_if_needed(join_path(GADGET_DIR, "configs").c_str())) return false;
    if (!mkdir_if_needed(configs_dir.c_str())) return false;
    // Real Pro Controller descriptor has iConfiguration = 0, so do not create
    // a configuration string in configs/c.1/strings.
    if (!mkdir_if_needed(functions_dir.c_str())) return false;

    if (!write_text_file(join_path(GADGET_DIR, "bcdDevice").c_str(), "0x0400")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bcdUSB").c_str(), "0x0200")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "idVendor").c_str(), "0x057e")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "idProduct").c_str(), "0x2009")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceClass").c_str(), "0x00")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceSubClass").c_str(), "0x00")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceProtocol").c_str(), "0x00")) return false;

    // USB descriptor serial belongs here, not in the controller SPI area.
    if (!write_text_file(join_path(strings_dir, "serialnumber").c_str(), g_usb_serial.c_str())) return false;
    if (!write_text_file(join_path(strings_dir, "manufacturer").c_str(), "Nintendo Co., Ltd.")) return false;
    if (!write_text_file(join_path(strings_dir, "product").c_str(), "Pro Controller")) return false;

    if (!write_text_file(join_path(configs_dir, "MaxPower").c_str(), "500")) return false;
    if (!write_text_file(join_path(configs_dir, "bmAttributes").c_str(), "0xA0")) return false;

    for (int i = 0; i < g_hid_port_count; ++i) {
        if (!create_hid_function(i)) return false;
    }

    std::string udc = first_udc_name();
    if (udc.empty()) {
        std::fprintf(stderr,
            "[gadget] No UDC found. Check dtoverlay=dwc2 in /boot/config.txt.\n");
        return false;
    }

    if (!write_text_file(join_path(GADGET_DIR, "UDC").c_str(), udc.c_str())) return false;
    std::printf("[gadget] Bound to UDC: %s\n", udc.c_str());

    // /dev/hidg* can appear shortly after binding.
    for (int tries = 0; tries < 20; ++tries) {
        bool all_seen = true;
        for (int i = 0; i < g_hid_port_count; ++i) {
            char path[32];
            std::snprintf(path, sizeof(path), "/dev/hidg%d", i);
            if (access(path, F_OK) != 0) all_seen = false;
            chmod(path, 0666);
        }
        if (all_seen && hidg_nodes_ready()) {
            std::printf("[gadget] Done. Exposed %d Pro Controller HID interface(s) (/dev/hidg0..%d)\n", g_hid_port_count, g_hid_port_count - 1);
            return true;
        }
        std::this_thread::sleep_for(ms(100));
    }

    std::fprintf(stderr, "[gadget] setup finished, but requested /dev/hidg* nodes are still not ready.\n");
    return false;
}

static bool run_gadget_setup_if_needed(bool force, const char* reason) {
    return setup_gadget_builtin(force, reason);
}

static void drain_hid_output_queue(int fd) {
    if (fd < 0) return;

    uint8_t discard[PRO_REPORT_SIZE];
    for (int i = 0; i < 32; ++i) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) break;
        ssize_t r = read(fd, discard, sizeof(discard));
        if (r <= 0) break;
    }
}

// ── Smart Multiplexer HID Writer Thread ───────────────────────────────────────
static void writer_thread(int hz) {
    for (int i = 0; i < g_hid_port_count; ++i) init_spi_flash(i);

    const auto tick = us(1'000'000 / hz);
    int fds[4] = {-1, -1, -1, -1};
    std::string devs[4] = {"/dev/hidg0", "/dev/hidg1", "/dev/hidg2", "/dev/hidg3"};
    bool was_connected = false;

    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[4];
    ControllerRuntime rt[4];
    for (int i = 0; i < g_hid_port_count; ++i) rt[i].ctrl = i;

    while (g_running.load(std::memory_order_relaxed)) {
        bool all_open = true;
        for (int i = 0; i < g_hid_port_count; ++i) {
            if (fds[i] < 0) {
                fds[i] = open(devs[i].c_str(), O_RDWR);
                if (fds[i] >= 0) {
                    rt[i].fd = fds[i];
                    rt[i].timer = 0;
                    rt[i].full_report_enabled = false;
                    rt[i].usb_seen_mac = false;
                    rt[i].usb_handshake_done = false;
                    rt[i].usb_baudrate_set = false;
                    rt[i].usb_timeout_disabled = false;
                    rt[i].pending_subcmd_reply = false;
                    rt[i].last_standard_report_us = 0;
                    rt[i].last_idle_neutral_us = 0;
                    rt[i].neutral_burst_until_us = 0;
                    memset(&rt[i].pending_reply, 0, sizeof(rt[i].pending_reply));
                } else {
                    all_open = false;
                }
            }
        }

        if (!all_open) {
            // Do not keep a partial set of opened endpoints around while the
            // gadget is being recreated/rebound.  Retry with a clean fd set.
            for (int i = 0; i < g_hid_port_count; ++i) {
                if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; rt[i].fd = -1; }
            }
            run_gadget_setup_if_needed(false, "requested /dev/hidg* nodes could not all be opened");
            std::this_thread::sleep_for(ms(500));
            continue;
        }

        if (g_verbose || !was_connected)
            std::printf("%dx Pro Controller /dev/hidg* opened\n", g_hid_port_count);
        was_connected = true;

        auto next = Clock::now() + tick;
        bool error_shown = false;
        bool timeout_printed[MAX_CLIENTS] = {};
        uint64_t writes_this_second = 0;
        auto last_rate_log = Clock::now();

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            uint64_t now_stamp = now_us();
            bool active_snap[MAX_CLIENTS] = {};
            bool uses_presence_snap[MAX_CLIENTS] = {};
            bool present_snap[MAX_CLIENTS][4] = {};
            uint64_t last_present_snap[MAX_CLIENTS][4] = {};
            ExtendedHIDReport report_snap[MAX_CLIENTS][4];
            MotionReport motion_snap[MAX_CLIENTS][4][3];
            uint8_t motion_count_snap[MAX_CLIENTS][4] = {};
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                for (int s = 0; s < 4; ++s) {
                    report_snap[c][s].reset();
                    for (int i = 0; i < 3; ++i) motion_snap[c][s][i].reset();
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                std::lock_guard<std::mutex> lk(g_mtx[c]);
                repair_future_client_timestamp(g_clients[c], now_stamp);
                const uint64_t client_idle_us = elapsed_us_saturated(now_stamp, g_clients[c].last_rx_us);
                if (g_clients[c].active && g_clients[c].last_rx_us != 0 && client_idle_us > g_client_timeout_us && !server_macro_running(c,0) && !server_macro_running(c,1) && !server_macro_running(c,2) && !server_macro_running(c,3)) {
                    g_clients[c].active = false;
                    g_clients[c].report.reset();
                    clear_all_motion_history(g_clients[c]);
                    g_clients[c].uses_pad_presence = false;
                    for (int s = 0; s < 4; ++s) {
                        g_clients[c].pad_present[s] = false;
                        g_clients[c].pad_last_present_us[s] = 0;
                        g_clients[c].debug_rumble_sent[s] = false;
                    }
                    if (g_verbose && !timeout_printed[c]) {
                        std::printf("PC %d timed out after %.1fs without UDP input and was disconnected.\n",
                                    c + 1, (double)elapsed_us_saturated(now_stamp, g_clients[c].last_rx_us) / 1000000.0);
                        timeout_printed[c] = true;
                    }
                } else if (g_clients[c].active) {
                    timeout_printed[c] = false;
                }
                active_snap[c] = g_clients[c].active;
                uses_presence_snap[c] = g_clients[c].uses_pad_presence;
                const bool input_stream_stale =
                    g_clients[c].active &&
                    g_clients[c].last_rx_us != 0 &&
                    client_idle_us > CLIENT_STALE_NEUTRAL_US;

                for (int s = 0; s < 4; ++s) {
                    present_snap[c][s] = g_clients[c].pad_present[s];
                    last_present_snap[c][s] = g_clients[c].pad_last_present_us[s];
                }

                if (input_stream_stale) {
                    // Keep the session/port alive through a short Windows UDP stall,
                    // but release all controls quickly so held R/ZR/sticks do not get
                    // stuck.  A real disconnect is handled later by g_client_timeout_us.
                    report_snap[c][0].reset();
                    report_snap[c][1].reset();
                    report_snap[c][2].reset();
                    report_snap[c][3].reset();
                    for (int s = 0; s < 4; ++s)
                        motion_count_snap[c][s] = 0;
                } else {
                    report_snap[c][0] = g_clients[c].report.p1;
                    report_snap[c][1] = g_clients[c].report.p2;
                    report_snap[c][2] = g_clients[c].report.p3;
                    report_snap[c][3] = g_clients[c].report.p4;
                    for (int s = 0; s < 4; ++s) {
                        motion_count_snap[c][s] = g_clients[c].motion_history_count[s];
                        for (int i = 0; i < 3; ++i)
                            motion_snap[c][s][i] = g_clients[c].motion_history[s][i];
                    }
                }
            }

            for (int h = 0; h < g_hid_port_count; ++h) {
                if (hw_slots[h].client_idx == -1) continue;

                int cidx = hw_slots[h].client_idx;
                int sidx = hw_slots[h].sub_idx;
                bool absent_too_long = false;
                if (uses_presence_snap[cidx] && !present_snap[cidx][sidx]) {
                    uint64_t last_seen = last_present_snap[cidx][sidx];
                    absent_too_long = (last_seen == 0) ||
                                      (now_stamp - last_seen >= WEB_PAD_ABSENT_RELEASE_US);
                }

                if ((!active_snap[cidx] || absent_too_long) && !server_macro_running(cidx, sidx)) {
                    hw_slots[h].client_idx = -1;
                    hw_slots[h].sub_idx = -1;

                    // The USB interface stays alive so the Switch can keep talking to
                    // it, but the player assignment is gone.  Send a short neutral
                    // release burst to clear any held buttons, drain stale output, then
                    // fall back to the low-rate idle heartbeat.
                    rt[h].neutral_burst_until_us = now_stamp + PRO_RELEASE_NEUTRAL_US;
                    drain_hid_output_queue(fds[h]);
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!active_snap[c]) continue;
                for (int s = 0; s < 4; ++s) {
                    bool mapped = false;
                    for (int h = 0; h < g_hid_port_count; ++h) {
                        if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                            mapped = true;
                            break;
                        }
                    }

                    // A browser/mobile pad that is connected but currently neutral still
                    // needs to claim its Switch port so rumble has a target immediately.
                    if (mapped) continue;
                    bool macro_active_for_pad = server_macro_running(c, s);
                    if (uses_presence_snap[c]) {
                        if (!present_snap[c][s] && !macro_active_for_pad) continue;
                    } else if (extended_is_neutral(report_snap[c][s]) && !macro_active_for_pad) {
                        continue;
                    }

                    // Preserve logical pad order.  The previous "first free port" mapper
                    // let Pad 2 steal Switch Port 1 whenever keyboard/mobile Pad 1 was
                    // neutral, which made keyboard mode and mobile mode look broken.
                    int chosen = -1;
                    if (s >= 0 && s < g_hid_port_count && hw_slots[s].client_idx == -1) {
                        chosen = s;
                    } else {
                        // Fallback only for multi-client cases where the preferred port
                        // is already occupied.
                        for (int h = 0; h < g_hid_port_count; ++h) {
                            if (hw_slots[h].client_idx == -1) {
                                chosen = h;
                                break;
                            }
                        }
                    }

                    if (chosen != -1) {
                        hw_slots[chosen].client_idx = c;
                        hw_slots[chosen].sub_idx = s;
                        if (g_verbose)
                            std::printf("Map -> PC %d (Pad %d) took Switch Pro Port %d\n", c + 1, s + 1, chosen + 1);
                    }
                }
            }

            ExtendedHIDReport out_reports[4];
            for (int h = 0; h < g_hid_port_count; ++h) out_reports[h].reset();
            for (int h = 0; h < g_hid_port_count; ++h) {
                if (hw_slots[h].client_idx != -1) {
                    out_reports[h] = report_snap[hw_slots[h].client_idx][hw_slots[h].sub_idx];
                    server_macro_apply(hw_slots[h].client_idx, hw_slots[h].sub_idx, out_reports[h].input);
                }
            }

            bool ok = true;
            for (int h = 0; h < g_hid_port_count; ++h) {
                const bool port_needed = (hw_slots[h].client_idx != -1);

                uint8_t write_buf[PRO_REPORT_SIZE] = {};
                bool have_report_to_write = false;
                bool wrote_subcmd_reply = false;

                if (rt[h].pending_subcmd_reply) {
                    rt[h].pending_reply.id = RID_INPUT_SUBCMD;
                    rt[h].pending_reply.timer = pro_timer_from_us(now_stamp);
                    if (port_needed)
                        apply_input_controls_to_pro21(out_reports[h], rt[h].pending_reply);
                    else
                        fill_neutral_controls(rt[h].pending_reply);
                    memcpy(write_buf, &rt[h].pending_reply, sizeof(ProInputReport21));
                    have_report_to_write = true;
                    wrote_subcmd_reply = true;
                } else if (rt[h].full_report_enabled) {
                    // Active/player-assigned ports run at the normal writer rate
                    // (250 Hz).  Unassigned ports still send neutral keepalive reports,
                    // but only at a low heartbeat rate so we do not spam neutral data.
                    bool release_burst = rt[h].neutral_burst_until_us != 0 &&
                                         now_stamp < rt[h].neutral_burst_until_us;
                    if (rt[h].neutral_burst_until_us != 0 && now_stamp >= rt[h].neutral_burst_until_us)
                        rt[h].neutral_burst_until_us = 0;

                    bool idle_due = (rt[h].last_idle_neutral_us == 0) ||
                                    (elapsed_us_saturated(now_stamp, rt[h].last_idle_neutral_us) >= PRO_IDLE_REPORT_INTERVAL_US);
                    bool standard_due = (rt[h].last_standard_report_us == 0) ||
                                        (elapsed_us_saturated(now_stamp, rt[h].last_standard_report_us) >= g_pro_report_interval_us);

                    // A real Pro Controller over USB emits full 0x30 input
                    // reports roughly every 15ms.  Sending 0x30 at 125-250Hz
                    // makes the 3 IMU samples/timer cadence unrealistic and
                    // some Switch software appears to ignore motion even though
                    // button/stick input still works.
                    bool should_write_standard = false;
                    if (port_needed || release_burst) should_write_standard = standard_due;
                    else should_write_standard = idle_due;

                    if (should_write_standard) {
                        ExtendedHIDReport report_for_port;
                        report_for_port.reset();
                        if (port_needed) report_for_port = out_reports[h];

                        const MotionReport* history_for_port = nullptr;
                        uint8_t history_count_for_port = 0;
                        if (port_needed) {
                            int cidx = hw_slots[h].client_idx;
                            int sidx = hw_slots[h].sub_idx;
                            history_for_port = motion_snap[cidx][sidx];
                            history_count_for_port = motion_count_snap[cidx][sidx];
                        }

                        ProInputReport30 std_in{};
                        build_standard_report(report_for_port,
                                              history_for_port,
                                              history_count_for_port,
                                              rt[h].imu_enabled,
                                              pro_timer_from_us(now_stamp),
                                              std_in);
                        memcpy(write_buf, &std_in, sizeof(ProInputReport30));
                        if (g_verbose && g_log_raw_30) {
                            static uint64_t last_raw30_log_us = 0;
                            if (last_raw30_log_us == 0 || elapsed_us_saturated(now_stamp, last_raw30_log_us) > 250000ULL) {
                                last_raw30_log_us = now_stamp;
                                std::printf("[raw30] ");
                                for (int bi = 0; bi < PRO_REPORT_SIZE; ++bi)
                                    std::printf("%02X%s", write_buf[bi], bi + 1 < PRO_REPORT_SIZE ? " " : "");
                                std::printf("\n");
                            }
                        }
                        have_report_to_write = true;

                        if (port_needed || release_burst)
                            rt[h].last_standard_report_us = now_stamp;
                        if (!port_needed && !release_burst)
                            rt[h].last_idle_neutral_us = now_stamp;
                    }
                }

                if (!have_report_to_write) continue;

                ssize_t w = write(fds[h], write_buf, PRO_REPORT_SIZE);
                if (w < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
                    // If a subcommand reply could not be written, keep it pending.
                    // Dropping it makes the Switch repeat commands such as 0x02 forever.
                } else if (w == (ssize_t)PRO_REPORT_SIZE) {
                    if (wrote_subcmd_reply) rt[h].pending_subcmd_reply = false;
                    writes_this_second++;
                } else if (w > 0) {
                    // Partial HID report writes should not happen.  Treat as an error so
                    // we reconnect cleanly rather than sending malformed controller data.
                    ok = false;
                }
            }

            for (int h = 0; h < g_hid_port_count; ++h) {
                // Always serve the HID control/output side for every exposed Pro
                // Controller interface.  HID gadgets are not lazily created: once
                // setup_gadget.sh exposes hidg0..hidg3, the Switch may send init
                // commands to any of them.  Ignoring those commands until a pad maps
                // to the port breaks keyboard/mobile/web input and leaves stale output
                // reports queued in /dev/hidgX.
                for (int output_reads = 0; output_reads < 8; ++output_reads) {
                    struct pollfd pfd = {fds[h], POLLIN, 0};
                    uint8_t read_buf[PRO_REPORT_SIZE];
                    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
                        break;

                    ssize_t r = read(fds[h], read_buf, PRO_REPORT_SIZE);
                    if (r <= 0) continue;

                    uint8_t id = read_buf[0];
                    if (id == RID_OUTPUT_CMD) {
                        if (hw_slots[h].client_idx != -1)
                            publish_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, false);

                        uint8_t subcmd_id = read_buf[10];
                        size_t subcmd_data_len = r > 11 ? std::min((size_t)53, (size_t)(r - 11)) : 0;
                        memset(&rt[h].pending_reply, 0, sizeof(rt[h].pending_reply));
                        int reply_len = handle_subcommand(
                            rt[h], subcmd_id,
                            subcmd_data_len > 0 ? read_buf + 11 : nullptr,
                            subcmd_data_len,
                            &rt[h].pending_reply
                        );
                        rt[h].pending_subcmd_reply = (reply_len >= 0);
                        if (g_verbose) {
                            std::printf("[pro%d] subcmd 0x%02X reply=0x%02X 0x%02X\n",
                                        h + 1, subcmd_id, rt[h].pending_reply.ack, rt[h].pending_reply.subcmd_id);
                        }
                    } else if (id == RID_OUTPUT_RUMBLE) {
                        if (hw_slots[h].client_idx != -1)
                            publish_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, true);
                    } else if (id == 0x80) {
                        const uint8_t usb_cmd = read_buf[1];
                        uint8_t resp_81[PRO_REPORT_SIZE] = {};
                        build_usb_81_response(resp_81, usb_cmd, h);

                        switch (usb_cmd) {
                        case 0x01: rt[h].usb_seen_mac = true; break;
                        case 0x02: rt[h].usb_handshake_done = true; break;
                        case 0x03: rt[h].usb_baudrate_set = true; break;
                        case 0x04: rt[h].usb_timeout_disabled = true; break;
                        case 0x05: rt[h].usb_timeout_disabled = false; break;
                        default: break;
                        }

                        ssize_t w = write(fds[h], resp_81, PRO_REPORT_SIZE);
                        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
                        if (g_verbose) {
                            std::printf("[pro%d] usb 0x80 cmd=0x%02X -> 0x81 subtype=0x%02X mac=%02X:%02X:%02X:%02X:%02X:%02X timeout=%s\n",
                                        h + 1, usb_cmd, resp_81[1],
                                        CTRL_MAC_BE[h][0], CTRL_MAC_BE[h][1], CTRL_MAC_BE[h][2],
                                        CTRL_MAC_BE[h][3], CTRL_MAC_BE[h][4], CTRL_MAC_BE[h][5],
                                        rt[h].usb_timeout_disabled ? "disabled" : "enabled");
                        }
                    } else {
                        if (g_verbose && id != 0x00)
                            std::printf("[pro%d] unknown output report id=0x%02X len=%zd\n", h + 1, id, r);
                    }
                }
            }

            if (!ok) {
                if (!error_shown) { std::puts("Switch disconnected — waiting for reconnect..."); error_shown = true; }
                for (int i = 0; i < 4; ++i) { close(fds[i]); fds[i] = -1; rt[i].fd = -1; }
                std::this_thread::sleep_for(ms(1000));
                break;
            }

            auto now_log = Clock::now();
            if (g_verbose && now_log - last_rate_log >= ms(1000)) {
                std::printf("pro_hid_writes/sec=%llu\n", (unsigned long long)writes_this_second);
                writes_this_second = 0;
                last_rate_log = now_log;
            }
            if (writes_this_second) ++g_hid_writes;
        }
    }

    for (int i = 0; i < 4; ++i) {
        if (fds[i] >= 0) close(fds[i]);
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
static std::mutex g_rate_mtx;

// ── Stats thread ──────────────────────────────────────────────────────────────
static void stats_thread() {
    uint64_t last_cleanup = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(ms(5000));

        // Periodic rate limiter cleanup (every 60s)
        uint64_t now = now_us();
        if (now - last_cleanup > 60000000) {
            last_cleanup = now;
            std::lock_guard<std::mutex> lk(g_rate_mtx);
            for (int i = 0; i < RATE_TABLE; ++i) {
                if (g_rate_table[i].ip != 0 &&
                    now - g_rate_table[i].window_start > RATE_WINDOW_US * 2)
                    g_rate_table[i].ip = 0;
            }
        }

        if (!g_verbose) continue;
        std::printf("pkts_rx=%-8llu  hid_writes=%-8llu\n",
            (unsigned long long)g_pkts_rx.load(),
            (unsigned long long)g_hid_writes.load());
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
static bool rate_allow(uint32_t ip) {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    uint64_t now = now_us();
    uint32_t idx = ip % RATE_TABLE;
    RateSlot &s = g_rate_table[idx];
    if (s.ip != ip) {
        s.ip = ip; s.count = 1; s.window_start = now; return true;
    }
    if (now - s.window_start > RATE_WINDOW_US) {
        s.count = 1; s.window_start = now; return true;
    }
    if (s.count < UINT32_MAX) s.count++;
    return s.count <= RATE_MAX_PKT;
}


#ifdef USE_UPNP
// ── UPnP port forwarding ──
static bool     g_upnp_active = false;
static uint16_t g_upnp_port   = 0;
static UPNPUrls g_upnp_urls{};
static IGDdatas g_upnp_data{};
static char     g_upnp_lan_addr[64]{};

static bool upnp_add_mapping(uint16_t port) {
    if (g_upnp_active) return false; 

    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
    if (!devlist) return false;
    
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
    freeUPNPDevlist(devlist);
    
    if (igd != 1 && igd != 2) { FreeUPNPUrls(&g_upnp_urls); return false; }
    
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    
    int r = UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype,
                                port_str, port_str, g_upnp_lan_addr, "ns-backend", "UDP", nullptr, "0");
    if (r != 0) { FreeUPNPUrls(&g_upnp_urls); return false; }
    
    g_upnp_active = true;
    g_upnp_port = port;
    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, external_ip) == 0) {
        std::printf("UPnP: UDP port %u successfully forwarded!\n", port);
        std::printf("UPnP: Tell your clients to connect to -> %s:%u\n", external_ip, port);
    }
    return true;
}

static void upnp_remove_mapping(uint16_t) {
    if (!g_upnp_active) return;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", g_upnp_port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str, "UDP", nullptr);
    std::puts("UPnP: port mapping removed cleanly");
    FreeUPNPUrls(&g_upnp_urls); g_upnp_active = false; g_upnp_port = 0;
}
#else
static bool upnp_add_mapping(uint16_t) { return false; }
static void upnp_remove_mapping(uint16_t) {}
#endif


// ══════════════════════════════════════════════════════════════════════════════
// ── Embedded Web Server (HTTP + WebSocket proxy, enabled with -w) ────────────
// ══════════════════════════════════════════════════════════════════════════════

static const char INDEX_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover\">\n"
    "    <title>NS Web Control</title>\n"
    "    <style>\n"
    "        body {\n"
    "            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;\n"
    "            background-color: #FFFFFF;\n"
    "            color: #1A1A1A;\n"
    "            padding: 20px;\n"
    "            max-width: 480px;\n"
    "        }\n"
    "        h2 { color: #CC0000; margin-top: 0; font-size: 20px; }\n"
    "        .row { display: flex; align-items: center; margin-bottom: 12px; }\n"
    "        .row label { width: 140px; text-align: right; margin-right: 10px; font-size: 14px; }\n"
    "        input[type=\"text\"], select {\n"
    "            flex: 1; padding: 4px; font-family: 'Consolas', monospace; border: 1px solid #ccc;\n"
    "        }\n"
    "        button {\n"
    "            padding: 6px 16px; font-family: 'Segoe UI'; font-size: 14px;\n"
    "            background: #f0f0f0; border: 1px solid #ccc; cursor: pointer;\n"
    "            border-radius: 4px;\n"
    "            min-height: 30px;\n"
    "        }\n"
    "        button:hover { background: #e0e0e0; }\n"
    "        .btn-group { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 15px; }\n"
    "        hr { border: none; border-top: 2px solid #DDDDDD; margin: 20px 0; }\n"
    "        .status { font-size: 13px; margin-bottom: 4px; }\n"
    "        \n"
    "        #modalOverlay {\n"
    "            display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%;\n"
    "            background: rgba(0,0,0,0.5); justify-content: center; align-items: center;\n"
    "            z-index: 10000;\n"
    "        }\n"
    "        #modalContent {\n"
    "            background: white; padding: 20px; border-radius: 8px; width: 90%; max-width: 440px;\n"
    "            max-height: 80vh; display: flex; flex-direction: column; box-shadow: 0 4px 12px rgba(0,0,0,0.2);\n"
    "        }\n"
    "        #modalContent h3 { margin-top: 0; margin-bottom: 15px; color: #CC0000; flex-shrink: 0; }\n"
    "        \n"
    "        #bindingsScrollArea {\n"
    "            overflow-y: auto; flex-grow: 1; padding-right: 10px;\n"
    "        }\n"
    "        \n"
    "        .bind-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; font-size: 13px; }\n"
    "        .bind-btn { width: 120px; font-family: 'Consolas'; font-size: 12px; transition: background 0.2s; }\n"
    "        .listening { background: #ffeb3b !important; border-color: #fbc02d !important; color: #000; transform: scale(1.02); }\n"
    "        \n"
    "        .modal-footer {\n"
    "            display: flex; justify-content: space-between; margin-top: 15px; padding-top: 15px; border-top: 1px solid #eee; flex-shrink: 0;\n"
    "        }\n"
    "        .footer-group { display: flex; gap: 8px; }\n"
    "        \n"
    "        @media (max-width: 480px) {\n"
    "            .row { flex-direction: column; align-items: flex-start; }\n"
    "            .row label { width: auto; text-align: left; margin-bottom: 5px; }\n"
    "            select, button { padding: 10px; min-height: 44px; }\n"
    "        }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "    <h2>NS Web Control</h2>\n"
    "    <div class=\"row\" id=\"kbModeContainer\">\n"
    "        <label>Keyboard Mode:</label>\n"
    "        <select id=\"kbMode\">\n"
    "            <option value=\"0\">OFF</option>\n"
    "            <option value=\"1\">ON (single)</option>\n"
    "            <option value=\"2\">ON (override)</option>\n"
    "        </select>\n"
    "    </div>\n"
    "    <div class=\"btn-group\">\n"
    "        <button id=\"btnConnect\">Connect</button>\n"
    "        <button id=\"btnBindings\">Bindings...</button>\n"
    "        <button id=\"btnMacros\">Macros...</button>\n"
    "        <button id=\"btnTouchControls\" style=\"display: none; background: #e3f2fd; border-color: #90caf9;\" onclick=\"window.location.href='/mobile'\">Touch Controls</button>\n"
    "        <button id=\"btnEditor\" style=\"display: none; background: #fff9c4; border-color: #ffe082;\" onclick=\"window.location.href='/editor'\">Editor</button>\n"
    "    </div>\n"
    "    <hr>\n"
    "    <div class=\"status\" id=\"statusText\">Ready</div>\n"
    "    <div class=\"status\" id=\"p1Text\">P1: Idle</div>\n"
    "    <div class=\"status\" id=\"p2Text\">P2: Not connected</div>\n"
    "    <div class=\"status\" id=\"p3Text\">P3: Not connected</div>\n"
    "    <div class=\"status\" id=\"p4Text\">P4: Not connected</div>\n"
    "    <div id=\"modalOverlay\">\n"
    "        <div id=\"modalContent\">\n"
    "            <h3>Keyboard Bindings</h3>\n"
    "            <div id=\"bindingsScrollArea\">\n"
    "                <div id=\"bindingsList\"></div>\n"
    "            </div>\n"
    "            <div class=\"modal-footer\">\n"
    "                <div class=\"footer-group\">\n"
    "                    <button id=\"btnSaveBindings\" style=\"background: #e3f2fd; border-color: #90caf9;\">Save</button>\n"
    "                    <button id=\"btnCancelBindings\">Cancel</button>\n"
    "                </div>\n"
    "                <div class=\"footer-group\">\n"
    "                    <button id=\"btnSetupBindings\">Setup Wizard</button>\n"
    "                    <button id=\"btnResetBindings\">Reset</button>\n"
    "                </div>\n"
    "            </div>\n"
    "        </div>\n"
    "    </div>\n"
    "    <div id=\"macroOverlay\" style=\"display:none; position:fixed; inset:0; background:rgba(0,0,0,0.5); justify-content:center; align-items:center; z-index:10001;\">\n"
    "        <div style=\"background:white; color:#1A1A1A; padding:18px; border-radius:8px; width:90%; max-width:520px; box-shadow:0 4px 12px rgba(0,0,0,0.25);\">\n"
    "            <h3 style=\"margin-top:0; color:#CC0000;\">Macros</h3>\n"
    "            <div id=\"macroRows\" style=\"min-height:28px; margin-bottom:12px;\"></div>\n"
    "            <input id=\"macroImportFile\" type=\"file\" accept=\"application/json,.json\" style=\"display:none\">\n"
    "            <div style=\"display:grid; grid-template-columns:88px 1fr 88px; gap:8px; margin-top:10px;\">\n"
    "                <button id=\"btnMacroImport\">Import</button><button id=\"btnMacroRecord\" style=\"grid-column:3;\">Record P1</button>\n"
    "                <button id=\"btnMacroExport\">Export</button><button id=\"btnMacroClose\" style=\"grid-column:3;\">Close</button>\n"
    "            </div>\n"
    "        </div>\n"
    "    </div>\n"
    "<script>\n"
    "const PROTO_MAGIC = 0x4E535743;\n"
    "const PROTO_VERSION = 5;\n"
    "const RUMBLE_MAGIC = 0x4E535652;\n"
    "const PAD_PRESENT = 1;\n"
    "const SECRET = \"nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY\";\n"
    "const EXT_REPORT_SIZE = 24;\n"
    "const PACKET_SIZE = 116;\n"
    "const PACKET_AUTH_SIZE = 52;\n"
    "const BTN_Y = 1<<0, BTN_B = 1<<1, BTN_A = 1<<2, BTN_X = 1<<3;\n"
    "const BTN_L = 1<<4, BTN_R = 1<<5, BTN_ZL = 1<<6, BTN_ZR = 1<<7;\n"
    "const BTN_MINUS = 1<<8, BTN_PLUS = 1<<9, BTN_LSTICK = 1<<10, BTN_RSTICK = 1<<11;\n"
    "const BTN_HOME = 1<<12, BTN_CAPTURE = 1<<13;\n"
    "const HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3, HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8;\n"
    "let ws = null;\n"
    "let isConnected = false;\n"
    "let loopId = null;\n"
    "let seqCounter = 0;\n"
    "let lastActivePads = [];\n"
    "let rumbleTargets = [null, null, null, null];\n"
    "let rumbleTargetIndexes = [-1, -1, -1, -1];\n"
    "let rumbleDebounceTime = [0, 0, 0, 0];\n"
    "let rumbleLastMag = ['', '', '', ''];\n"
    "let motion = { enabled:false, ax:0, ay:0, az:0, gx:0, gy:0, gz:0 };\n"
    "let motionListenerInstalled = false, orientationListenerInstalled = false, lastDeviceMotionMs = 0;\n"
    "const keysDown = new Set();\n"
    "const defaultBindings = {\n"
    "    'BTN_Y': 'KeyZ', 'BTN_B': 'KeyX', 'BTN_A': 'KeyV', 'BTN_X': 'KeyC',\n"
    "    'BTN_L': 'KeyQ', 'BTN_R': 'KeyE', 'BTN_ZL': 'Digit1', 'BTN_ZR': 'Digit2',\n"
    "    'BTN_MINUS': 'Digit3', 'BTN_PLUS': 'Digit4',\n"
    "    'BTN_LSTICK': 'ShiftLeft', 'BTN_RSTICK': 'ShiftRight',\n"
    "    'BTN_HOME': 'Home', 'BTN_CAPTURE': 'PrintScreen',\n"
    "    'DPAD_UP': 'ArrowUp', 'DPAD_DOWN': 'ArrowDown', 'DPAD_LEFT': 'ArrowLeft', 'DPAD_RIGHT': 'ArrowRight',\n"
    "    'LSTICK_UP': 'KeyW', 'LSTICK_DOWN': 'KeyS', 'LSTICK_LEFT': 'KeyA', 'LSTICK_RIGHT': 'KeyD',\n"
    "    'RSTICK_UP': 'KeyI', 'RSTICK_DOWN': 'KeyK', 'RSTICK_LEFT': 'KeyJ', 'RSTICK_RIGHT': 'KeyL'\n"
    "};\n"
    "let currentBindings = { ...defaultBindings };\n"
    "let preEditBindings = {};\n"
    "window.onload = () => {\n"
    "    const isMobile = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent);\n"
    "    if (isMobile) {\n"
    "        document.getElementById('kbModeContainer').style.display = 'none';\n"
    "        document.getElementById('btnBindings').style.display = 'none';\n"
    "        document.getElementById('btnMacros').style.display = 'none';\n"
    "        document.getElementById('btnTouchControls').style.display = 'inline-block';\n"
    "        document.getElementById('btnEditor').style.display = 'inline-block';\n"
    "    }\n"
    "    const savedMode = localStorage.getItem('nswc_mode');\n"
    "    if (savedMode) document.getElementById('kbMode').value = savedMode;\n"
    "    const savedBindings = localStorage.getItem('nswc_bindings');\n"
    "    if (savedBindings) currentBindings = JSON.parse(savedBindings);\n"
    "    wireMacroMenu();\n"
    "};\n"
    "document.getElementById('kbMode').onchange = (e) => localStorage.setItem('nswc_mode', e.target.value);\n"
    "window.addEventListener('keydown', (e) => {\n"
    "    if (activeBindKey) { e.preventDefault(); remapKey(e.code); return; }\n"
    "    const m = savedMacros.find(x => normalizeMacroKey(x.hotkey) && normalizeMacroKey(x.hotkey) === normalizeMacroKey(e.code));\n"
    "    if (m && !macroHotkeyConflicts(e.code, savedMacros.indexOf(m))) { e.preventDefault(); runMacroServerSide(m); return; }\n"
    "    if (!['INPUT','TEXTAREA','SELECT','BUTTON'].includes((e.target && e.target.tagName) || '')) e.preventDefault();\n"
    "    keysDown.add(e.code);\n"
    "});\n"
    "window.addEventListener('keyup', (e) => { e.preventDefault(); keysDown.delete(e.code); });\n"
    "function getNeutralState() { return { buttons: 0, hat: HAT_NEUTRAL, lx: 128, ly: 128, rx: 128, ry: 128 }; }\n"
    "function getKeyboardState() {\n"
    "    let buttons = 0, hat = HAT_NEUTRAL, lx = 128, ly = 128, rx = 128, ry = 128;\n"
    "    if (keysDown.has(currentBindings['BTN_Y'])) buttons |= BTN_Y;\n"
    "    if (keysDown.has(currentBindings['BTN_B'])) buttons |= BTN_B;\n"
    "    if (keysDown.has(currentBindings['BTN_A'])) buttons |= BTN_A;\n"
    "    if (keysDown.has(currentBindings['BTN_X'])) buttons |= BTN_X;\n"
    "    if (keysDown.has(currentBindings['BTN_L'])) buttons |= BTN_L;\n"
    "    if (keysDown.has(currentBindings['BTN_R'])) buttons |= BTN_R;\n"
    "    if (keysDown.has(currentBindings['BTN_ZL'])) buttons |= BTN_ZL;\n"
    "    if (keysDown.has(currentBindings['BTN_ZR'])) buttons |= BTN_ZR;\n"
    "    if (keysDown.has(currentBindings['BTN_MINUS'])) buttons |= BTN_MINUS;\n"
    "    if (keysDown.has(currentBindings['BTN_PLUS'])) buttons |= BTN_PLUS;\n"
    "    if (keysDown.has(currentBindings['BTN_LSTICK'])) buttons |= BTN_LSTICK;\n"
    "    if (keysDown.has(currentBindings['BTN_RSTICK'])) buttons |= BTN_RSTICK;\n"
    "    if (keysDown.has(currentBindings['BTN_HOME'])) buttons |= BTN_HOME;\n"
    "    if (keysDown.has(currentBindings['BTN_CAPTURE'])) buttons |= BTN_CAPTURE;\n"
    "    const up = keysDown.has(currentBindings['DPAD_UP']), down = keysDown.has(currentBindings['DPAD_DOWN']);\n"
    "    const left = keysDown.has(currentBindings['DPAD_LEFT']), right = keysDown.has(currentBindings['DPAD_RIGHT']);\n"
    "    if (up && right) hat = HAT_NE; else if (up && left) hat = HAT_NW;\n"
    "    else if (down && right) hat = HAT_SE; else if (down && left) hat = HAT_SW;\n"
    "    else if (up) hat = HAT_N; else if (down) hat = HAT_S;\n"
    "    else if (left) hat = HAT_W; else if (right) hat = HAT_E;\n"
    "    if (keysDown.has(currentBindings['LSTICK_LEFT']) && !keysDown.has(currentBindings['LSTICK_RIGHT'])) lx = 0;\n"
    "    else if (keysDown.has(currentBindings['LSTICK_RIGHT']) && !keysDown.has(currentBindings['LSTICK_LEFT'])) lx = 255;\n"
    "    if (keysDown.has(currentBindings['LSTICK_UP']) && !keysDown.has(currentBindings['LSTICK_DOWN'])) ly = 0;\n"
    "    else if (keysDown.has(currentBindings['LSTICK_DOWN']) && !keysDown.has(currentBindings['LSTICK_UP'])) ly = 255;\n"
    "    if (keysDown.has(currentBindings['RSTICK_LEFT']) && !keysDown.has(currentBindings['RSTICK_RIGHT'])) rx = 0;\n"
    "    else if (keysDown.has(currentBindings['RSTICK_RIGHT']) && !keysDown.has(currentBindings['RSTICK_LEFT'])) rx = 255;\n"
    "    if (keysDown.has(currentBindings['RSTICK_UP']) && !keysDown.has(currentBindings['RSTICK_DOWN'])) ry = 0;\n"
    "    else if (keysDown.has(currentBindings['RSTICK_DOWN']) && !keysDown.has(currentBindings['RSTICK_UP'])) ry = 255;\n"
    "    return { buttons, hat, lx, ly, rx, ry };\n"
    "}\n"
    "function getGamepadState(pad) {\n"
    "    if (!pad) return null;\n"
    "    let buttons = 0, hat = HAT_NEUTRAL, lx = 128, ly = 128, rx = 128, ry = 128;\n"
    "    if (pad.buttons[0]?.pressed) buttons |= BTN_B;\n"
    "    if (pad.buttons[1]?.pressed) buttons |= BTN_A;\n"
    "    if (pad.buttons[2]?.pressed) buttons |= BTN_Y;\n"
    "    if (pad.buttons[3]?.pressed) buttons |= BTN_X;\n"
    "    if (pad.buttons[4]?.pressed) buttons |= BTN_L;\n"
    "    if (pad.buttons[5]?.pressed) buttons |= BTN_R;\n"
    "    if (pad.buttons[6]?.pressed) buttons |= BTN_ZL;\n"
    "    if (pad.buttons[7]?.pressed) buttons |= BTN_ZR;\n"
    "    if (pad.buttons[8]?.pressed) buttons |= BTN_MINUS;\n"
    "    if (pad.buttons[9]?.pressed) buttons |= BTN_PLUS;\n"
    "    if (pad.buttons[10]?.pressed) buttons |= BTN_LSTICK;\n"
    "    if (pad.buttons[11]?.pressed) buttons |= BTN_RSTICK;\n"
    "    if (pad.buttons[16]?.pressed) buttons |= BTN_HOME;\n"
    "    if (pad.buttons[17]?.pressed) buttons |= BTN_CAPTURE;\n"
    "    if ((buttons & BTN_LSTICK) && (buttons & BTN_RSTICK)) buttons |= BTN_HOME;\n"
    "    if ((buttons & BTN_MINUS) && (buttons & BTN_PLUS)) buttons |= BTN_CAPTURE;\n"
    "    const pup = pad.buttons[12]?.pressed, pdown = pad.buttons[13]?.pressed;\n"
    "    const pleft = pad.buttons[14]?.pressed, pright = pad.buttons[15]?.pressed;\n"
    "    if (pup && pright) hat = HAT_NE; else if (pup && pleft) hat = HAT_NW;\n"
    "    else if (pdown && pright) hat = HAT_SE; else if (pdown && pleft) hat = HAT_SW;\n"
    "    else if (pup) hat = HAT_N; else if (pdown) hat = HAT_S;\n"
    "    else if (pleft) hat = HAT_W; else if (pright) hat = HAT_E;\n"
    "    const applyDeadzone = (val) => { val = Number(val || 0); if (Math.abs(val) < 0.15) return 128; return Math.max(0, Math.min(255, Math.round(((val + 1) / 2) * 255))); };\n"
    "    if (pad.axes.length >= 2) {\n"
    "        lx = applyDeadzone(pad.axes[0]); ly = applyDeadzone(pad.axes[1]);\n"
    "    }\n"
    "    if (pad.axes.length >= 4) {\n"
    "        rx = applyDeadzone(pad.axes[2]); ry = applyDeadzone(pad.axes[3]);\n"
    "    }\n"
    "    return { buttons, hat, lx, ly, rx, ry };\n"
    "}\n"
    "function mergeStates(s1, s2) {\n"
    "    if (!s1) return s2; if (!s2) return s1;\n"
    "    return { buttons: s1.buttons | s2.buttons, hat: s1.hat !== HAT_NEUTRAL ? s1.hat : s2.hat,\n"
    "        lx: s1.lx !== 128 ? s1.lx : s2.lx, ly: s1.ly !== 128 ? s1.ly : s2.ly,\n"
    "        rx: s1.rx !== 128 ? s1.rx : s2.rx, ry: s1.ry !== 128 ? s1.ry : s2.ry };\n"
    "}\n"
    "function normalizeSystemShortcuts(buttons) {\n"
    "    const captureCombo = (buttons & BTN_MINUS) && (buttons & BTN_PLUS);\n"
    "    const homeCombo = (buttons & BTN_LSTICK) && (buttons & BTN_RSTICK);\n"
    "    // Capture (- +) and Home (LS RS) must be exclusive. Some browsers/controllers\n"
    "    // also expose vendor HOME/CAPTURE buttons while these combos are held, so\n"
    "    // resolve the combo once, right before packing the packet. Capture wins if\n"
    "    // both shortcut combos are held accidentally.\n"
    "    if (captureCombo) {\n"
    "        buttons |= BTN_CAPTURE;\n"
    "        buttons &= ~(BTN_MINUS | BTN_PLUS | BTN_HOME);\n"
    "        if (homeCombo) buttons &= ~(BTN_LSTICK | BTN_RSTICK);\n"
    "    } else if (homeCombo) {\n"
    "        buttons |= BTN_HOME;\n"
    "        buttons &= ~(BTN_LSTICK | BTN_RSTICK | BTN_CAPTURE);\n"
    "    }\n"
    "    return buttons;\n"
    "}\n"
    "function clamp01(v) { return Math.max(0.0, Math.min(1.0, v)); }\n"
    "function freshGamepadForSlot(subpadIndex, gamepads) {\n"
    "    // navigator.getGamepads() returns snapshots, so this function must only\n"
    "    // be called with a freshly-polled array from the current rumble event.\n"
    "    const storedIndex = rumbleTargetIndexes[subpadIndex];\n"
    "    if (storedIndex >= 0 && gamepads[storedIndex]) return gamepads[storedIndex];\n"
    "    if (gamepads[subpadIndex]) return gamepads[subpadIndex];\n"
    "    const bySlot = rumbleTargets[subpadIndex];\n"
    "    if (bySlot && typeof bySlot.index === 'number' && gamepads[bySlot.index]) return gamepads[bySlot.index];\n"
    "    return null;\n"
    "}\n"
    "async function rumbleOnePad(pad, weakFloat, strongFloat, durationMs) {\n"
    "    if (!pad || pad.connected === false || durationMs <= 0) return false;\n"
    "    if (pad.vibrationActuator && typeof pad.vibrationActuator.playEffect === 'function') {\n"
    "        try {\n"
    "            await pad.vibrationActuator.playEffect('dual-rumble', {\n"
    "                startDelay: 0,\n"
    "                duration: durationMs,\n"
    "                weakMagnitude: weakFloat,\n"
    "                strongMagnitude: strongFloat\n"
    "            });\n"
    "            return true;\n"
    "        } catch (err) {\n"
    "            console.error('Browser blocked rumble:', err, pad.id || pad.index);\n"
    "        }\n"
    "    }\n"
    "    if (pad.hapticActuators && pad.hapticActuators[0] && typeof pad.hapticActuators[0].pulse === 'function') {\n"
    "        try {\n"
    "            await pad.hapticActuators[0].pulse(Math.max(weakFloat, strongFloat), durationMs);\n"
    "            return true;\n"
    "        } catch (err) {\n"
    "            console.error('Browser blocked haptic pulse:', err, pad.id || pad.index);\n"
    "        }\n"
    "    }\n"
    "    return false;\n"
    "}\n"
    "async function triggerRumble(subpadIndex, low255, high255, duration10ms) {\n"
    "    // Fresh snapshot right now; never use a cached Gamepad object for rumble.\n"
    "    const gamepads = navigator.getGamepads ? navigator.getGamepads() : [];\n"
    "    const weakFloat = clamp01(low255 / 255.0);\n"
    "    const strongFloat = clamp01(high255 / 255.0);\n"
    "    const isNeutral = (!low255 && !high255);\n"
    "\n"
    "    // Switch rumble packets can arrive at ~60Hz. Browser haptics dislike being\n"
    "    // restarted every frame, so keep active rumble alive long enough to spin.\n"
    "    // Neutral packets still pass through with zero magnitude to brake/stop it.\n"
    "    const durationMs = isNeutral ? 10 : 250;\n"
    "    const now = Date.now();\n"
    "    const magKey = weakFloat + '_' + strongFloat;\n"
    "    if (magKey === rumbleLastMag[subpadIndex] && (now - rumbleDebounceTime[subpadIndex] < 100)) {\n"
    "        return true;\n"
    "    }\n"
    "    rumbleLastMag[subpadIndex] = magKey;\n"
    "    rumbleDebounceTime[subpadIndex] = now;\n"
    "\n"
    "    let ok = await rumbleOnePad(freshGamepadForSlot(subpadIndex, gamepads), weakFloat, strongFloat, durationMs);\n"
    "    if (!ok) {\n"
    "        const directPad = gamepads[subpadIndex];\n"
    "        if (directPad) ok = await rumbleOnePad(directPad, weakFloat, strongFloat, durationMs);\n"
    "    }\n"
    "    if (!ok) {\n"
    "        for (const pad of gamepads) {\n"
    "            if (pad && await rumbleOnePad(pad, weakFloat, strongFloat, durationMs)) { ok = true; break; }\n"
    "        }\n"
    "    }\n"
    "    if (!ok && navigator.vibrate && !isNeutral) {\n"
    "        try { navigator.vibrate(durationMs); } catch (err) { console.error('Browser blocked vibration fallback:', err); }\n"
    "    }\n"
    "    return ok;\n"
    "}\n"
    "function clamp16(v) { v = Math.round(v || 0); return Math.max(-32768, Math.min(32767, v)); }\n"
    "function firstVec3(...candidates) {\n"
    "    for (const v of candidates) {\n"
    "        if (!v) continue;\n"
    "        if (Array.isArray(v) || ArrayBuffer.isView(v)) {\n"
    "            if (v.length >= 3) return [Number(v[0]) || 0, Number(v[1]) || 0, Number(v[2]) || 0];\n"
    "        } else if (typeof v === 'object') {\n"
    "            const x = Number(v.x ?? v.beta ?? v.pitch ?? v[0]);\n"
    "            const y = Number(v.y ?? v.gamma ?? v.yaw ?? v[1]);\n"
    "            const z = Number(v.z ?? v.alpha ?? v.roll ?? v[2]);\n"
    "            if (Number.isFinite(x) || Number.isFinite(y) || Number.isFinite(z)) return [x || 0, y || 0, z || 0];\n"
    "        }\n"
    "    }\n"
    "    return null;\n"
    "}\n"
    "function motionFromGamepad(pad) {\n"
    "    if (!pad) return null;\n"
    "    // Standard Gamepad exposes buttons/axes/haptics only. Some browsers/devices\n"
    "    // expose experimental pose/motion fields; use them when available.\n"
    "    // DS4Windows' normal virtual Xbox/DS4 output may still hide gyro from web pages.\n"
    "    const pose = pad.pose || pad.motion || pad.sensor || {};\n"
    "    const acc = firstVec3(pose.linearAcceleration, pose.acceleration, pad.linearAcceleration, pad.acceleration);\n"
    "    const gyro = firstVec3(pose.angularVelocity, pose.gyro, pose.rotationRate, pad.angularVelocity, pad.gyro, pad.rotationRate);\n"
    "    if (!acc && !gyro) return null;\n"
    "    const m = { enabled:true, ax:0, ay:0, az:0, gx:0, gy:0, gz:0 };\n"
    "    if (acc) {\n"
    "        m.ax = clamp16(acc[0] / 9.80665 * 4096);\n"
    "        m.ay = clamp16(acc[1] / 9.80665 * 4096);\n"
    "        m.az = clamp16(acc[2] / 9.80665 * 4096);\n"
    "    }\n"
    "    if (gyro) {\n"
    "        const toSwitchGyro = 180 / Math.PI * 16;\n"
    "        m.gx = clamp16(gyro[0] * toSwitchGyro);\n"
    "        m.gy = clamp16(gyro[1] * toSwitchGyro);\n"
    "        m.gz = clamp16(gyro[2] * toSwitchGyro);\n"
    "    }\n"
    "    return m;\n"
    "}\n"
    "async function requestSensorPermission(apiName) {\n"
    "    const Api = window[apiName];\n"
    "    if (typeof Api === 'undefined') return 'unavailable';\n"
    "    if (typeof Api.requestPermission === 'function') {\n"
    "        try { return await Api.requestPermission(); } catch(e) { console.warn('[motion permission]', apiName, e); return 'error'; }\n"
    "    }\n"
    "    return 'granted';\n"
    "}\n"
    "function installDeviceMotionListener() {\n"
    "    if (motionListenerInstalled || typeof DeviceMotionEvent === 'undefined') return;\n"
    "    motionListenerInstalled = true;\n"
    "    window.addEventListener('devicemotion', e => {\n"
    "        lastDeviceMotionMs = Date.now();\n"
    "        const a = e.accelerationIncludingGravity || e.acceleration || {};\n"
    "        const rr = e.rotationRate || {};\n"
    "        motion.enabled = true;\n"
    "        motion.ax = clamp16((a.x || 0) / 9.80665 * 4096);\n"
    "        motion.ay = clamp16((a.y || 0) / 9.80665 * 4096);\n"
    "        motion.az = clamp16((a.z || 0) / 9.80665 * 4096);\n"
    "        motion.gx = clamp16((rr.beta  || 0) * 16);\n"
    "        motion.gy = clamp16((rr.gamma || 0) * 16);\n"
    "        motion.gz = clamp16((rr.alpha || 0) * 16);\n"
    "    }, {passive:true});\n"
    "}\n"
    "function installDeviceOrientationFallback() {\n"
    "    if (orientationListenerInstalled || typeof DeviceOrientationEvent === 'undefined') return;\n"
    "    orientationListenerInstalled = true;\n"
    "    window.addEventListener('deviceorientation', e => {\n"
    "        if (Date.now() - lastDeviceMotionMs < 250) return;\n"
    "        motion.enabled = true;\n"
    "        motion.gx = clamp16((e.beta  || 0) * 16);\n"
    "        motion.gy = clamp16((e.gamma || 0) * 16);\n"
    "        motion.gz = clamp16((e.alpha || 0) * 16);\n"
    "    }, {passive:true});\n"
    "}\n"
    "async function enableMotion() {\n"
    "    const motionPerm = await requestSensorPermission('DeviceMotionEvent');\n"
    "    const orientPerm = await requestSensorPermission('DeviceOrientationEvent');\n"
    "    // Install best-effort listeners even on plain HTTP.  Some browsers expose\n"
    "    // sensor data without a secure context, and the listener is harmless if not.\n"
    "    if (motionPerm !== 'denied') installDeviceMotionListener();\n"
    "    if (orientPerm !== 'denied') installDeviceOrientationFallback();\n"
    "}\n"
    "function handleRumbleMessage(data) {\n"
    "    if (!(data instanceof ArrayBuffer) || data.byteLength < 8) return;\n"
    "    const v = new DataView(data);\n"
    "    if (v.getUint32(0, true) !== RUMBLE_MAGIC) return;\n"
    "    const idx = v.getUint8(4), low = v.getUint8(5), high = v.getUint8(6), dur10 = v.getUint8(7);\n"
    "    triggerRumble(idx, low, high, dur10);\n"
    "}\n"
    "function makeWsUrl() {\n"
    "    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';\n"
    "    return `${proto}//${window.location.host}/`;\n"
    "}\n"
    "let savedMacros = JSON.parse(localStorage.getItem('nswc_macros') || '[]');\n"
    "if (savedMacros && Array.isArray(savedMacros.macros)) savedMacros = savedMacros.macros; if (!Array.isArray(savedMacros)) savedMacros = [];\n"
    "let macroRunning = false, macroSteps = [], macroStepIndex = 0, macroStepUntil = 0, macroState = null;\n"
    "let macroRecording = false, macroRecordLast = null, macroRecordSince = 0, macroRecorded = [];\n"
    "function macroBtnBit(name) {\n"
    "    name = String(name || '').trim().toUpperCase();\n"
    "    const map = {Y:BTN_Y,B:BTN_B,A:BTN_A,X:BTN_X,L:BTN_L,R:BTN_R,ZL:BTN_ZL,ZR:BTN_ZR,MINUS:BTN_MINUS,'-':BTN_MINUS,PLUS:BTN_PLUS,'+':BTN_PLUS,LSTICK:BTN_LSTICK,LS:BTN_LSTICK,RSTICK:BTN_RSTICK,RS:BTN_RSTICK,HOME:BTN_HOME,CAPTURE:BTN_CAPTURE};\n"
    "    return map[name] || 0;\n"
    "}\n"
    "function macroButtonsFromText(txt) { return String(txt || '').split(/[+|, ]+/).reduce((b, x) => b | macroBtnBit(x), 0); }\n"
    "function macroButtonsToText(buttons) {\n"
    "    const names = [['Y',BTN_Y],['B',BTN_B],['A',BTN_A],['X',BTN_X],['L',BTN_L],['R',BTN_R],['ZL',BTN_ZL],['ZR',BTN_ZR],['MINUS',BTN_MINUS],['PLUS',BTN_PLUS],['LSTICK',BTN_LSTICK],['RSTICK',BTN_RSTICK],['HOME',BTN_HOME],['CAPTURE',BTN_CAPTURE]];\n"
    "    return names.filter(x => buttons & x[1]).map(x => x[0]).join('+') || 'WAIT';\n"
    "}\n"
    "function macroFrameFromState(s) { const axis=v=>v<80?-1:(v>176?1:0); s=s||getNeutralState(); return {buttons:s.buttons||0, hat:s.hat, lx:axis(s.lx), ly:axis(s.ly), rx:axis(s.rx), ry:axis(s.ry)}; }\n"
    "function macroFrameEqual(a,b) { return !!a && !!b && a.buttons===b.buttons && a.hat===b.hat && a.lx===b.lx && a.ly===b.ly && a.rx===b.rx && a.ry===b.ry; }\n"
    "function macroFrameToText(f) { const parts=[]; const btn=macroButtonsToText(f.buttons); if(btn!=='WAIT') parts.push(btn); if(f.hat===HAT_N) parts.push('DPAD_UP'); else if(f.hat===HAT_NE) parts.push('DPAD_UP','DPAD_RIGHT'); else if(f.hat===HAT_E) parts.push('DPAD_RIGHT'); else if(f.hat===HAT_SE) parts.push('DPAD_DOWN','DPAD_RIGHT'); else if(f.hat===HAT_S) parts.push('DPAD_DOWN'); else if(f.hat===HAT_SW) parts.push('DPAD_DOWN','DPAD_LEFT'); else if(f.hat===HAT_W) parts.push('DPAD_LEFT'); else if(f.hat===HAT_NW) parts.push('DPAD_UP','DPAD_LEFT'); if(f.lx<0) parts.push('LSTICK_LEFT'); else if(f.lx>0) parts.push('LSTICK_RIGHT'); if(f.ly<0) parts.push('LSTICK_UP'); else if(f.ly>0) parts.push('LSTICK_DOWN'); if(f.rx<0) parts.push('RSTICK_LEFT'); else if(f.rx>0) parts.push('RSTICK_RIGHT'); if(f.ry<0) parts.push('RSTICK_UP'); else if(f.ry>0) parts.push('RSTICK_DOWN'); return parts.join('+') || 'WAIT'; }\n"
    "function macroCommandString(objOrText) {\n"
    "    if (!objOrText) return '';\n"
    "    if (typeof objOrText === 'string') { try { return macroCommandString(JSON.parse(objOrText)); } catch(e) { return objOrText; } }\n"
    "    if (Array.isArray(objOrText)) return objOrText.map(s => `${s.buttons || s.button || s.cmd || 'WAIT'} ${s.ms || s.duration || 100}`).join('; ');\n"
    "    return objOrText.commands || objOrText.macro || '';\n"
    "}\n"

    "const MACRO_JSON_MAX_BYTES = 50 * 1024 * 1024;\n"
    "const MACRO_VALID_INPUTS = new Set(['A','B','X','Y','L','R','ZL','ZR','MINUS','-','PLUS','+','LSTICK','LS','RSTICK','RS','HOME','CAPTURE','DPAD_UP','DPAD_DOWN','DPAD_LEFT','DPAD_RIGHT','UP','DOWN','LEFT','RIGHT','LSTICK_UP','LSTICK_DOWN','LSTICK_LEFT','LSTICK_RIGHT','LS_UP','LS_DOWN','LS_LEFT','LS_RIGHT','RSTICK_UP','RSTICK_DOWN','RSTICK_LEFT','RSTICK_RIGHT','RS_UP','RS_DOWN','RS_LEFT','RS_RIGHT']);\n"
    "function macroCommandsArray(objOrText) { const obj = (typeof objOrText === 'string') ? (()=>{ try { return JSON.parse(objOrText); } catch(_) { return {commands:objOrText}; } })() : objOrText; const c = obj && obj.commands !== undefined ? obj.commands : objOrText; if (Array.isArray(c)) return c.map(String); if (typeof c === 'string') return c.split(/[;\\n\\r]+/).map(x=>x.trim()).filter(Boolean); throw new Error('commands must be a string or array of strings'); }\n"
    "function validateMacro(objOrText) { const lines = macroCommandsArray(objOrText); if (!lines.length) throw new Error('no macro commands found'); for (const line of lines) { const m = line.trim().match(/^(.*?)\\s+(\\d+)$/); if (!m) throw new Error('missing duration: '+line); const cmd=m[1].trim(), ms=Number(m[2]); if (!Number.isSafeInteger(ms) || ms <= 0 || ms > 4294967295) throw new Error('invalid duration: '+line); if (cmd.toUpperCase()==='WAIT' || cmd.toUpperCase()==='LOOP') continue; const toks=cmd.split(/[+,|\\s]+/).filter(Boolean); if (!toks.length) throw new Error('missing input: '+line); const seen=new Set(); for (const t0 of toks) { const t=t0.toUpperCase(); if (!MACRO_VALID_INPUTS.has(t)) throw new Error('unknown input '+t0+' in '+line); seen.add(t); } const has=(...xs)=>xs.some(x=>seen.has(x)); if (has('DPAD_UP','UP')&&has('DPAD_DOWN','DOWN')) throw new Error('DPAD up/down conflict: '+line); if (has('DPAD_LEFT','LEFT')&&has('DPAD_RIGHT','RIGHT')) throw new Error('DPAD left/right conflict: '+line); if (has('LSTICK_UP','LS_UP')&&has('LSTICK_DOWN','LS_DOWN')) throw new Error('left stick up/down conflict: '+line); if (has('LSTICK_LEFT','LS_LEFT')&&has('LSTICK_RIGHT','LS_RIGHT')) throw new Error('left stick left/right conflict: '+line); if (has('RSTICK_UP','RS_UP')&&has('RSTICK_DOWN','RS_DOWN')) throw new Error('right stick up/down conflict: '+line); if (has('RSTICK_LEFT','RS_LEFT')&&has('RSTICK_RIGHT','RS_RIGHT')) throw new Error('right stick left/right conflict: '+line); } return lines; }\n"
    "function macroPrettyObject(objOrText) { const lines=validateMacro(objOrText); const obj=(typeof objOrText==='string')?(()=>{try{return JSON.parse(objOrText);}catch(_){return {commands:objOrText};}})():objOrText; return {name:(obj&&obj.name)||'Macro', commands:lines}; }\n"
    "function parseMacro(objOrText) {\n"
    "    const text = macroCommandString(objOrText).replace(/[\\r\\n]+/g, ';');\n"
    "    const steps = [];\n"
    "    let segmentStart = 0; const addStep = p => { const m = p.match(/^(.*?)\\s+(\\d+)$/); if (!m) return; const cmd = m[1].trim().toUpperCase(), ms = Math.max(1, parseInt(m[2], 10)); if (cmd === 'LOOP') { const block = steps.slice(segmentStart); for (let i=1; i<ms && steps.length<1000000; ++i) steps.push(...block.map(x=>({...x}))); segmentStart = steps.length; return; } const st = {...getNeutralState(), ms}; if (cmd !== 'WAIT') for (const t0 of cmd.split(/[+,|\\s]+/).filter(Boolean)) { const t=t0.toUpperCase(); st.buttons |= macroBtnBit(t); if (t==='DPAD_UP'||t==='UP') st.hat=HAT_N; else if (t==='DPAD_DOWN'||t==='DOWN') st.hat=HAT_S; else if (t==='DPAD_LEFT'||t==='LEFT') st.hat=HAT_W; else if (t==='DPAD_RIGHT'||t==='RIGHT') st.hat=HAT_E; else if (t==='LSTICK_UP'||t==='LS_UP') st.ly=0; else if (t==='LSTICK_DOWN'||t==='LS_DOWN') st.ly=255; else if (t==='LSTICK_LEFT'||t==='LS_LEFT') st.lx=0; else if (t==='LSTICK_RIGHT'||t==='LS_RIGHT') st.lx=255; else if (t==='RSTICK_UP'||t==='RS_UP') st.ry=0; else if (t==='RSTICK_DOWN'||t==='RS_DOWN') st.ry=255; else if (t==='RSTICK_LEFT'||t==='RS_LEFT') st.rx=0; else if (t==='RSTICK_RIGHT'||t==='RS_RIGHT') st.rx=255; } steps.push(st); };\n"
    "    for (const raw of text.split(';')) { const p = raw.trim(); if (p) addStep(p); }\n"
    "    return steps;\n"
    "}\n"
    "function normalizeMacroKey(k) { return String(k || '').trim().toUpperCase(); }\n"
    "function macroEntryName(m,i) { return (m && m.name) || `Macro ${i+1}`; }\n"
    "function uniqueMacroName(base) { base=String(base||'Recorded Macro').trim()||'Recorded Macro'; let n=base, s=2; while(savedMacros.some(m=>String(m.name||'').toUpperCase()===n.toUpperCase())) n=`${base} ${s++}`; return n; }\n"
    "function macroPrettyEntry(obj) { const m=macroPrettyObject(obj); if (!m.name) m.name='Macro'; m.hotkey=normalizeMacroKey(obj && obj.hotkey); return m; }\n"
    "function persistMacros() { savedMacros = savedMacros.map(macroPrettyEntry); localStorage.setItem('nswc_macros', JSON.stringify(savedMacros)); }\n"
    "function macroEntriesExport(entries=savedMacros) { return JSON.stringify({macros:entries.map(macroPrettyEntry)}, null, 2); }\n"
    "function macroHotkeyConflicts(code, skip=-1) { code=normalizeMacroKey(code); if (!code) return false; if (Object.values(currentBindings).includes(code)) return 'keyboard binding'; const dup=savedMacros.find((m,i)=>i!==skip && normalizeMacroKey(m.hotkey)===code); return dup ? (dup.name || 'another macro') : false; }\n"
    "function refreshMacroList() { const rows=document.getElementById('macroRows'); if(!rows) return; rows.innerHTML=''; if(!savedMacros.length){ const e=document.createElement('div'); e.textContent='No macros'; e.style.cssText='text-align:center; width:250px; padding:4px;'; rows.appendChild(e); } savedMacros.forEach((m,i)=>{ const r=document.createElement('div'); r.style.cssText='display:grid; grid-template-columns:minmax(0,250px) 110px 68px 64px 64px; gap:4px; margin-bottom:4px;'; const add=(txt,fn)=>{const b=document.createElement('button'); b.textContent=txt; b.onclick=fn; r.appendChild(b);}; add(macroEntryName(m,i),()=>runMacroServerSide(m)); add(normalizeMacroKey(m.hotkey),()=>beginMacroHotkeyListen(i)); add('Rename',()=>renameMacro(i)); add('Export',()=>exportOneMacro(i)); add('Delete',()=>{savedMacros.splice(i,1); persistMacros(); refreshMacroList();}); rows.appendChild(r); }); }\n"
    "function startMacro(objOrText) { macroSteps = parseMacro(objOrText); if (!macroSteps.length) { alert('No usable macro commands. Example: WAIT 200; A 100; B 100'); return; } macroRunning = true; macroStepIndex = 0; macroState = getNeutralState(); macroStepUntil = performance.now(); }\n"
    "function updateMacroState() {\n"
    "    if (!macroRunning) return null; const now = performance.now();\n"
    "    while (macroRunning && now >= macroStepUntil) { const st = macroSteps[macroStepIndex++]; if (!st) { macroRunning = false; macroState = getNeutralState(); return null; } macroState = {...getNeutralState(), ...st}; macroStepUntil = now + st.ms; }\n"
    "    return macroState;\n"
    "}\n"
    "function sampleMacroRecording(p1) {\n"
    "    if (!macroRecording) return; const now = performance.now(); const frame = macroFrameFromState(p1);\n"
    "    if (macroRecordLast === null) { macroRecordLast = frame; macroRecordSince = now; return; }\n"
    "    if (!macroFrameEqual(frame, macroRecordLast)) { const dur = Math.max(1, Math.round(now - macroRecordSince)); macroRecorded.push(`${macroFrameToText(macroRecordLast)} ${dur}`); macroRecordLast = frame; macroRecordSince = now; }\n"
    "}\n"

    "function sendServerMacroChunks(payload, subpad=0) {\n"
    "    const enc = new TextEncoder(); const bytes = enc.encode(payload);\n"
    "    if (bytes.length > MACRO_JSON_MAX_BYTES) throw new Error('macro JSON exceeds 50MB limit');\n"
    "    const chunkSize = 32000, count = Math.ceil(bytes.length / chunkSize), uploadId = (Date.now() ^ Math.floor(Math.random()*0xFFFFFFFF)) >>> 0;\n"
    "    for (let i=0; i<count; i++) {\n"
    "        const start=i*chunkSize, chunk=bytes.slice(start, Math.min(bytes.length, start+chunkSize));\n"
    "        const buf = new ArrayBuffer(30 + chunk.length), v = new DataView(buf);\n"
    "        v.setUint32(0, 0x4E534D4B, true); v.setUint8(4, PROTO_VERSION); v.setUint8(5, subpad & 3); v.setUint8(6, i+1===count ? 1 : 0); v.setUint8(7, 0);\n"
    "        v.setUint32(8, uploadId, true); v.setUint32(12, i, true); v.setUint32(16, count, true); v.setUint32(20, bytes.length, true); v.setUint16(24, chunk.length, true); v.setUint32(26, seqCounter++, true);\n"
    "        new Uint8Array(buf, 30).set(chunk); ws.send(buf);\n"
    "    }\n"
    "}\n"
    "function runMacroServerSide(objOrText) {\n"
    "    const obj = (typeof objOrText === 'string') ? {name:'Macro', commands:objOrText} : objOrText;\n"
    "    try { validateMacro(obj); } catch(err) { alert('Invalid macro: '+err.message); return; }\n"
    "    if (ws && ws.readyState === WebSocket.OPEN) { try { sendServerMacroChunks(JSON.stringify(macroPrettyObject(obj))); return; } catch(err) { alert('Macro upload failed: '+err.message); return; } }\n"
    "    startMacro(obj); // fallback for old/offline servers\n"
    "}\n"
    "function exportMacrosJson() {\n"
    "    const blob = new Blob([macroEntriesExport()], {type:'application/json'});\n"
    "    const a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = 'ns-macros.json'; a.click(); setTimeout(()=>URL.revokeObjectURL(a.href), 1000);\n"
    "}\n"
    "function exportOneMacro(i) { const blob=new Blob([macroEntriesExport([savedMacros[i]])],{type:'application/json'}); const a=document.createElement('a'); a.href=URL.createObjectURL(blob); a.download=`${macroEntryName(savedMacros[i],i).replace(/[\\\\/:*?\"<>|]/g,'_')}.json`; a.click(); setTimeout(()=>URL.revokeObjectURL(a.href),1000); }\n"
    "function importMacrosJsonText(txt) {\n"
    "    let data = JSON.parse(txt); let replace=false; if (data && Array.isArray(data.macros)) { data=data.macros; replace=true; } else if (!Array.isArray(data)) data=[data];\n"
    "    const imported=[]; for (const m of data) if (m && (m.commands || typeof m === 'string')) { const obj=macroPrettyEntry(typeof m === 'string' ? {name:`Macro ${savedMacros.length+1}`, commands:m} : m); validateMacro(obj); imported.push(obj); }\n"
    "    if (replace || imported.length > 1) savedMacros=imported; else if (imported.length) { const idx=savedMacros.findIndex(m=>String(m.name||'').toUpperCase()===String(imported[0].name||'').toUpperCase()); if(idx>=0) savedMacros[idx]=imported[0]; else savedMacros.push(imported[0]); }\n"
    "    persistMacros(); refreshMacroList();\n"
    "}\n"
    "function beginMacroHotkeyListen(i) { const once=(ev)=>{ ev.preventDefault(); let code=ev.code==='Escape'?'':ev.code; const conflict=macroHotkeyConflicts(code,i); if(conflict){ alert('Macro keybind is already used by: '+conflict); } else { savedMacros[i].hotkey=code; persistMacros(); refreshMacroList(); } window.removeEventListener('keydown', once, true); }; window.addEventListener('keydown', once, true); }\n"
    "function renameMacro(i) { const old=macroEntryName(savedMacros[i],i); const name=prompt('Macro name:', old); if(name===null) return; const n=name.trim(); if(!n){ alert('Macro name cannot be empty.'); return; } const dup=savedMacros.findIndex((m,j)=>j!==i && String(m.name||'').toUpperCase()===n.toUpperCase()); if(dup>=0){ alert('Another macro already uses that name.'); return; } savedMacros[i].name=n; savedMacros[i]=macroPrettyEntry(savedMacros[i]); persistMacros(); refreshMacroList(); }\n"
    "function wireMacroMenu() {\n"    "    const btn = document.getElementById('btnMacros'); if (!btn) return;\n"
    "    btn.onclick = () => { refreshMacroList(); document.getElementById('macroOverlay').style.display='flex'; };\n"
    "    document.getElementById('btnMacroClose').onclick = () => document.getElementById('macroOverlay').style.display='none';\n"
    "    document.getElementById('btnMacroExport').onclick = exportMacrosJson;\n"
    "    document.getElementById('btnMacroImport').onclick = () => document.getElementById('macroImportFile').click();\n"
    "    document.getElementById('macroImportFile').onchange = e => { const f=e.target.files[0]; if(!f) return; if (f.size > MACRO_JSON_MAX_BYTES) { alert('Macro JSON exceeds the 50MB limit.'); e.target.value=''; return; } const r=new FileReader(); r.onload=()=>{ try{ importMacrosJsonText(String(r.result)); } catch(err){ alert('Invalid macro JSON: '+err.message); } }; r.readAsText(f); e.target.value=''; };\n"
    "    document.getElementById('btnMacroRecord').onclick = () => {\n"
    "        if (!macroRecording) { macroRecording=true; macroRecordLast=null; macroRecorded=[]; document.getElementById('btnMacroRecord').innerText='Stop'; }\n"
    "        else { macroRecording=false; if (macroRecordLast !== null) macroRecorded.push(`${macroFrameToText(macroRecordLast)} ${Math.max(1, Math.round(performance.now()-macroRecordSince))}`); if(macroRecorded.some(x=>!x.startsWith('WAIT '))) savedMacros.push(macroPrettyEntry({name:uniqueMacroName('Recorded Macro'), commands:macroRecorded})); persistMacros(); refreshMacroList(); document.getElementById('btnMacroRecord').innerText='Record P1'; }\n"
    "    };\n"
    "}\n"
    "function buildAndSendPacket() {\n"
    "    if (!ws || ws.readyState !== WebSocket.OPEN) return;\n"
    "    const rawGamepads = navigator.getGamepads ? navigator.getGamepads() : [];\n"
    "    const activePads = [];\n"
    "    for (let i = 0; i < rawGamepads.length; i++) if (rawGamepads[i]) activePads.push(rawGamepads[i]);\n"
    "    lastActivePads = activePads;\n"
    "    const mode = parseInt(document.getElementById('kbMode').value);\n"
    "    const kbState = getKeyboardState();\n"
    "    let slotStates = [null, null, null, null];\n"
    "    let uiText = [\"\", \"\", \"\", \"\"];\n"
    "    let nextRumbleTargets = [null, null, null, null];\n"
    "    let nextRumbleTargetIndexes = [-1, -1, -1, -1];\n"
    "    let slotPresent = [false, false, false, false];\n"
    "    let slotMotion = [null, null, null, null];\n"
    "    if (mode === 0) {\n"
    "        for (let i = 0; i < 4; i++) {\n"
    "            let pad = activePads[i];\n"
    "            let gp = getGamepadState(pad);\n"
    "            slotStates[i] = gp || getNeutralState();\n"
    "            nextRumbleTargets[i] = gp ? pad : null;\n"
    "            nextRumbleTargetIndexes[i] = gp ? pad.index : -1;\n"
    "            slotPresent[i] = !!gp;\n"
    "            slotMotion[i] = gp ? motionFromGamepad(pad) : null;\n"
    "            uiText[i] = gp ? \"Connected\" : \"Not connected\";\n"
    "        }\n"
    "    } else if (mode === 1) {\n"
    "        slotStates[0] = kbState; slotPresent[0] = true; uiText[0] = `Keyboard (Connected)`;\n"
    "        for (let i = 1; i < 4; i++) {\n"
    "            let pad = activePads[i - 1];\n"
    "            let gp = getGamepadState(pad);\n"
    "            slotStates[i] = gp || getNeutralState();\n"
    "            nextRumbleTargets[i] = gp ? pad : null;\n"
    "            nextRumbleTargetIndexes[i] = gp ? pad.index : -1;\n"
    "            slotPresent[i] = !!gp;\n"
    "            slotMotion[i] = gp ? motionFromGamepad(pad) : null;\n"
    "            uiText[i] = gp ? \"Connected\" : \"Not connected\";\n"
    "        }\n"
    "    } else if (mode === 2) {\n"
    "        let pad0 = activePads[0];\n"
    "        let gp0 = getGamepadState(pad0);\n"
    "        slotStates[0] = mergeStates(kbState, gp0 || getNeutralState());\n"
    "        nextRumbleTargets[0] = gp0 ? pad0 : null;\n"
    "        nextRumbleTargetIndexes[0] = gp0 ? pad0.index : -1;\n"
    "        slotPresent[0] = true;\n"
    "        slotMotion[0] = gp0 ? motionFromGamepad(pad0) : null;\n"
    "        uiText[0] = `${gp0 ? \"Connected\" : \"Not connected\"} \\ Keyboard`;\n"
    "        for (let i = 1; i < 4; i++) {\n"
    "            let pad = activePads[i];\n"
    "            let gp = getGamepadState(pad);\n"
    "            slotStates[i] = gp || getNeutralState();\n"
    "            nextRumbleTargets[i] = gp ? pad : null;\n"
    "            nextRumbleTargetIndexes[i] = gp ? pad.index : -1;\n"
    "            slotPresent[i] = !!gp;\n"
    "            slotMotion[i] = gp ? motionFromGamepad(pad) : null;\n"
    "            uiText[i] = gp ? \"Connected\" : \"Not connected\";\n"
    "        }\n"
    "    }\n"
    "    rumbleTargets = nextRumbleTargets;\n"
    "    rumbleTargetIndexes = nextRumbleTargetIndexes;\n"
    "    sampleMacroRecording(slotStates[0]);\n"
    "    const macroOverride = updateMacroState();\n"
    "    if (macroOverride) { slotStates[0] = macroOverride; uiText[0] = 'Macro running'; }\n"
    "    const buffer = new ArrayBuffer(PACKET_SIZE), view = new DataView(buffer);\n"
    "    view.setUint32(0, PROTO_MAGIC, true); view.setUint8(4, PROTO_VERSION); view.setUint8(5, 0);\n"
    "    view.setUint16(6, 0, true); view.setUint32(8, seqCounter++, true); view.setBigUint64(12, BigInt(Date.now() * 1000), true);\n"
    "    for(let p = 0; p < 4; p++) {\n"
    "        document.getElementById(`p${p+1}Text`).innerText = `P${p+1}: ${uiText[p]}`;\n"
    "        const finalButtons = normalizeSystemShortcuts(slotStates[p].buttons);\n"
    "        const offset = 20 + (p * EXT_REPORT_SIZE);\n"
    "        view.setUint16(offset, finalButtons, true);\n"
    "        view.setUint8(offset + 2, slotStates[p].hat);\n"
    "        view.setUint8(offset + 3, slotStates[p].lx); view.setUint8(offset + 4, slotStates[p].ly);\n"
    "        view.setUint8(offset + 5, slotStates[p].rx); view.setUint8(offset + 6, slotStates[p].ry);\n"
    "        view.setUint8(offset + 7, slotPresent[p] ? PAD_PRESENT : 0);\n"
    "        const motionForSlot = slotMotion[p] || ((p === 0 && motion.enabled) ? motion : null);\n"
    "        if (motionForSlot && motionForSlot.enabled) {\n"
    "            view.setInt16(offset + 8, motionForSlot.ax, true); view.setInt16(offset + 10, motionForSlot.ay, true); view.setInt16(offset + 12, motionForSlot.az, true);\n"
    "            view.setInt16(offset + 14, motionForSlot.gx, true); view.setInt16(offset + 16, motionForSlot.gy, true); view.setInt16(offset + 18, motionForSlot.gz, true);\n"
    "            view.setUint8(offset + 20, 1); view.setUint8(offset + 21, 0); view.setUint8(offset + 22, 0); view.setUint8(offset + 23, 0);\n"
    "        } else {\n"
    "            for (let k = 8; k < EXT_REPORT_SIZE; k++) view.setUint8(offset + k, 0);\n"
    "        }\n"
    "    }\n"
    "    ws.send(buffer);\n"
    "}\n"
    "document.getElementById('btnConnect').onclick = async () => {\n"
    "    if (isConnected) {\n"
    "        clearInterval(loopId); ws.close(); isConnected = false;\n"
    "        document.getElementById('btnConnect').innerText = \"Connect\";\n"
    "        document.getElementById('statusText').innerText = \"Disconnected\";\n"
    "        document.getElementById('kbMode').disabled = false;\n"
    "        return;\n"
    "    }\n"
    "    await enableMotion();\n"
    "    const wsUrl = makeWsUrl();\n"
    "    ws = new WebSocket(wsUrl); ws.binaryType = \"arraybuffer\";\n"
    "    ws.onmessage = (ev) => handleRumbleMessage(ev.data);\n"
    "    ws.onopen = () => {\n"
    "        isConnected = true;\n"
    "        document.getElementById('btnConnect').innerText = \"Disconnect\";\n"
    "        document.getElementById('kbMode').disabled = true;\n"
    "        document.getElementById('statusText').innerText = `Connected to Pi Proxy.`;\n"
    "        try { window.focus(); document.body.focus(); } catch(e) {}\n"
    "        loopId = setInterval(buildAndSendPacket, 4);\n"
    "    };\n"
    "    ws.onerror = () => alert(\"Failed to connect to proxy!\");\n"
    "    ws.onclose = () => {\n"
    "        isConnected = false; clearInterval(loopId);\n"
    "        document.getElementById('btnConnect').innerText = \"Connect\";\n"
    "        document.getElementById('statusText').innerText = \"Disconnected\";\n"
    "        document.getElementById('kbMode').disabled = false;\n"
    "    }\n"
    "};\n"
    "function formatKeyName(code) {\n"
    "    if (code === 'Unbound') return '';\n"
    "    if (code.startsWith('Key')) return code.replace('Key', '');\n"
    "    if (code.startsWith('Digit')) return code.replace('Digit', '');\n"
    "    if (code.startsWith('Arrow')) return code.replace('Arrow', '');\n"
    "    if (code === 'ShiftLeft') return 'LShift'; if (code === 'ShiftRight') return 'RShift';\n"
    "    if (code === 'ControlLeft') return 'LCtrl'; if (code === 'ControlRight') return 'RCtrl';\n"
    "    if (code === 'AltLeft') return 'LAlt'; if (code === 'AltRight') return 'RAlt';\n"
    "    return code;\n"
    "}\n"
    "let activeBindKey = null; let isSetupMode = false; let setupQueue = [];\n"
    "function renderBindings() {\n"
    "    const list = document.getElementById('bindingsList'); list.innerHTML = '';\n"
    "    for (const [btn, code] of Object.entries(currentBindings)) {\n"
    "        const row = document.createElement('div'); row.className = 'bind-row';\n"
    "        const label = document.createElement('span'); label.innerText = btn.replace('BTN_', '');\n"
    "        const btnChange = document.createElement('button'); btnChange.className = 'bind-btn';\n"
    "        btnChange.innerText = formatKeyName(code); btnChange.id = `btn-${btn}`;\n"
    "        btnChange.onclick = () => {\n"
    "            if (isSetupMode) return;\n"
    "            if (activeBindKey) document.getElementById(`btn-${activeBindKey}`).classList.remove('listening');\n"
    "            activeBindKey = btn; btnChange.innerText = \"---\"; btnChange.classList.add('listening');\n"
    "        };\n"
    "        row.appendChild(label); row.appendChild(btnChange); list.appendChild(row);\n"
    "    }\n"
    "}\n"
    "function startNextSetupBind() {\n"
    "    if (setupQueue.length === 0) {\n"
    "        isSetupMode = false; activeBindKey = null; renderBindings(); return;\n"
    "    }\n"
    "    activeBindKey = setupQueue.shift(); renderBindings();\n"
    "    const targetBtn = document.getElementById(`btn-${activeBindKey}`);\n"
    "    if (targetBtn) { targetBtn.innerText = \"---\"; targetBtn.classList.add('listening'); targetBtn.scrollIntoView({ behavior: 'smooth', block: 'center' }); }\n"
    "}\n"
    "function remapKey(code) {\n"
    "    if (!activeBindKey) return;\n"
    "    if (code === 'Escape') { isSetupMode = false; activeBindKey = null; renderBindings(); return; }\n"
    "    if (isSetupMode) {\n"
    "        for (const existingCode of Object.values(currentBindings)) if (existingCode === code) return;\n"
    "        currentBindings[activeBindKey] = code; startNextSetupBind();\n"
    "    } else {\n"
    "        for (const [existingBtn, existingCode] of Object.entries(currentBindings)) {\n"
    "            if (existingCode === code && existingBtn !== activeBindKey) currentBindings[existingBtn] = 'Unbound';\n"
    "        }\n"
    "        currentBindings[activeBindKey] = code; activeBindKey = null; renderBindings();\n"
    "    }\n"
    "}\n"
    "document.getElementById('btnBindings').onclick = () => {\n"
    "    preEditBindings = { ...currentBindings }; isSetupMode = false; activeBindKey = null;\n"
    "    renderBindings(); document.getElementById('modalOverlay').style.display = 'flex';\n"
    "};\n"
    "document.getElementById('btnSaveBindings').onclick = () => {\n"
    "    localStorage.setItem('nswc_bindings', JSON.stringify(currentBindings));\n"
    "    isSetupMode = false; activeBindKey = null; document.getElementById('modalOverlay').style.display = 'none';\n"
    "};\n"
    "document.getElementById('btnCancelBindings').onclick = () => {\n"
    "    currentBindings = { ...preEditBindings };\n"
    "    isSetupMode = false; activeBindKey = null; document.getElementById('modalOverlay').style.display = 'none';\n"
    "};\n"
    "document.getElementById('btnResetBindings').onclick = () => {\n"
    "    if (isSetupMode) isSetupMode = false;\n"
    "    currentBindings = { ...defaultBindings }; activeBindKey = null; renderBindings();\n"
    "};\n"
    "document.getElementById('btnSetupBindings').onclick = () => {\n"
    "    for (let k in currentBindings) currentBindings[k] = 'Unbound';\n"
    "    setupQueue = Object.keys(currentBindings); isSetupMode = true; startNextSetupBind();\n"
    "};\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static const char MOBILE_STYLE_AND_DOM[] =
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover\">\n"
    "    <style>\n"
    "        html, body { background: #111; color: #fff; margin:0; padding:0; width: 100%; height: 100%; overflow:hidden; touch-action:none; user-select:none; -webkit-user-select:none; font-family:sans-serif; }\n"
    "        #gamepad { position: fixed; top: env(safe-area-inset-top, 0px); right: env(safe-area-inset-right, 0px); bottom: env(safe-area-inset-bottom, 0px); left: env(safe-area-inset-left, 0px); box-sizing: border-box; }\n"
    "        #rotate-msg { display:none; position:absolute; inset:0; background:#000; z-index:9999; justify-content:center; align-items:center; text-align:center; padding:20px; font-size:18px; }\n"
    "        @media (orientation: portrait) { #rotate-msg { display:flex; } #gamepad { display:none; } }\n"
    "        \n"
    "        .edit-item { position: absolute; box-sizing: border-box; }\n"
    "        .btn { background: rgba(255,255,255,0.15); border: 1px solid rgba(255,255,255,0.2); display:flex; justify-content:center; align-items:center; font-weight:bold; color:#ddd; font-size:16px; box-shadow: 0 4px 8px rgba(0,0,0,0.3); }\n"
    "        .btn, .joystick, .cross { touch-action:none; -webkit-tap-highlight-color: transparent; -webkit-touch-callout:none; }\n"
    "        .btn.active { background: rgba(255,255,255,0.7); color: #000; box-shadow: 0 0 10px rgba(255,255,255,0.5); }\n"
    "        .btn-circle { border-radius: 50%; width: 100%; height: 100%; }\n"
    "        .btn-shoulder { border-radius: 8px; font-size:14px; }\n"
    "        .btn-sys { border-radius: 50%; font-size: 14px; }\n"
    "        \n"
    "        .cross { display: grid; grid-template-columns: 1fr 1fr 1fr; grid-template-rows: 1fr 1fr 1fr; gap: 4px; }\n"
    "        .cross .up { grid-column: 2; grid-row: 1; }\n"
    "        .cross .down { grid-column: 2; grid-row: 3; }\n"
    "        .cross .left { grid-column: 1; grid-row: 2; }\n"
    "        .cross .right { grid-column: 3; grid-row: 2; }\n"
    "        .joystick { background: rgba(255,255,255,0.05); border: 2px solid rgba(255,255,255,0.15); border-radius: 50%; box-shadow: inset 0 0 20px rgba(0,0,0,0.5); aspect-ratio: 1/1; }\n"
    "        .knob { width: 40%; height: 40%; background: rgba(255,255,255,0.3); border-radius: 50%; position: absolute; top: 30%; left: 30%; pointer-events: none; box-shadow: 0 4px 10px rgba(0,0,0,0.4); }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n";

static const char MOBILE_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <title>NS Touch Controls</title>\n"
    "%s" // MOBILE_STYLE_AND_DOM
    "    <style>\n"
    "        #btnConnect { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); padding: 15px 30px; border-radius: 30px; background: rgba(200,0,0,0.8); border:none; color:#fff; font-weight:bold; font-size: 18px; z-index: 100; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }\n"
    "        #btnConnect.connected { background: rgba(0,200,0,0.8); }\n"
    "        #statusDot { display: none; position: absolute; bottom: 20px; right: 20px; width: 15px; height: 15px; background: #0f0; border-radius: 50%; box-shadow: 0 0 10px #0f0; z-index: 100; cursor: pointer; }\n"
    "        #rumblePulse { display:none; position:absolute; inset:0; pointer-events:none; z-index:99; background:rgba(255,255,255,0.12); opacity:0; transition:opacity 80ms linear; }\n"
    "        #rumblePulse.active { opacity:1; }\n"
    "    </style>\n"
    "    <div id=\"rotate-msg\">Please rotate to landscape mode.</div>\n"
    "    <div id=\"gamepad\">\n"
    "        <div id=\"statusDot\"></div>\n"
    "        <div id=\"rumblePulse\"></div>\n"
    "        <button id=\"btnConnect\">Connect</button>\n"
    "        <div id=\"btn-zl\" class=\"edit-item btn-shoulder btn btn-map\" data-btn=\"64\">ZL</div>\n"
    "        <div id=\"btn-l\" class=\"edit-item btn-shoulder btn btn-map\" data-btn=\"16\">L</div>\n"
    "        <div id=\"btn-zr\" class=\"edit-item btn-shoulder btn btn-map\" data-btn=\"128\">ZR</div>\n"
    "        <div id=\"btn-r\" class=\"edit-item btn-shoulder btn btn-map\" data-btn=\"32\">R</div>\n"
    "        <div id=\"btn-minus\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"256\">-</div>\n"
    "        <div id=\"btn-plus\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"512\">+</div>\n"
    "        <div id=\"btn-capture\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"8192\">O</div>\n"
    "        <div id=\"btn-home\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"4096\">⌂</div>\n"
    "        <div id=\"lstick\" class=\"edit-item joystick\"><div class=\"knob\" id=\"lknob\"></div></div>\n"
    "        <div id=\"btn-ls\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"1024\">LS</div>\n"
    "        <div id=\"dpad\" class=\"edit-item cross\">\n"
    "            <div class=\"btn btn-circle up btn-dpad\" data-dir=\"u\">&#x25B2;&#xFE0E;</div><div class=\"btn btn-circle left btn-dpad\" data-dir=\"l\">&#x25C0;&#xFE0E;</div>\n"
    "            <div class=\"btn btn-circle right btn-dpad\" data-dir=\"r\">&#x25B6;&#xFE0E;</div><div class=\"btn btn-circle down btn-dpad\" data-dir=\"d\">&#x25BC;&#xFE0E;</div>\n"
    "        </div>\n"
    "        <div id=\"abxy\" class=\"edit-item cross\">\n"
    "            <div class=\"btn btn-circle up btn-map\" data-btn=\"8\">X</div><div class=\"btn btn-circle left btn-map\" data-btn=\"1\">Y</div>\n"
    "            <div class=\"btn btn-circle right btn-map\" data-btn=\"4\">A</div><div class=\"btn btn-circle down btn-map\" data-btn=\"2\">B</div>\n"
    "        </div>\n"
    "        <div id=\"rstick\" class=\"edit-item joystick\"><div class=\"knob\" id=\"rknob\"></div></div>\n"
    "        <div id=\"btn-rs\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"2048\">RS</div>\n"
    "    </div>\n"
    "<script>\n"
    "const defLayout = {\n"
    "    'btn-zl': {l:4, t:4, w:14, h:8}, 'btn-l': {l:4, t:14, w:14, h:8},\n"
    "    'btn-zr': {l:82, t:4, w:14, h:8}, 'btn-r': {l:82, t:14, w:14, h:8},\n"
    "    'btn-minus': {l:38, t:5, w:6}, 'btn-plus': {l:56, t:5, w:6},\n"
    "    'btn-capture': {l:42, t:18, w:5}, 'btn-home': {l:53, t:18, w:5},\n"
    "    'lstick': {l:6, t:35, w:16}, 'btn-ls': {l:2, t:65, w:5},\n"
    "    'dpad': {l:22, t:60, w:16},\n"
    "    'abxy': {l:78, t:35, w:16},\n"
    "    'rstick': {l:62, t:60, w:16}, 'btn-rs': {l:85, t:80, w:5}\n"
    "};\n"
    "let layout = JSON.parse(localStorage.getItem('nswc_layout')) || defLayout;\n"
    "function applyLayout() {\n"
    "    for(let id in defLayout) {\n"
    "        let el = document.getElementById(id);\n"
    "        if(!el) continue;\n"
    "        let conf = layout[id] || defLayout[id];\n"
    "        if(conf.hide) { el.style.display = 'none'; }\n"
    "        else {\n"
    "            if(id === 'dpad' || id === 'abxy') el.style.display = 'grid';\n"
    "            else el.style.display = 'flex';\n"
    "            el.style.left = conf.l + '%'; el.style.top = conf.t + '%'; el.style.width = conf.w + '%';\n"
    "            if(conf.h) el.style.height = conf.h + '%';\n"
    "            else { el.style.aspectRatio = '1 / 1'; el.style.height = 'auto'; }\n"
    "        }\n"
    "    }\n"
    "}\n"
    "applyLayout();\n"
    "const PROTO_MAGIC = 0x4E535743, PROTO_VERSION = 5, RUMBLE_MAGIC = 0x4E535652;\n"
    "const PAD_PRESENT = 1;\n"
    "const FLAG_SINGLE_PAD = 0x04;\n"
    "const EXT_REPORT_SIZE = 24, PACKET_SIZE = 116;\n"
    "const BTN_MINUS = 1<<8, BTN_PLUS = 1<<9, BTN_LSTICK = 1<<10, BTN_RSTICK = 1<<11;\n"
    "const BTN_HOME = 1<<12, BTN_CAPTURE = 1<<13;\n"
    "let ws = null, loopId = null, seqCounter = 0, isConnected = false, connectTimeout = null;\n"
    "let state = { buttons: 0, hat: 8, lx: 128, ly: 128, rx: 128, ry: 128 };\n"
    "let motion = { enabled:false, ax:0, ay:0, az:0, gx:0, gy:0, gz:0 };\n"
    "let motionListenerInstalled = false, orientationListenerInstalled = false, lastDeviceMotionMs = 0;\n"
    "let mobileRumbleDebounceTime = 0;\n"
    "let mobileRumbleLastMag = '';\n"
    "const buttonControls = Array.from(document.querySelectorAll('.btn-map,.btn-dpad'));\n"
    "const activeTouchControls = new Map();\n"
    "const allTouchButtonBits = buttonControls\n"
    "    .filter(el => el.classList.contains('btn-map'))\n"
    "    .reduce((mask, el) => mask | parseInt(el.dataset.btn), 0);\n"
    "const dpad = { u:false, d:false, l:false, r:false };\n"
    "function updateHat() {\n"
    "    if (dpad.u && dpad.r) state.hat = 1; else if (dpad.u && dpad.l) state.hat = 7;\n"
    "    else if (dpad.d && dpad.r) state.hat = 3; else if (dpad.d && dpad.l) state.hat = 5;\n"
    "    else if (dpad.u) state.hat = 0; else if (dpad.d) state.hat = 4;\n"
    "    else if (dpad.l) state.hat = 6; else if (dpad.r) state.hat = 2; else state.hat = 8;\n"
    "}\n"
    "function controlForElement(el) {\n"
    "    if (!el) return null;\n"
    "    if (el.classList.contains('btn-map')) return { el, kind:'button', bit:parseInt(el.dataset.btn) };\n"
    "    if (el.classList.contains('btn-dpad')) return { el, kind:'dpad', dir:el.dataset.dir };\n"
    "    return null;\n"
    "}\n"
    "function sameControl(a, b) {\n"
    "    return !!a && !!b && a.el === b.el && a.kind === b.kind && a.bit === b.bit && a.dir === b.dir;\n"
    "}\n"
    "function pointInRect(x, y, r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }\n"
    "function pointInElementId(id, x, y) {\n"
    "    const el = document.getElementById(id);\n"
    "    return !!el && el.offsetParent !== null && pointInRect(x, y, el.getBoundingClientRect());\n"
    "}\n"
    "function getControlAt(x, y) {\n"
    "    const stack = document.elementsFromPoint ? document.elementsFromPoint(x, y) : [];\n"
    "    for (const node of stack) {\n"
    "        const el = node.closest && node.closest('.btn-map,.btn-dpad');\n"
    "        if (el) return controlForElement(el);\n"
    "    }\n"
    "    if (pointInElementId('lstick', x, y) || pointInElementId('rstick', x, y)) return null;\n"
    "\n"
    "    // Forgiving fallback: treat the whole visible button area, plus a small\n"
    "    // finger-radius margin, as pressable. This avoids the \"only the exact\n"
    "    // middle works\" feeling on iPhone Safari.\n"
    "    const hitSlop = Math.max(14, Math.min(window.innerWidth, window.innerHeight) * 0.025);\n"
    "    let best = null, bestScore = Infinity;\n"
    "    for (const el of buttonControls) {\n"
    "        if (el.offsetParent === null) continue;\n"
    "        const r = el.getBoundingClientRect();\n"
    "        if (x < r.left - hitSlop || x > r.right + hitSlop || y < r.top - hitSlop || y > r.bottom + hitSlop) continue;\n"
    "        const cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;\n"
    "        const score = (x - cx) * (x - cx) + (y - cy) * (y - cy);\n"
    "        if (score < bestScore) { bestScore = score; best = controlForElement(el); }\n"
    "    }\n"
    "    return best;\n"
    "}\n"
    "function recomputeTouchControls() {\n"
    "    state.buttons &= ~allTouchButtonBits;\n"
    "    dpad.u = dpad.d = dpad.l = dpad.r = false;\n"
    "    buttonControls.forEach(el => el.classList.remove('active'));\n"
    "    for (const info of activeTouchControls.values()) {\n"
    "        if (!info) continue;\n"
    "        info.el.classList.add('active');\n"
    "        if (info.kind === 'button') state.buttons |= info.bit;\n"
    "        else if (info.kind === 'dpad') dpad[info.dir] = true;\n"
    "    }\n"
    "    updateHat();\n"
    "}\n"
    "function pressControl(touchId, info) {\n"
    "    if (!info) return false;\n"
    "    const prev = activeTouchControls.get(touchId);\n"
    "    if (sameControl(prev, info)) return true;\n"
    "    activeTouchControls.set(touchId, info);\n"
    "    recomputeTouchControls();\n"
    "    return true;\n"
    "}\n"
    "function releaseControl(touchId) {\n"
    "    if (!activeTouchControls.has(touchId)) return false;\n"
    "    activeTouchControls.delete(touchId);\n"
    "    recomputeTouchControls();\n"
    "    return true;\n"
    "}\n"
    "const padSurface = document.getElementById('gamepad');\n"
    "padSurface.addEventListener('touchstart', e => {\n"
    "    let handled = false;\n"
    "    for (const t of e.changedTouches) {\n"
    "        const info = getControlAt(t.clientX, t.clientY);\n"
    "        if (info) handled = pressControl(t.identifier, info) || handled;\n"
    "    }\n"
    "    if (handled) e.preventDefault();\n"
    "}, {passive:false});\n"
    "padSurface.addEventListener('touchmove', e => {\n"
    "    let handled = false;\n"
    "    for (const t of e.changedTouches) {\n"
    "        if (!activeTouchControls.has(t.identifier)) continue;\n"
    "        const info = getControlAt(t.clientX, t.clientY);\n"
    "        const prev = activeTouchControls.get(t.identifier);\n"
    "        if (info && sameControl(prev, info)) { handled = true; continue; }\n"
    "        if (info) pressControl(t.identifier, info);\n"
    "        else releaseControl(t.identifier);\n"
    "        handled = true;\n"
    "    }\n"
    "    if (handled) e.preventDefault();\n"
    "}, {passive:false});\n"
    "padSurface.addEventListener('touchend', e => {\n"
    "    let handled = false;\n"
    "    for (const t of e.changedTouches) handled = releaseControl(t.identifier) || handled;\n"
    "    if (handled) e.preventDefault();\n"
    "}, {passive:false});\n"
    "padSurface.addEventListener('touchcancel', e => {\n"
    "    let handled = false;\n"
    "    for (const t of e.changedTouches) handled = releaseControl(t.identifier) || handled;\n"
    "    if (handled) e.preventDefault();\n"
    "}, {passive:false});\n"
    "function clamp16(v) { v = Math.round(v || 0); return Math.max(-32768, Math.min(32767, v)); }\n"
    "function motionWarn(msg) { console.warn('[motion]', msg); }\n"
    "async function requestSensorPermission(apiName) {\n"
    "    const Api = window[apiName];\n"
    "    if (typeof Api === 'undefined') return 'unavailable';\n"
    "    if (typeof Api.requestPermission === 'function') {\n"
    "        try { return await Api.requestPermission(); } catch(e) { motionWarn(`${apiName} permission error: ${e && e.message ? e.message : e}`); return 'error'; }\n"
    "    }\n"
    "    return 'granted';\n"
    "}\n"
    "function installDeviceMotionListener() {\n"
    "    if (motionListenerInstalled || typeof DeviceMotionEvent === 'undefined') return;\n"
    "    motionListenerInstalled = true;\n"
    "    window.addEventListener('devicemotion', e => {\n"
    "        lastDeviceMotionMs = Date.now();\n"
    "        const a = e.accelerationIncludingGravity || e.acceleration || {};\n"
    "        const rr = e.rotationRate || {};\n"
    "        motion.enabled = true;\n"
    "        motion.ax = clamp16((a.x || 0) / 9.80665 * 4096);\n"
    "        motion.ay = clamp16((a.y || 0) / 9.80665 * 4096);\n"
    "        motion.az = clamp16((a.z || 0) / 9.80665 * 4096);\n"
    "        motion.gx = clamp16((rr.beta  || 0) * 16);\n"
    "        motion.gy = clamp16((rr.gamma || 0) * 16);\n"
    "        motion.gz = clamp16((rr.alpha || 0) * 16);\n"
    "    }, {passive:true});\n"
    "}\n"
    "function installDeviceOrientationFallback() {\n"
    "    if (orientationListenerInstalled || typeof DeviceOrientationEvent === 'undefined') return;\n"
    "    orientationListenerInstalled = true;\n"
    "    window.addEventListener('deviceorientation', e => {\n"
    "        if (Date.now() - lastDeviceMotionMs < 250) return;\n"
    "        motion.enabled = true;\n"
    "        motion.gx = clamp16((e.beta  || 0) * 16);\n"
    "        motion.gy = clamp16((e.gamma || 0) * 16);\n"
    "        motion.gz = clamp16((e.alpha || 0) * 16);\n"
    "    }, {passive:true});\n"
    "}\n"
    "async function enableMotion() {\n"
    "    const motionPerm = await requestSensorPermission('DeviceMotionEvent');\n"
    "    const orientPerm = await requestSensorPermission('DeviceOrientationEvent');\n"
    "    // Best-effort on both HTTP and HTTPS.  If a browser allows sensors on HTTP,\n"
    "    // this will use them; if it does not, the events simply will not fire.\n"
    "    if (motionPerm !== 'denied') installDeviceMotionListener();\n"
    "    if (orientPerm !== 'denied') installDeviceOrientationFallback();\n"
    "}\n"
    "function clamp01(v) { return Math.max(0.0, Math.min(1.0, v)); }\n"
    "function handleRumbleMessage(data) {\n"
    "    if (!(data instanceof ArrayBuffer) || data.byteLength < 8) return;\n"
    "    const v = new DataView(data); if (v.getUint32(0, true) !== RUMBLE_MAGIC) return;\n"
    "    const low255 = v.getUint8(5), high255 = v.getUint8(6);\n"
    "    const isNeutral = (!low255 && !high255);\n"
    "    const weakFloat = clamp01(low255 / 255.0), strongFloat = clamp01(high255 / 255.0);\n"
    "    const dur = isNeutral ? 10 : 250;\n"
    "    const now = Date.now();\n"
    "    const magKey = weakFloat + '_' + strongFloat;\n"
    "    if (magKey === mobileRumbleLastMag && (now - mobileRumbleDebounceTime < 100)) return;\n"
    "    mobileRumbleLastMag = magKey; mobileRumbleDebounceTime = now;\n"
    "    const pulse = document.getElementById('rumblePulse');\n"
    "    if (pulse) { pulse.style.display = isNeutral ? 'none' : 'block'; if (!isNeutral) { pulse.classList.add('active'); setTimeout(() => pulse.classList.remove('active'), 220); } }\n"
    "    if (navigator.vibrate) {\n"
    "        try { if (isNeutral) navigator.vibrate(0); else navigator.vibrate([dur]); }\n"
    "        catch (err) { console.error('Browser blocked mobile vibration:', err); }\n"
    "    }\n"
    "}\n"
    "function makeWsUrl() {\n"
    "    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';\n"
    "    return `${proto}//${window.location.host}/`;\n"
    "}\n"
    "function setupJoystick(baseId, knobId, axisX, axisY) {\n"
    "    const base = document.getElementById(baseId), knob = document.getElementById(knobId);\n"
    "    if(!base) return;\n"
    "    let activeTouch = null;\n"
    "    const reset = () => { state[axisX] = 128; state[axisY] = 128; knob.style.transform = `translate(0px, 0px)`; activeTouch = null; };\n"
    "    base.addEventListener('touchstart', e => { e.preventDefault(); activeTouch = e.changedTouches[0].identifier; updateJoy(e.changedTouches[0]); }, {passive:false});\n"
    "    base.addEventListener('touchmove', e => {\n"
    "        e.preventDefault();\n"
    "        for (let i=0; i<e.changedTouches.length; i++) if (e.changedTouches[i].identifier === activeTouch) updateJoy(e.changedTouches[i]);\n"
    "    }, {passive:false});\n"
    "    base.addEventListener('touchend', e => { for (let i=0; i<e.changedTouches.length; i++) if (e.changedTouches[i].identifier === activeTouch) reset(); }, {passive:false});\n"
    "    base.addEventListener('touchcancel', e => { for (let i=0; i<e.changedTouches.length; i++) if (e.changedTouches[i].identifier === activeTouch) reset(); }, {passive:false});\n"
    "    function updateJoy(t) {\n"
    "        const rect = base.getBoundingClientRect(); const maxDist = rect.width / 2;\n"
    "        let dx = t.clientX - (rect.left + rect.width/2), dy = t.clientY - (rect.top + rect.height/2);\n"
    "        let dist = Math.sqrt(dx*dx + dy*dy);\n"
    "        if (dist > maxDist) { dx = (dx/dist)*maxDist; dy = (dy/dist)*maxDist; }\n"
    "        knob.style.transform = `translate(${dx}px, ${dy}px)`;\n"
    "        state[axisX] = Math.round(((dx / maxDist) + 1) * 127.5);\n"
    "        state[axisY] = Math.round(((dy / maxDist) + 1) * 127.5);\n"
    "    }\n"
    "}\n"
    "setupJoystick('lstick', 'lknob', 'lx', 'ly');\n"
    "setupJoystick('rstick', 'rknob', 'rx', 'ry');\n"
    "function normalizeSystemShortcuts(buttons) {\n"
    "    const captureCombo = (buttons & BTN_MINUS) && (buttons & BTN_PLUS);\n"
    "    const homeCombo = (buttons & BTN_LSTICK) && (buttons & BTN_RSTICK);\n"
    "    if (captureCombo) {\n"
    "        buttons |= BTN_CAPTURE;\n"
    "        buttons &= ~(BTN_MINUS | BTN_PLUS | BTN_HOME);\n"
    "        if (homeCombo) buttons &= ~(BTN_LSTICK | BTN_RSTICK);\n"
    "    } else if (homeCombo) {\n"
    "        buttons |= BTN_HOME;\n"
    "        buttons &= ~(BTN_LSTICK | BTN_RSTICK | BTN_CAPTURE);\n"
    "    }\n"
    "    return buttons;\n"
    "}\n"
    "function sendPacket() {\n"
    "    if (!ws || ws.readyState !== WebSocket.OPEN) return;\n"
    "    const buffer = new ArrayBuffer(PACKET_SIZE), view = new DataView(buffer);\n"
    "    view.setUint32(0, PROTO_MAGIC, true); view.setUint8(4, PROTO_VERSION); view.setUint8(5, FLAG_SINGLE_PAD);\n"
    "    view.setUint16(6, 0, true); view.setUint32(8, seqCounter++, true); view.setBigUint64(12, BigInt(Date.now()*1000), true);\n"
    "    let off = 20;\n"
    "    const sendButtons = normalizeSystemShortcuts(state.buttons);\n"
    "    view.setUint16(off, sendButtons, true); view.setUint8(off+2, state.hat);\n"
    "    view.setUint8(off+3, state.lx); view.setUint8(off+4, state.ly); view.setUint8(off+5, state.rx); view.setUint8(off+6, state.ry); view.setUint8(off+7, PAD_PRESENT);\n"
    "    view.setInt16(off+8, motion.ax, true); view.setInt16(off+10, motion.ay, true); view.setInt16(off+12, motion.az, true);\n"
    "    view.setInt16(off+14, motion.gx, true); view.setInt16(off+16, motion.gy, true); view.setInt16(off+18, motion.gz, true);\n"
    "    view.setUint8(off+20, motion.enabled ? 1 : 0); view.setUint8(off+21, 0); view.setUint8(off+22, 0); view.setUint8(off+23, 0);\n"
    "    for(let p=1; p<4; p++) {\n"
    "        off = 20 + (p*EXT_REPORT_SIZE); view.setUint16(off, 0, true); view.setUint8(off+2, 8);\n"
    "        view.setUint8(off+3, 128); view.setUint8(off+4, 128); view.setUint8(off+5, 128); view.setUint8(off+6, 128); view.setUint8(off+7, 0);\n"
    "        for(let k=8; k<EXT_REPORT_SIZE; k++) view.setUint8(off+k, 0);\n"
    "    }\n"
    "    ws.send(buffer);\n"
    "}\n"
    "document.getElementById('btnConnect').onclick = async () => {\n"
    "    if (isConnected) { ws.close(); return; }\n"
    "    await enableMotion();\n"
    "    if (document.documentElement.requestFullscreen) { document.documentElement.requestFullscreen().catch(()=>{}); }\n"
    "    const wsUrl = makeWsUrl();\n"
    "    ws = new WebSocket(wsUrl); ws.binaryType = \"arraybuffer\";\n"
    "    ws.onmessage = (ev) => handleRumbleMessage(ev.data);\n"
    "    ws.onopen = () => {\n"
    "        isConnected = true; const btn = document.getElementById('btnConnect');\n"
    "        btn.innerText = \"Connected\"; btn.classList.add('connected');\n"
    "        connectTimeout = setTimeout(() => { btn.style.display = 'none'; document.getElementById('statusDot').style.display = 'block'; }, 2000);\n"
    "        loopId = setInterval(sendPacket, 4);\n"
    "    };\n"
    "    ws.onerror = () => alert(\"Connection failed!\");\n"
    "    ws.onclose = () => {\n"
    "        isConnected = false; clearInterval(loopId); clearTimeout(connectTimeout);\n"
    "        const btn = document.getElementById('btnConnect');\n"
    "        btn.style.display = 'block'; btn.innerText = \"Connect\"; btn.classList.remove('connected');\n"
    "        document.getElementById('statusDot').style.display = 'none';\n"
    "    };\n"
    "};\n"
    "document.getElementById('statusDot').onclick = () => { if(ws) ws.close(); };\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static const char EDITOR_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <title>Layout Editor</title>\n"
    "%s" // MOBILE_STYLE_AND_DOM
    "    <style>\n"
    "        .overlap { border: 2px solid red !important; box-shadow: 0 0 15px red !important; z-index: 1000; }\n"
    "        #editor-bar {\n"
    "            position: fixed; top: 50%; left: 50%; transform: translate(-50%, -50%);\n"
    "            background: rgba(0,0,0,0.85); border-radius: 12px; display: flex; flex-direction: column;\n"
    "            z-index: 10000; box-shadow: 0 10px 30px rgba(0,0,0,0.8); border: 1px solid #444; width: 300px;\n"
    "            max-height: 90vh; overflow-y: auto;\n"
    "        }\n"
    "        #editor-bar button, #editor-bar select { padding: 10px; font-size: 16px; border-radius: 6px; border: none; font-weight: bold; cursor: pointer; }\n"
    "        #toggleDel { background: #555; color: white; transition: 0.2s; }\n"
    "        #toggleDel.active { background: #e33; }\n"
    "    </style>\n"
    "    <div id=\"rotate-msg\">Please rotate to landscape mode.</div>\n"
    "    <div id=\"gamepad\">\n"
    "        <div id=\"menu-toggle\" onclick=\"toggleMenu()\" style=\"position:absolute; top:50%; left:50%; transform:translate(-50%,-50%); width:50px; height:50px; background:rgba(255,255,255,0.2); border-radius:50%; display:none; justify-content:center; align-items:center; z-index:9999; font-size:24px; box-shadow: 0 0 10px rgba(0,0,0,0.5); border: 1px solid rgba(255,255,255,0.5);\">⚙️</div>\n"
    "        <div id=\"editor-bar\">\n"
    "            <div id=\"eb-header\" style=\"padding: 10px; background: #333; border-radius: 12px 12px 0 0; display: flex; justify-content: space-between; align-items: center; cursor: move;\">\n"
    "                <span style=\"font-weight:bold; font-size:14px; color: white;\">Editor Settings</span>\n"
    "                <button onclick=\"toggleMenu()\" style=\"background:none; border:none; color:white; font-size:22px; padding:10px; margin-right:-5px; cursor:pointer; line-height:1;\">❌</button>\n"
    "            </div>\n"
    "            <div style=\"padding: 15px; display: flex; flex-direction: column; gap: 10px;\">\n"
    "                <div style=\"color:#fff; text-align:center; font-size:14px; margin-bottom:5px;\">Drag to Move, Pinch to Resize</div>\n"
    "                <button onclick=\"saveLayout()\" style=\"background:#0a0; color:#fff;\">Save</button>\n"
    "                <select id=\"addSel\" onchange=\"addBtn()\"><option value=\"\">+ Add Button</option></select>\n"
    "                <button id=\"toggleDel\" onclick=\"toggleDel()\">🗑️ Delete Mode: OFF</button>\n"
    "                <button onclick=\"window.location.href='/mobile'\" style=\"background:#a00; color:#fff;\">Cancel</button>\n"
    "                <button onclick=\"resetLayout()\" style=\"background:#e91e63; color:#fff;\">Reset to Defaults</button>\n"
    "            </div>\n"
    "        </div>\n"
    "        <div id=\"btn-zl\" class=\"edit-item btn-shoulder btn\">ZL</div>\n"
    "        <div id=\"btn-l\" class=\"edit-item btn-shoulder btn\">L</div>\n"
    "        <div id=\"btn-zr\" class=\"edit-item btn-shoulder btn\">ZR</div>\n"
    "        <div id=\"btn-r\" class=\"edit-item btn-shoulder btn\">R</div>\n"
    "        <div id=\"btn-minus\" class=\"edit-item btn-sys btn\">-</div>\n"
    "        <div id=\"btn-plus\" class=\"edit-item btn-sys btn\">+</div>\n"
    "        <div id=\"btn-capture\" class=\"edit-item btn-sys btn\">O</div>\n"
    "        <div id=\"btn-home\" class=\"edit-item btn-sys btn\">⌂</div>\n"
    "        <div id=\"lstick\" class=\"edit-item joystick\"><div class=\"knob\"></div></div>\n"
    "        <div id=\"btn-ls\" class=\"edit-item btn-sys btn\">LS</div>\n"
    "        <div id=\"dpad\" class=\"edit-item cross\">\n"
    "            <div class=\"btn btn-circle up\">&#x25B2;&#xFE0E;</div><div class=\"btn btn-circle left\">&#x25C0;&#xFE0E;</div>\n"
    "            <div class=\"btn btn-circle right\">&#x25B6;&#xFE0E;</div><div class=\"btn btn-circle down\">&#x25BC;&#xFE0E;</div>\n"
    "        </div>\n"
    "        <div id=\"abxy\" class=\"edit-item cross\">\n"
    "            <div class=\"btn btn-circle up\">X</div><div class=\"btn btn-circle left\">Y</div>\n"
    "            <div class=\"btn btn-circle right\">A</div><div class=\"btn btn-circle down\">B</div>\n"
    "        </div>\n"
    "        <div id=\"rstick\" class=\"edit-item joystick\"><div class=\"knob\"></div></div>\n"
    "        <div id=\"btn-rs\" class=\"edit-item btn-sys btn\">RS</div>\n"
    "    </div>\n"
    "<script>\n"
    "const defLayout = {\n"
    "    'btn-zl': {l:4, t:4, w:14, h:8}, 'btn-l': {l:4, t:14, w:14, h:8},\n"
    "    'btn-zr': {l:82, t:4, w:14, h:8}, 'btn-r': {l:82, t:14, w:14, h:8},\n"
    "    'btn-minus': {l:38, t:5, w:6}, 'btn-plus': {l:56, t:5, w:6},\n"
    "    'btn-capture': {l:42, t:18, w:5}, 'btn-home': {l:53, t:18, w:5},\n"
    "    'lstick': {l:6, t:35, w:16}, 'btn-ls': {l:2, t:65, w:5},\n"
    "    'dpad': {l:22, t:60, w:16},\n"
    "    'abxy': {l:78, t:35, w:16},\n"
    "    'rstick': {l:62, t:60, w:16}, 'btn-rs': {l:85, t:80, w:5}\n"
    "};\n"
    "let layout = JSON.parse(localStorage.getItem('nswc_layout')) || defLayout;\n"
    "function applyLayout() {\n"
    "    for(let id in defLayout) {\n"
    "        let el = document.getElementById(id);\n"
    "        if(!el) continue;\n"
    "        let conf = layout[id] || defLayout[id];\n"
    "        if(conf.hide) { el.style.display = 'none'; }\n"
    "        else {\n"
    "            if(id === 'dpad' || id === 'abxy') el.style.display = 'grid';\n"
    "            else el.style.display = 'flex';\n"
    "            el.style.left = conf.l + '%'; el.style.top = conf.t + '%'; el.style.width = conf.w + '%';\n"
    "            if(conf.h) el.style.height = conf.h + '%';\n"
    "            else { el.style.aspectRatio = '1 / 1'; el.style.height = 'auto'; }\n"
    "        }\n"
    "    }\n"
    "}\n"
    "applyLayout();\n"
    "function populateAdd() {\n"
    "    let sel = document.getElementById('addSel'); sel.innerHTML = '<option value=\"\">+ Add Button</option>';\n"
    "    for(let id in defLayout) {\n"
    "        let conf = layout[id] || defLayout[id];\n"
    "        if(conf.hide) { let opt = document.createElement('option'); opt.value = id; opt.innerText = id; sel.appendChild(opt); }\n"
    "    }\n"
    "}\n"
    "populateAdd();\n"
    "function addBtn() {\n"
    "    let val = document.getElementById('addSel').value;\n"
    "    if(!val) return;\n"
    "    if(!layout[val]) layout[val] = {...defLayout[val]};\n"
    "    layout[val].hide = false; layout[val].l = 45; layout[val].t = 15;\n"
    "    applyLayout(); populateAdd(); checkOverlaps();\n"
    "}\n"
    "function checkOverlaps() {\n"
    "    let els = Array.from(document.querySelectorAll('.edit-item')).filter(e => e.style.display !== 'none');\n"
    "    els.forEach(e => e.classList.remove('overlap'));\n"
    "    let hasOverlap = false;\n"
    "    for(let i=0; i<els.length; i++) {\n"
    "        for(let j=i+1; j<els.length; j++) {\n"
    "            let b1 = els[i].getBoundingClientRect(), b2 = els[j].getBoundingClientRect();\n"
    "            let s1x = b1.width * 0.1, s1y = b1.height * 0.1;\n"
    "            let s2x = b2.width * 0.1, s2y = b2.height * 0.1;\n"
    "            if (!(b1.right - s1x < b2.left + s2x || b1.left + s1x > b2.right - s2x || b1.bottom - s1y < b2.top + s2y || b1.top + s1y > b2.bottom - s2y)) {\n"
    "                els[i].classList.add('overlap'); els[j].classList.add('overlap'); hasOverlap = true;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    return hasOverlap;\n"
    "}\n"
    "function toggleMenu() {\n"
    "    let eb = document.getElementById('editor-bar'); let tg = document.getElementById('menu-toggle');\n"
    "    if(eb.style.display === 'none') { eb.style.display = 'flex'; tg.style.display = 'none'; }\n"
    "    else { eb.style.display = 'none'; tg.style.display = 'flex'; }\n"
    "}\n"
    "let eb = document.getElementById('editor-bar');\n"
    "let isDraggingMenu = false, menuDx, menuDy;\n"
    "eb.addEventListener('touchstart', e => {\n"
    "    if (e.target.closest('button, select, input')) return;\n"
    "    isDraggingMenu = true; let rect = eb.getBoundingClientRect();\n"
    "    menuDx = e.touches[0].clientX - (rect.left + rect.width/2);\n"
    "    menuDy = e.touches[0].clientY - (rect.top + rect.height/2);\n"
    "    e.stopPropagation();\n"
    "}, {passive:false});\n"
    "window.addEventListener('touchmove', e => {\n"
    "    if(isDraggingMenu) {\n"
    "        e.preventDefault();\n"
    "        eb.style.left = (e.touches[0].clientX - menuDx) + 'px';\n"
    "        eb.style.top = (e.touches[0].clientY - menuDy) + 'px';\n"
    "    }\n"
    "}, {passive:false});\n"
    "window.addEventListener('touchend', e => { isDraggingMenu = false; });\n"
    "let activeEl = null, mode = 'none', startX, startY, startL, startT, initialDist, startW, startH, deleteMode = false;\n"
    "function toggleDel() {\n"
    "    deleteMode = !deleteMode; const btn = document.getElementById('toggleDel');\n"
    "    if(deleteMode) { btn.classList.add('active'); btn.innerText = \"🗑️ Delete Mode: ON\"; }\n"
    "    else { btn.classList.remove('active'); btn.innerText = \"🗑️ Delete Mode: OFF\"; }\n"
    "}\n"
    "function getDist(t1, t2) { return Math.hypot(t1.clientX - t2.clientX, t1.clientY - t2.clientY); }\n"
    "document.getElementById('gamepad').addEventListener('touchstart', e => {\n"
    "    if(e.target.closest('#editor-bar') || e.target.closest('#menu-toggle')) return;\n"
    "    let el = e.target.closest('.edit-item'); if(!el) return;\n"
    "    e.preventDefault();\n"
    "    if (deleteMode) {\n"
    "        if(!layout[el.id]) layout[el.id] = {...defLayout[el.id]};\n"
    "        layout[el.id].hide = true; applyLayout(); populateAdd(); checkOverlaps(); return;\n"
    "    }\n"
    "    activeEl = el; if(!layout[el.id]) layout[el.id] = {...defLayout[el.id]};\n"
    "    if(e.touches.length === 1) {\n"
    "        mode = 'drag'; startX = e.touches[0].clientX; startY = e.touches[0].clientY;\n"
    "        startL = layout[el.id].l; startT = layout[el.id].t;\n"
    "    } else if(e.touches.length === 2) {\n"
    "        mode = 'pinch'; initialDist = getDist(e.touches[0], e.touches[1]);\n"
    "        startW = layout[el.id].w; startH = layout[el.id].h;\n"
    "    }\n"
    "}, {passive:false});\n"
    "document.getElementById('gamepad').addEventListener('touchmove', e => {\n"
    "    if(!activeEl || deleteMode) return; e.preventDefault();\n"
    "    let pw = window.innerWidth, ph = window.innerHeight, conf = layout[activeEl.id];\n"
    "    if(mode === 'drag' && e.touches.length === 1) {\n"
    "        let dx = e.touches[0].clientX - startX, dy = e.touches[0].clientY - startY;\n"
    "        conf.l = startL + (dx / pw * 100); conf.t = startT + (dy / ph * 100);\n"
    "    } else if(mode === 'pinch' && e.touches.length >= 2) {\n"
    "        let scale = getDist(e.touches[0], e.touches[1]) / initialDist;\n"
    "        conf.w = Math.max(2, startW * scale); if(startH) conf.h = Math.max(2, startH * scale);\n"
    "    }\n"
    "    applyLayout(); checkOverlaps();\n"
    "}, {passive:false});\n"
    "document.getElementById('gamepad').addEventListener('touchend', e => {\n"
    "    if(e.touches.length === 0) { activeEl = null; mode = 'none'; }\n"
    "    else if(e.touches.length === 1 && activeEl) {\n"
    "        mode = 'drag'; startX = e.touches[0].clientX; startY = e.touches[0].clientY;\n"
    "        startL = layout[activeEl.id].l; startT = layout[activeEl.id].t;\n"
    "    }\n"
    "});\n"
    "setTimeout(checkOverlaps, 500);\n"
    "function saveLayout() {\n"
    "    if(checkOverlaps()) { alert('Fix overlapping buttons (red) before saving!'); return; }\n"
    "    localStorage.setItem('nswc_layout', JSON.stringify(layout));\n"
    "    window.location.href = '/mobile';\n"
    "}\n"
    "function resetLayout() {\n"
    "    if(confirm('Are you sure you want to reset all buttons to their default positions and sizes?')) {\n"
    "        layout = JSON.parse(JSON.stringify(defLayout));\n"
    "        applyLayout();\n"
    "        populateAdd();\n"
    "        checkOverlaps();\n"
    "    }\n"
    "}\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";


// ── Minimal SHA-1 (for WebSocket handshake) ───────────────────────────────────
struct Sha1Ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
};

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (block[i*4]<<24) | (block[i*4+1]<<16) | (block[i*4+2]<<8) | block[i*4+3];
    for (int i = 16; i < 80; i++) {
        uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (t << 1) | (t >> 31);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)       { f = (b & c) | (~b & d);       k = 0x5A827999; }
        else if (i < 40)  { f = b ^ c ^ d;                k = 0x6ED9EBA1; }
        else if (i < 60)  { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;                k = 0xCA62C1D6; }
        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1_init(Sha1Ctx *ctx) {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE; ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0; ctx->count = 0;
}

static void sha1_update(Sha1Ctx *ctx, const uint8_t *data, size_t len) {
    size_t idx = ctx->count & 63;
    ctx->count += len;
    while (len--) {
        ctx->buffer[idx++] = *data++;
        if (idx == 64) { sha1_transform(ctx->state, ctx->buffer); idx = 0; }
    }
}

static void sha1_final(Sha1Ctx *ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = ctx->count & 63;
    size_t pad = (idx < 56) ? (56 - idx) : (120 - idx);
    uint8_t padding[64];
    memset(padding, 0, pad);
    padding[0] = 0x80;
    sha1_update(ctx, padding, pad);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) len_bytes[7-i] = (bits >> (i*8)) & 0xFF;
    sha1_update(ctx, len_bytes, 8);
    for (int i = 0; i < 5; i++) {
        digest[i*4]   = (ctx->state[i] >> 24) & 0xFF;
        digest[i*4+1] = (ctx->state[i] >> 16) & 0xFF;
        digest[i*4+2] = (ctx->state[i] >> 8) & 0xFF;
        digest[i*4+3] = ctx->state[i] & 0xFF;
    }
}


// ── Single-threaded WebSocket/HTTP client state ─────────────────────────────
static constexpr int MAX_WS_CLIENTS = 32;

struct WebClient {
    int fd = -1;

    // Read buffer
    uint8_t buf[65536];
    size_t fill = 0;

    enum State : uint8_t {
        READ_HTTP,   // reading HTTP request headers
        WRITE_RESP,  // writing HTTP / WS upgrade response (POLLOUT)
        WS_ACTIVE,   // WebSocket mode (frames)
        CLOSED
    } state = CLOSED;

                // HTTP headers (null-terminated)
    char http_buf[8192];
    size_t http_len = 0;
    uint64_t connect_time = 0;
    uint32_t ip = 0;

    // WS session
    int      ws_slot = -1;
    uint32_t ws_seq = 0;
    bool     ws_first = true;
    uint64_t ws_last_rx = 0;
    uint32_t last_rumble_seq[4] = {};

    // Async write queue (heap-allocated, freed after write completes or on cleanup)
    uint8_t *wbuf = nullptr;
    size_t   wlen = 0;
    size_t   woff = 0;
    State    after_write = CLOSED;
};

static void legacy_multi_to_extended(const MultiReport& in, ExtendedMultiReport& out) {
    out.reset();
    out.p1.input = in.p1;
    out.p2.input = in.p2;
    out.p3.input = in.p3;
    out.p4.input = in.p4;
}

// UDP extended packets keep the old UDP packet untouched, but add motion/gyro
// and an opt-in rumble return path.  Layout is the same 20-byte header used by
// Packet/WebSocket, followed by ExtendedMultiReport and a normal HMAC tag.
// Old clients still send the legacy Packet size/version and are handled below.
struct NS_LOCAL_PACKED ExtendedUdpPacket {
    uint32_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t seq;
    uint64_t timestamp_us;
    ExtendedMultiReport report;
    uint8_t  hmac[HMAC_TAG_SIZE];
};

static constexpr size_t EXT_UDP_PACKET_AUTH_SIZE = 20 + sizeof(ExtendedMultiReport);
static constexpr size_t EXT_UDP_PACKET_SIZE      = EXT_UDP_PACKET_AUTH_SIZE + HMAC_TAG_SIZE;
static constexpr size_t UDP_RX_MAX_PACKET_SIZE   =
    sizeof(ExtendedUdpPacket) > sizeof(Packet) ? sizeof(ExtendedUdpPacket) : sizeof(Packet);
static_assert(sizeof(ExtendedUdpPacket) == EXT_UDP_PACKET_SIZE,
              "ExtendedUdpPacket size must match its wire format");

static bool extended_udp_packet_ok(const ExtendedUdpPacket& p) {
    return p.magic == PROTO_MAGIC &&
           (p.version == WEB_PROTO_VERSION || p.version == PROTO_VERSION);
}

static bool extended_report_pad_present(const ExtendedMultiReport& report, int subpad) {
    if (subpad < 0 || subpad >= 4) return false;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&report);
    return (raw[subpad * sizeof(ExtendedHIDReport) + 7] & 0x01) != 0;
}

static void clear_udp_rumble_state(ClientSession& c) {
    c.udp_rumble_enabled = false;
    for (int s = 0; s < 4; ++s)
        c.udp_last_rumble_seq[s] = c.rumble_seq[s];
}

static void enable_udp_rumble_state(ClientSession& c) {
    if (!c.udp_rumble_enabled) {
        c.udp_rumble_enabled = true;
        for (int s = 0; s < 4; ++s)
            c.udp_last_rumble_seq[s] = c.rumble_seq[s];
    }
}

static void flush_rumble_to_udp(int sock, int client_idx) {
    if (sock < 0 || client_idx < 0 || client_idx >= MAX_CLIENTS) return;

    sockaddr_in dest{};
    RumblePacket pending[4]{};
    bool has[4]{};

    {
        std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
        ClientSession& c = g_clients[client_idx];
        if (!c.active || !c.udp_rumble_enabled) return;

        dest = c.addr;
        for (int s = 0; s < 4; ++s) {
            uint32_t seq = c.rumble_seq[s];
            if (seq != c.udp_last_rumble_seq[s]) {
                pending[s] = c.rumble[s];
                c.udp_last_rumble_seq[s] = seq;
                has[s] = true;
            }
        }
    }

    for (int s = 0; s < 4; ++s) {
        if (!has[s]) continue;
        ssize_t sent = sendto(sock, &pending[s], sizeof(RumblePacket), 0,
                              reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (g_verbose && sent != (ssize_t)sizeof(RumblePacket))
            std::fprintf(stderr, "[udp] failed to send rumble packet: %s\n", std::strerror(errno));
    }
}

static bool send_ws_binary_frame(WebClient* c, const uint8_t* payload, size_t len) {
    if (!c || c->state != WebClient::WS_ACTIVE || c->fd < 0) return false;
    if (c->wbuf != nullptr) return false; // previous WS frame still draining
    if (len >= 126) return false;

    const size_t hdr = 2;
    const size_t total = hdr + len;
    uint8_t small_frame[2 + sizeof(RumblePacket)] = {};
    if (total > sizeof(small_frame)) return false;

    small_frame[0] = 0x82; // FIN + binary
    small_frame[1] = (uint8_t)len;
    memcpy(small_frame + hdr, payload, len);

    ssize_t w = write(c->fd, small_frame, total);
    if (w == (ssize_t)total) return true;

    size_t written = 0;
    if (w < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
    } else if (w > 0) {
        written = (size_t)w;
    }

    // Non-blocking socket is full or only accepted part of the frame. Queue the
    // remaining bytes and let the poll loop finish it before sending another
    // rumble frame. This prevents silently dropping rumble on busy web/mobile clients.
    c->wbuf = (uint8_t*)malloc(total);
    if (!c->wbuf) return false;
    memcpy(c->wbuf, small_frame, total);
    c->wlen = total;
    c->woff = written;
    c->after_write = WebClient::WS_ACTIVE;
    return true;
}

static void flush_rumble_to_ws(WebClient* c) {
    if (!c || c->state != WebClient::WS_ACTIVE || c->ws_slot < 0) return;

    RumblePacket pending[4]{};
    uint32_t seqs[4]{};
    bool has[4]{};

    {
        std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
        for (int s = 0; s < 4; ++s) {
            uint32_t seq = g_clients[c->ws_slot].rumble_seq[s];
            if (seq != c->last_rumble_seq[s]) {
                pending[s] = g_clients[c->ws_slot].rumble[s];
                seqs[s] = seq;
                has[s] = true;
            }
        }
    }

    for (int s = 0; s < 4; ++s) {
        if (!has[s]) continue;
        if (send_ws_binary_frame(c, (const uint8_t*)&pending[s], sizeof(RumblePacket)))
            c->last_rumble_seq[s] = seqs[s];
    }
}


// ── Process one complete WebSocket frame from client buffer ──────────────────
// Returns bytes consumed, or 0 if need more data.  Sets c->state = CLOSED on close/error.
static size_t process_ws_frame(WebClient *c) {
    uint8_t *buf = c->buf;
    size_t len  = c->fill;
    if (len < 2) return 0;

    int      opcode  = buf[0] & 0x0F;
    bool     masked  = buf[1] & 0x80;
    uint64_t flen    = buf[1] & 0x7F;
    size_t   hdr_sz  = 2;

    if (flen == 126) {
        if (len < 4) return 0;
        flen   = ((uint64_t)buf[2] << 8) | buf[3];
        hdr_sz = 4;
    } else if (flen == 127) {
        if (len < 10) return 0;
        flen = 0;
        for (int i = 0; i < 8; i++) flen = (flen << 8) | buf[2 + i];
        hdr_sz = 10;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (len < hdr_sz + 4) return 0;
        memcpy(mask, buf + hdr_sz, 4);
        hdr_sz += 4;
    }

    if (len < hdr_sz + flen) return 0;

    uint8_t *payload = buf + hdr_sz;
    size_t   total   = hdr_sz + flen;

    if (opcode == 9) { // ping → pong
        if (masked)
            for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];
        uint8_t pong[2] = {0x8A, 0x00};
        ssize_t _u = write(c->fd, pong, 2); (void)_u;
        return total;
    }
    if (opcode == 8) { // close
        uint8_t close_frame[] = {0x88, 0x00};
        ssize_t _u = write(c->fd, close_frame, 2); (void)_u;
        c->state = WebClient::CLOSED;
        return total;
    }

    if (opcode == 1) { // text control frame: MACRO_RUN:<json>
        if (masked)
            for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];
        std::string text(reinterpret_cast<char*>(payload), (size_t)flen);
        const std::string prefix = "MACRO_RUN:";
        if (text.rfind(prefix, 0) == 0) {
            uint64_t now = now_us();
            if (c->ws_slot < 0) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_mtx[i]);
                    if (!g_clients[i].active) {
                        c->ws_slot = i;
                        g_clients[i].active = true;
                        g_clients[i].first_pkt = true;
                        g_clients[i].report.reset();
                        clear_all_motion_history(g_clients[i]);
                        g_clients[i].uses_pad_presence = true;
                        clear_udp_rumble_state(g_clients[i]);
                        for (int s = 0; s < 4; ++s) { g_clients[i].pad_present[s] = false; g_clients[i].pad_last_present_us[s] = 0; g_clients[i].debug_rumble_sent[s] = false; }
                        g_clients[i].last_rx_us = now;
                        break;
                    }
                }
            }
            if (c->ws_slot >= 0) {
                {
                    std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
                    g_clients[c->ws_slot].active = true;
                    g_clients[c->ws_slot].uses_pad_presence = true;
                    g_clients[c->ws_slot].pad_present[0] = true;
                    g_clients[c->ws_slot].pad_last_present_us[0] = now;
                    g_clients[c->ws_slot].last_rx_us = now;
                }
                server_macro_start(c->ws_slot, 0, text.substr(prefix.size()));
            }
        }
        return total;
    }
    if (opcode == 0) {
        c->state = WebClient::CLOSED;
        return total;
    }
    if (opcode != 2) return total;

    if (masked)
        for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];

    if (flen >= MACRO_CHUNK_HEADER_SIZE) {
        uint32_t maybe_macro_magic = 0;
        memcpy(&maybe_macro_magic, payload, 4);
        if (maybe_macro_magic == MACRO_UDP_CHUNK_MAGIC) {
            uint64_t now = now_us();
            if (c->ws_slot < 0) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_mtx[i]);
                    if (!g_clients[i].active) {
                        c->ws_slot = i;
                        g_clients[i].active = true;
                        g_clients[i].first_pkt = true;
                        g_clients[i].report.reset();
                        clear_all_motion_history(g_clients[i]);
                        g_clients[i].uses_pad_presence = true;
                        g_clients[i].last_rx_us = now;
                        break;
                    }
                }
            }
            if (c->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
                g_clients[c->ws_slot].active = true;
                g_clients[c->ws_slot].uses_pad_presence = true;
                g_clients[c->ws_slot].pad_present[0] = true;
                g_clients[c->ws_slot].pad_last_present_us[0] = now;
                g_clients[c->ws_slot].last_rx_us = now;
            }
            if (c->ws_slot >= 0) server_macro_handle_ws_chunk_packet(c->ws_slot, payload, (size_t)flen);
            return total;
        }
    }

    if (flen != PACKET_SIZE && flen != WEB_PACKET_SIZE) {
        c->state = WebClient::CLOSED;
        return total;
    }

    uint32_t magic; memcpy(&magic, payload, 4);
    if (magic != PROTO_MAGIC) return total;
    uint8_t ver; memcpy(&ver, payload + 4, 1);
    uint8_t flags; memcpy(&flags, payload + 5, 1);
    bool is_reset = (flags & FLAG_RESET);
    uint32_t seq; memcpy(&seq, payload + 8, 4);

    ExtendedMultiReport report;
    report.reset();
    bool pad_present[4] = {};

    if (ver == PROTO_VERSION && flen == PACKET_SIZE) {
        MultiReport legacy;
        memcpy(&legacy, payload + 20, sizeof(MultiReport));
        legacy_multi_to_extended(legacy, report);
        pad_present[0] = !extended_is_neutral(report.p1);
        pad_present[1] = !extended_is_neutral(report.p2);
        pad_present[2] = !extended_is_neutral(report.p3);
        pad_present[3] = !extended_is_neutral(report.p4);
    } else if (ver == WEB_PROTO_VERSION && flen == WEB_PACKET_SIZE) {
        memcpy(&report, payload + 20, sizeof(ExtendedMultiReport));
        for (int s = 0; s < 4; ++s)
            pad_present[s] = (payload[20 + s * sizeof(ExtendedHIDReport) + 7] & 0x01) != 0;
        if (flags & FLAG_SINGLE_PAD) {
            // The mobile touch page is one virtual controller only.  Force the
            // unused subpads to neutral so a malformed/old cached mobile packet
            // cannot accidentally claim Switch ports 2-4.
            report.p2.reset();
            report.p3.reset();
            report.p4.reset();
            pad_present[0] = true;
            pad_present[1] = false;
            pad_present[2] = false;
            pad_present[3] = false;
        }
    } else {
        return total;
    }

    if (!c->ws_first && !is_reset && (int32_t)(seq - c->ws_seq) < 0) return total;
    c->ws_first = false;
    c->ws_seq = seq + 1;

    uint64_t now = now_us();

    // Keep the WebSocket pinned to its backend session while that session is
    // still alive.  Dropping c->ws_slot here after a short receive gap can
    // orphan the old session and remap the same browser to another Switch
    // port, which looks like held buttons flickering.
    if (c->ws_slot >= 0) {
        std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
        if (!g_clients[c->ws_slot].active)
            c->ws_slot = -1;
    }
    if (c->ws_slot < 0) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            std::lock_guard<std::mutex> lk(g_mtx[i]);
            if (!g_clients[i].active) {
                c->ws_slot = i;
                g_clients[i].active = true;
                g_clients[i].first_pkt = true;
                g_clients[i].report.reset();
                clear_all_motion_history(g_clients[i]);
                g_clients[i].uses_pad_presence = true;
                clear_udp_rumble_state(g_clients[i]);
                for (int s = 0; s < 4; ++s) {
                    g_clients[i].pad_present[s] = false;
                    g_clients[i].pad_last_present_us[s] = 0;
                    g_clients[i].debug_rumble_sent[s] = false;
                }
                g_clients[i].last_rx_us = now;
                for (int s = 0; s < 4; ++s)
                    c->last_rumble_seq[s] = g_clients[i].rumble_seq[s];
                break;
            }
        }
    }
    if (c->ws_slot >= 0) {
        std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);

        ExtendedHIDReport* dst_pads[4] = {
            &g_clients[c->ws_slot].report.p1,
            &g_clients[c->ws_slot].report.p2,
            &g_clients[c->ws_slot].report.p3,
            &g_clients[c->ws_slot].report.p4,
        };
        const ExtendedHIDReport* src_pads[4] = {
            &report.p1, &report.p2, &report.p3, &report.p4,
        };

        if (is_reset) {
            g_clients[c->ws_slot].report.reset();
            clear_all_motion_history(g_clients[c->ws_slot]);
            for (int s = 0; s < 4; ++s) {
                g_clients[c->ws_slot].pad_present[s] = false;
                g_clients[c->ws_slot].pad_last_present_us[s] = 0;
            }
        } else {
            for (int s = 0; s < 4; ++s) {
                if (pad_present[s]) {
                    *dst_pads[s] = *src_pads[s];
                    if (src_pads[s]->has_motion)
                        push_motion_history(g_clients[c->ws_slot], s, src_pads[s]->motion);
                    g_clients[c->ws_slot].pad_present[s] = true;
                    g_clients[c->ws_slot].pad_last_present_us[s] = now;
                } else {
                    // Treat an absent web/gamepad slot as a debounced disconnect,
                    // not as an immediate neutral frame.  That prevents one bad
                    // navigator.getGamepads() sample from releasing held R/ZR/etc.
                    g_clients[c->ws_slot].pad_present[s] = false;
                    uint64_t last_seen = g_clients[c->ws_slot].pad_last_present_us[s];
                    if (last_seen == 0 || now - last_seen >= WEB_PAD_ABSENT_RELEASE_US) {
                        dst_pads[s]->reset();
                        clear_motion_history(g_clients[c->ws_slot], s);
                    }
                }
            }
        }
        g_clients[c->ws_slot].last_rx_us = now;
    }
    c->ws_last_rx = now;
    ++g_pkts_rx;

    return total;
}


// ── Base64 encoding ──────────────────────────────────────────────────────────
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, size_t len, char *out) {
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i+1 < len) v |= (uint32_t)in[i+1] << 8;
        if (i+2 < len) v |= in[i+2];
        *out++ = B64[(v >> 18) & 0x3F];
        *out++ = B64[(v >> 12) & 0x3F];
        *out++ = (i+1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        *out++ = (i+2 < len) ? B64[v & 0x3F] : '=';
    }
    *out = '\0';
}


// ── Perform WebSocket upgrade handshake ──────────────────────────────────────
// Returns response length, or -1 on failure.  Caller must queue via async write.
static int ws_upgrade(const char *key_line, char *resp, size_t resp_sz) {
    const char *key_start = nullptr;
    const char *header_name = "sec-websocket-key:";
    const char *p = key_line;
    while (*p) {
        const char *h = header_name;
        const char *b = p;
        while (*h && *b && (tolower((unsigned char)*b) == tolower((unsigned char)*h))) {
            b++; h++;
        }
        if (!*h) { key_start = b; break; }
        p++;
    }

    if (!key_start) return -1;

    while (*key_start == ' ') key_start++;
    const char *key_end = strchr(key_start, '\r');
    if (!key_end) key_end = strchr(key_start, '\n');
    if (!key_end) return -1;

    char key[256];
    size_t klen = key_end - key_start;
    if (klen >= sizeof(key)) return -1;
    memcpy(key, key_start, klen);
    key[klen] = '\0';

    const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t sha_input[256];
    size_t slen = snprintf((char*)sha_input, sizeof(sha_input), "%s%s", key, magic);

    Sha1Ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, sha_input, slen);
    uint8_t digest[20];
    sha1_final(&ctx, digest);

    char b64out[64];
    base64_encode(digest, 20, b64out);

    return snprintf(resp, resp_sz,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", b64out);
}


// ── Format HTML body (heap-allocated, caller must free) ──────────────────────
// Do not pass raw HTML/CSS through snprintf as a format string: CSS contains
// plenty of '%' characters such as width: 100%, transform: translate(-50%, ...),
// and those are undefined behaviour in printf-style format strings.  This helper
// performs a single literal %s replacement instead, preserving the rest verbatim.
static char* html_format(const char *fmt, const char *arg, size_t *out_len) {
    if (!fmt || !arg || !out_len) return nullptr;

    std::string s = fmt;
    size_t pos = s.find("%s");
    if (pos != std::string::npos) {
        s.replace(pos, 2, arg);
    }

    char *buf = (char*)malloc(s.size() + 1);
    if (!buf) {
        *out_len = 0;
        return nullptr;
    }

    memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    *out_len = s.size();
    return buf;
}


// ── Case-insensitive header check ───────────────────────────────────────────
static bool has_header(const char *buf, const char *header) {
    size_t hlen = strlen(header);
    const char *p = buf;
    while (*p) {
        if ((p == buf || p[-1] == '\n') &&
            strncasecmp(p, header, hlen) == 0)
            return true;
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return false;
}

// ── Request-line prefix match (line-start only) ──────────────────────────────
static bool req_match(const char *buf, const char *path) {
    size_t plen = strlen(path);
    const char *p = buf;
    while (*p) {
        if ((p == buf || p[-1] == '\n') &&
            strncmp(p, path, plen) == 0)
            return true;
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return false;
}


// ── Web Server Thread (single-threaded poll reactor, fully non-blocking) ─────
static void web_server_thread(int web_port, uint16_t udp_port) {
    (void)udp_port;
    int srv = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv < 0) { perror("web socket"); return; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(web_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("web bind"); close(srv); return; }
    if (listen(srv, 8) < 0) { perror("web listen"); close(srv); return; }

    std::printf("[web] HTTP + WebSocket server listening on port %d\n", web_port);

    struct pollfd pfds[1 + MAX_WS_CLIENTS];
    static WebClient clients[MAX_WS_CLIENTS];
    int           n_clients = 0;

    pfds[0].fd = srv; pfds[0].events = POLLIN; pfds[0].revents = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) { pfds[i+1].fd = -1; }

    while (g_running.load(std::memory_order_relaxed)) {
        // Push pending Switch rumble events back to browser/mobile WebSocket clients.
        for (int i = 0; i < n_clients; i++)
            if (clients[i].state == WebClient::WS_ACTIVE)
                flush_rumble_to_ws(&clients[i]);

        // Idle WS timeout (30s) and HTTP handshake timeout (5s)
        uint64_t now_ws = now_us();
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state == WebClient::WS_ACTIVE &&
                now_ws - clients[i].ws_last_rx > 30000000)
                clients[i].state = WebClient::CLOSED;
            if (clients[i].state == WebClient::READ_HTTP &&
                clients[i].connect_time > 0 &&
                now_ws - clients[i].connect_time > 5000000)
                clients[i].state = WebClient::CLOSED;
        }

        // Update poll events based on client state (POLLOUT for write, POLLIN for read)
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state != WebClient::CLOSED) {
                if (clients[i].state == WebClient::WRITE_RESP)
                    pfds[i+1].events = POLLOUT;
                else if (clients[i].state == WebClient::WS_ACTIVE && clients[i].wbuf != nullptr)
                    pfds[i+1].events = POLLIN | POLLOUT;
                else
                    pfds[i+1].events = POLLIN;
                pfds[i+1].revents = 0;
            }
        }

        int rc = poll(pfds, 1 + n_clients, 200);
        if (rc <= 0) continue;

        // ── Accept new connections ─────────────────────────────────────────
        if (pfds[0].revents & POLLIN) {
            sockaddr_in peer{};
            socklen_t   plen = sizeof(peer);
            int fd = accept(srv, (sockaddr*)&peer, &plen);
            if (fd >= 0) {
                uint32_t peer_ip = peer.sin_addr.s_addr;
                int cnt = 0;
                for (int i = 0; i < n_clients; i++)
                    if (clients[i].state != WebClient::CLOSED && clients[i].ip == peer_ip)
                        cnt++;
                if (cnt >= 8) {
                    close(fd);
                    if (g_verbose) std::printf("[web] client rejected: %d connections from this IP\n", cnt);
                    continue;
                }

                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                struct timeval tv = {10, 0};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                int slot = -1;
                for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                    if (clients[i].state == WebClient::CLOSED) { slot = i; break; }
                }
                if (slot >= 0) {
                    clients[slot] = WebClient{};
                    clients[slot].fd = fd;
                    clients[slot].state = WebClient::READ_HTTP;
                    clients[slot].connect_time = now_us();
                    clients[slot].ip = peer_ip;
                    pfds[slot+1].fd = fd;
                    pfds[slot+1].events = POLLIN;
                    pfds[slot+1].revents = 0;
                    if (slot >= n_clients) n_clients = slot + 1;
                    if (g_verbose) std::printf("[web] client %d accepted (slot %d)\n", fd, slot);
                } else {
                    close(fd);
                    if (g_verbose) std::puts("[web] rejected: all slots full");
                }
            }
        }

        // ── Service existing clients ───────────────────────────────────────
        for (int i = 0; i < n_clients; i++) {
            WebClient *c = &clients[i];
            if (c->state == WebClient::CLOSED) continue;
            short rev = pfds[i+1].revents;
            if (rev & (POLLHUP | POLLERR)) { c->state = WebClient::CLOSED; continue; }

            // ── WRITE_RESP: flush queued HTTP response ──────────────────────
            if (c->state == WebClient::WRITE_RESP && (rev & POLLOUT)) {
                ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        continue; // will retry on next poll cycle
                    c->state = WebClient::CLOSED;
                } else {
                    c->woff += n;
                    if (c->woff >= c->wlen) {
                        free(c->wbuf);
                        c->wbuf = nullptr;
                        c->state = c->after_write;
                    }
                }
                continue;
            }

            // ── WS_ACTIVE: flush queued server→browser binary frames ─────────
            if (c->state == WebClient::WS_ACTIVE && c->wbuf != nullptr && (rev & POLLOUT)) {
                ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
                if (n < 0) {
                    if (!(errno == EAGAIN || errno == EWOULDBLOCK))
                        c->state = WebClient::CLOSED;
                } else {
                    c->woff += n;
                    if (c->woff >= c->wlen) {
                        free(c->wbuf);
                        c->wbuf = nullptr;
                        c->wlen = c->woff = 0;
                    }
                }
                if (c->state == WebClient::CLOSED) continue;
            }

            if (!(rev & POLLIN)) continue;

            // Read available data (defensive: check for buffer full first)
            if (c->fill >= sizeof(c->buf)) { c->state = WebClient::CLOSED; continue; }
            ssize_t n = read(c->fd, c->buf + c->fill, sizeof(c->buf) - c->fill);
            if (n <= 0) { c->state = WebClient::CLOSED; continue; }
            c->fill += n;

            // ── READ_HTTP: accumulate headers, then dispatch ────────────────
            if (c->state == WebClient::READ_HTTP) {
                size_t copy = std::min(sizeof(c->http_buf) - 1 - c->http_len, c->fill);
                if (copy == 0) { c->state = WebClient::CLOSED; continue; }
                memcpy(c->http_buf + c->http_len, c->buf, copy);
                c->http_len += copy;
                memmove(c->buf, c->buf + copy, c->fill - copy);
                c->fill -= copy;
                c->http_buf[c->http_len] = '\0';

                if (c->http_len >= 4 &&
                    c->http_buf[c->http_len-1] == '\n' &&
                    c->http_buf[c->http_len-2] == '\r' &&
                    c->http_buf[c->http_len-3] == '\n' &&
                    c->http_buf[c->http_len-4] == '\r')
                {
                    bool is_ws = has_header(c->http_buf, "upgrade: websocket") &&
                                 has_header(c->http_buf, "sec-websocket-key:");
                    if (is_ws) {
                        // Queue WS upgrade response via async write (never block on non-blocking socket)
                        char resp[512];
                        int n = ws_upgrade(c->http_buf, resp, sizeof(resp));
                        if (n > 0) {
                            c->wbuf = (uint8_t*)malloc(n);
                            if (c->wbuf) {
                                memcpy(c->wbuf, resp, n);
                                c->wlen = n;
                                c->woff = 0;
                                c->after_write = WebClient::WS_ACTIVE;
                                c->state = WebClient::WRITE_RESP;
                                c->ws_first = true;
                                c->ws_seq = 0;
                                c->ws_slot = -1;
                                c->ws_last_rx = now_us();
                                if (g_verbose) std::puts("[web] WS upgrade queued");
                            } else {
                                c->state = WebClient::CLOSED;
                            }
                        } else {
                            if (g_verbose) std::puts("[web] WS upgrade failed");
                            c->state = WebClient::CLOSED;
                        }
                    } else {
                        // Build HTTP response and queue it for non-blocking write
                        const char *body = nullptr;
                        size_t body_len = 0;
                        char *free_body = nullptr;
                        int status = 200;
                        const char *status_str = "OK";

                        if (req_match(c->http_buf, "GET / ") ||
                            req_match(c->http_buf, "GET /index.html ")) {
                            body = INDEX_HTML;
                            body_len = strlen(INDEX_HTML);
                        } else if (req_match(c->http_buf, "GET /mobile ")) {
                            free_body = html_format(MOBILE_HTML, MOBILE_STYLE_AND_DOM, &body_len);
                            body = free_body;
                        } else if (req_match(c->http_buf, "GET /editor ")) {
                            free_body = html_format(EDITOR_HTML, MOBILE_STYLE_AND_DOM, &body_len);
                            body = free_body;
                        } else {
                            body = "Not Found";
                            body_len = 9;
                            status = 404;
                            status_str = "Not Found";
                        }

                        if (!body) { c->state = WebClient::CLOSED; continue; }

                        char hdr[512];
                        int hdr_len = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 %d %s\r\n"
                            "Content-Type: text/html; charset=utf-8\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n"
                            "Cache-Control: no-cache\r\n"
                            "\r\n", status, status_str, body_len);

                        c->wbuf = (uint8_t*)malloc(hdr_len + body_len);
                        memcpy(c->wbuf, hdr, hdr_len);
                        memcpy(c->wbuf + hdr_len, body, body_len);
                        c->wlen = hdr_len + body_len;
                        c->woff = 0;
                        c->after_write = WebClient::CLOSED;
                        c->state = WebClient::WRITE_RESP;

                        free(free_body);
                        if (g_verbose) std::printf("[web] HTTP %d queued (%zu bytes)\n", status, c->wlen);
                    }
                }
                continue;
            }

            // ── WS_ACTIVE: process WebSocket frames ─────────────────────────
            if (c->state == WebClient::WS_ACTIVE) {
                size_t used;
                do {
                    used = process_ws_frame(c);
                    if (used > 0) {
                        memmove(c->buf, c->buf + used, c->fill - used);
                        c->fill -= used;
                    }
                } while (used > 0 && c->state == WebClient::WS_ACTIVE);
            }
        }

        // ── Cleanup closed clients ────────────────────────────────────────
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state == WebClient::CLOSED && clients[i].fd >= 0) {
                free(clients[i].wbuf);
                clients[i].wbuf = nullptr;
                if (clients[i].ws_slot >= 0) {
                    std::lock_guard<std::mutex> lk(g_mtx[clients[i].ws_slot]);
                    if (g_clients[clients[i].ws_slot].last_rx_us == clients[i].ws_last_rx) {
                        g_clients[clients[i].ws_slot].active = false;
                        g_clients[clients[i].ws_slot].report.reset();
                        clear_all_motion_history(g_clients[clients[i].ws_slot]);
                        g_clients[clients[i].ws_slot].uses_pad_presence = false;
                        for (int s = 0; s < 4; ++s) {
                            g_clients[clients[i].ws_slot].pad_present[s] = false;
                            g_clients[clients[i].ws_slot].pad_last_present_us[s] = 0;
                        }
                    }
                }
                if (g_verbose) std::printf("[web] client %d closed\n", clients[i].fd);
                close(clients[i].fd);
                clients[i].fd = -1;
                pfds[i+1].fd = -1;
            }
        }

        // Shrink n_clients
        while (n_clients > 0 && pfds[n_clients].fd == -1)
            n_clients--;
    }

    // Final cleanup
    for (int i = 0; i < n_clients; i++) {
        if (clients[i].fd >= 0) {
            free(clients[i].wbuf);
            if (clients[i].ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_mtx[clients[i].ws_slot]);
                if (g_clients[clients[i].ws_slot].last_rx_us == clients[i].ws_last_rx) {
                    g_clients[clients[i].ws_slot].active = false;
                    g_clients[clients[i].ws_slot].report.reset();
                    clear_all_motion_history(g_clients[clients[i].ws_slot]);
                    g_clients[clients[i].ws_slot].uses_pad_presence = false;
                    clear_udp_rumble_state(g_clients[clients[i].ws_slot]);
                    for (int s = 0; s < 4; ++s) {
                        g_clients[clients[i].ws_slot].pad_present[s] = false;
                        g_clients[clients[i].ws_slot].pad_last_present_us[s] = 0;
                    }
                }
            }
            close(clients[i].fd);
        }
    }
    close(srv);
    std::printf("[web] server stopped\n");
}


// ══════════════════════════════════════════════════════════════════════════════
// ── UDP receive loop (main thread) ────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
    std::string bind_addr = "0.0.0.0";
    bool        do_upnp   = false;
    int         web_port  = 0; // 0 = disabled

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if      (a == "-p" && i+1 < argc) port      = (uint16_t)std::atoi(argv[++i]);
        else if (a == "-b" && i+1 < argc) bind_addr = argv[++i];
        else if (a == "-v")               g_verbose  = true;
        else if (a == "--upnp")           do_upnp    = true;
        else if (a == "--no-auto-gadget") g_auto_gadget_setup = false;
        else if (a == "--force-gadget-setup") g_force_gadget_setup = true;
        else if (a == "--keep-gadget-on-exit") g_teardown_gadget_exit = false;
        else if (a == "--client-timeout-ms" && i+1 < argc) {
            uint64_t ms_v = (uint64_t)std::strtoull(argv[++i], nullptr, 10);
            if (ms_v < 1000) ms_v = 1000;
            g_client_timeout_us = ms_v * 1000ULL;
        }
        else if (a == "--imu-map" && i+1 < argc) {
            if (!parse_imu_map(argv[++i])) {
                std::fprintf(stderr, "Unknown --imu-map. Use: direct, invert-x, invert-y, invert-z, swap-yz, switch1, switch2, switch3\n");
                return 1;
            }
        }
        else if (a == "--pro-report-us" && i+1 < argc) {
            uint64_t us_v = (uint64_t)std::strtoull(argv[++i], nullptr, 10);
            // Keep sane bounds. 15ms is the real USB Pro Controller cadence;
            // 8ms is useful for experiments, but lower starts to look unlike
            // the real device and can make IMU consumers unhappy.
            if (us_v < 8000) us_v = 8000;
            if (us_v > 50000) us_v = 50000;
            g_pro_report_interval_us = us_v;
        }
        else if (a == "--test-imu" && i+1 < argc) {
            if (!parse_imu_test(argv[++i])) {
                std::fprintf(stderr, "Unknown --test-imu. Use: off, roll, pitch, yaw, shake, gentle\n");
                return 1;
            }
        }
        else if (a == "--imu-layout" && i+1 < argc) {
            if (!parse_imu_layout(argv[++i])) {
                std::fprintf(stderr, "Unknown --imu-layout. Use: accel-gyro or gyro-accel\n");
                return 1;
            }
        }
        else if (a == "--bat-con" && i+1 < argc) {
            if (!parse_u8_arg(argv[++i], g_pro_bat_con)) {
                std::fprintf(stderr, "Invalid --bat-con byte. Examples: 0x81, 0x91, 0x8E\n");
                return 1;
            }
        }
        else if ((a == "--vibrator-report" || a == "--vibrator-byte") && i+1 < argc) {
            if (!parse_u8_arg(argv[++i], g_pro_vibrator_report)) {
                std::fprintf(stderr, "Invalid --vibrator-report byte. Examples: 0x00, 0x80\n");
                return 1;
            }
        }
        else if (a == "--log-raw-30") g_log_raw_30 = true;
        else if (a == "--log-neutral-rumble") g_log_neutral_rumble = true;
        else if (a == "--test-rumble") g_test_rumble_on_connect = true;
        else if (a == "--random-identity") g_random_identity = true;
        else if (a == "--no-user-imu-cal") g_no_user_imu_cal = true;
        else if (a == "--gadget-pads" && i+1 < argc) {
            int n = std::atoi(argv[++i]);
            if (n < 1) n = 1;
            if (n > 4) n = 4;
            g_hid_port_count = n;
        }
        else if (a == "-w") {
            if (i+1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
                web_port = std::atoi(argv[++i]);
            else
                web_port = 8080;
        }
        else if (a == "-h") {
            puts("ns-backend  [-p PORT] [-b ADDR] [--upnp] [-w [WEB_PORT]] [-v]");
            puts("            [--force-gadget-setup] [--no-auto-gadget] [--keep-gadget-on-exit] [--gadget-pads 1..4]");
            puts("            [--random-identity]");
            puts("            [--client-timeout-ms MS] [--imu-map direct|invert-x|invert-y|invert-z|swap-yz|switch1|switch2|switch3]");
            puts("            [--pro-report-us US] [--test-imu off|roll|pitch|yaw|shake]");
            puts("            [--log-neutral-rumble] [--test-rumble] [--no-user-imu-cal]");
            return 0;
        }
    }

    if (g_random_identity)
        randomize_controller_identity();
    print_controller_identity();

    // Always recreate the built-in gadget at process startup when auto-gadget
    // mode is enabled.  This makes every launch self-healing: stale configfs
    // state, leftover /dev/hidg* nodes, or a previous unclean shutdown are
    // cleared before the backend starts talking to the Switch.
    if (g_auto_gadget_setup || g_force_gadget_setup)
        run_gadget_setup_if_needed(true,
            g_force_gadget_setup ? "forced gadget setup requested"
                                 : "startup gadget recreation requested");

    derive_key(DEFAULT_SECRET, g_hmac_key);
    if (g_verbose)
        std::printf("[pro] report interval=%llu us, imu-map=%s, imu-layout=%s, imu-test=%s, bat_con=0x%02X, vibrator=0x%02X\n",
                    (unsigned long long)g_pro_report_interval_us,
                    imu_map_name(g_imu_map),
                    imu_layout_name(g_imu_layout),
                    imu_test_name(g_imu_test),
                    g_pro_bat_con,
                    g_pro_vibrator_report);
    signal(SIGINT,  on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (do_upnp) upnp_add_mapping(port);

    // Start web server if requested
    std::thread web_thread;
    if (web_port > 0)
        web_thread = std::thread(web_server_thread, web_port, port);

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int rbuf = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }
    
    std::printf("UDP %s:%u writer=%d Hz\n",
                bind_addr.c_str(), port, WRITER_HZ);
    if (g_verbose) {
        std::printf("[config] client_timeout=%.1fs stale_neutral=%.0fms imu_map=%s gadget_pads=%d random_identity=%s neutral_rumble_log=%s test_rumble=%s no_user_imu_cal=%s\n",
                    (double)g_client_timeout_us / 1000000.0,
                    (double)CLIENT_STALE_NEUTRAL_US / 1000.0,
                    imu_map_name(g_imu_map),
                    g_hid_port_count,
                    g_random_identity ? "yes" : "no",
                    g_log_neutral_rumble ? "yes" : "no",
                    g_test_rumble_on_connect ? "yes" : "no",
                    g_no_user_imu_cal ? "yes" : "no");
    }

    std::thread wt(writer_thread, WRITER_HZ);
    std::thread st(stats_thread);

    int ep = epoll_create1(0); epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = sock; epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);

    std::vector<uint8_t> udp_rx(std::max(UDP_RX_MAX_PACKET_SIZE, MACRO_CHUNK_HEADER_SIZE + MACRO_UDP_CHUNK_MAX + HMAC_TAG_SIZE));
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 4, 200);
        if (n <= 0) continue;

        sockaddr_in sender{};
        socklen_t slen;
        ssize_t bytes;

        // Drain all available packets from the kernel buffer.
        // Two UDP packet formats are accepted:
        //   1) legacy Packet: input only, unchanged, authenticated with HMAC.
        //   2) ExtendedUdpPacket: ExtendedMultiReport with motion/gyro, authenticated
        //      with HMAC, and opted into UDP rumble replies.
        while (g_running.load(std::memory_order_relaxed)) {
            slen = sizeof(sender);
            bytes = recvfrom(sock, udp_rx.data(), udp_rx.size(), 0, (sockaddr*)&sender, &slen);
            if (bytes <= 0) break; // EAGAIN or error — ring is drained


            if (bytes >= 4) {
                uint32_t mmagic = 0;
                memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == MACRO_UDP_CHUNK_MAGIC) {
                    server_macro_handle_chunk_packet(udp_rx.data(), (size_t)bytes, sender);
                    continue;
                }
            }

            if (bytes >= (ssize_t)(MACRO_UDP_HEADER_SIZE + HMAC_TAG_SIZE)) {
                uint32_t mmagic = 0;
                memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == MACRO_UDP_MAGIC) {
                    MacroUdpHeaderWire mh{};
                    memcpy(&mh, udp_rx.data(), sizeof(mh));
                    uint32_t text_len = mh.text_len;
                    if (text_len <= MACRO_UDP_TEXT_MAX && bytes == (ssize_t)(MACRO_UDP_HEADER_SIZE + text_len + HMAC_TAG_SIZE)) {
                        const uint8_t* recv_hmac = udp_rx.data() + MACRO_UDP_HEADER_SIZE + text_len;
                        if (hmac_verify(g_hmac_key, 32, udp_rx.data(), MACRO_UDP_HEADER_SIZE + text_len, recv_hmac, HMAC_TAG_SIZE) == 0) {
                            if (!rate_allow(sender.sin_addr.s_addr)) continue;
                            int client_idx = server_macro_client_for_sender(sender);
                            if (client_idx >= 0) {
                                {
                                    std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
                                    g_clients[client_idx].uses_pad_presence = true;
                                    int sp = mh.subpad < 4 ? mh.subpad : 0;
                                    g_clients[client_idx].pad_present[sp] = true;
                                    g_clients[client_idx].pad_last_present_us[sp] = now_us();
                                }
                                std::string text(reinterpret_cast<char*>(udp_rx.data() + MACRO_UDP_HEADER_SIZE), text_len);
                                server_macro_start(client_idx, mh.subpad < 4 ? mh.subpad : 0, text);
                            }
                        } else if (g_verbose) puts("bad macro HMAC, dropped");
                    } else if (g_verbose) puts("bad macro packet size, dropped");
                    continue;
                }
            }

            bool is_extended_udp = false;
            Packet pkt{};
            ExtendedUdpPacket ext_pkt{};

            if (bytes == (ssize_t)PACKET_SIZE) {
                memcpy(&pkt, udp_rx.data(), sizeof(pkt));
            } else if (bytes == (ssize_t)EXT_UDP_PACKET_SIZE) {
                memcpy(&ext_pkt, udp_rx.data(), sizeof(ext_pkt));
                is_extended_udp = true;
            } else {
                if (g_verbose) std::printf("[udp] unexpected packet size=%zd, dropped\n", bytes);
                continue;
            }

            // ── 1. Per-IP rate limiter ────────────────────────────────────────────
            uint32_t src_ip = sender.sin_addr.s_addr;
            if (!rate_allow(src_ip)) {
                if (g_verbose) puts("rate limit exceeded, dropped");
                continue;
            }

            // ── 2. Magic + version check ──────────────────────────────────────────
            if (is_extended_udp) {
                if (!extended_udp_packet_ok(ext_pkt)) {
                    if (g_verbose) puts("bad extended UDP magic/version, dropped");
                    continue;
                }
            } else if (!packet_ok(pkt)) {
                if (g_verbose) puts("bad magic/version, dropped");
                continue;
            }

            // ── 3. Find Client Session or Pin new IP:port ─────────────────────────
            int client_idx = -1;
            uint64_t now = now_us();

            for (int i = 0; i < MAX_CLIENTS; ++i) {
                std::lock_guard<std::mutex> lk(g_mtx[i]);
                if (g_clients[i].active &&
                    g_clients[i].addr.sin_addr.s_addr == src_ip &&
                    g_clients[i].addr.sin_port == sender.sin_port) {
                    client_idx = i;
                    break;
                }
            }

            // If not found, assign to a free/timed-out slot.
            if (client_idx == -1) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_mtx[i]);
                    if (!g_clients[i].active || elapsed_us_over(now, g_clients[i].last_rx_us, g_client_timeout_us)) {
                        client_idx = i;
                        g_clients[i].active = true;
                        g_clients[i].addr = sender;
                        g_clients[i].first_pkt = true;
                        g_clients[i].expected_seq = 0;
                        g_clients[i].report.reset();
                        clear_all_motion_history(g_clients[i]);
                        g_clients[i].uses_pad_presence = false;
                        clear_udp_rumble_state(g_clients[i]);
                        for (int s = 0; s < 4; ++s) {
                            g_clients[i].pad_present[s] = false;
                            g_clients[i].pad_last_present_us[s] = 0;
                            g_clients[i].debug_rumble_sent[s] = false;
                        }
                        g_clients[i].last_rx_us = now;
                        if (g_verbose) std::printf("New UDP client accepted into Server Slot %d/4\n", i+1);
                        break;
                    }
                }
            }

            // If all 4 slots are taken by active PCs, drop the packet.
            if (client_idx == -1) {
                if (g_verbose) puts("server is full (4 PCs already active), dropped");
                continue;
            }

            // ── 4. HMAC authentication ────────────────────────────────────────────
            int hmac_ok = 0;
            if (is_extended_udp) {
                hmac_ok = hmac_verify(g_hmac_key, 32,
                                      reinterpret_cast<const uint8_t*>(&ext_pkt),
                                      EXT_UDP_PACKET_AUTH_SIZE,
                                      ext_pkt.hmac,
                                      HMAC_TAG_SIZE);
            } else {
                hmac_ok = hmac_verify(g_hmac_key, 32,
                                      reinterpret_cast<const uint8_t*>(&pkt),
                                      PACKET_AUTH_SIZE,
                                      pkt.hmac,
                                      HMAC_TAG_SIZE);
            }
            if (hmac_ok != 0) {
                if (g_verbose) puts("bad HMAC, dropped");
                continue;
            }

            // ── 5. Sequence counter + Apply to shared state ───────────────────────
            bool accepted = false;
            {
                std::lock_guard<std::mutex> lk(g_mtx[client_idx]);

                // Re-validate: writer may have deactivated the slot during HMAC.
                if (!g_clients[client_idx].active) continue;

                uint8_t flags = is_extended_udp ? ext_pkt.flags : pkt.flags;
                uint32_t seq = is_extended_udp ? ext_pkt.seq : pkt.seq;
                bool is_reset = (flags & FLAG_RESET);
                bool sequence_jump = (g_clients[client_idx].expected_seq > seq) &&
                                     ((g_clients[client_idx].expected_seq - seq) > 100);

                if (!g_clients[client_idx].first_pkt && seq < g_clients[client_idx].expected_seq && !is_reset && !sequence_jump) {
                    if (g_verbose)
                        std::printf("UDP client %d out-of-order seq=%u, dropped\n", client_idx+1, seq);
                    continue;
                }
                g_clients[client_idx].first_pkt = false;
                g_clients[client_idx].expected_seq = seq + 1;

                if (is_reset) {
                    g_clients[client_idx].report.reset();
                    clear_all_motion_history(g_clients[client_idx]);
                    for (int s = 0; s < 4; ++s) {
                        g_clients[client_idx].pad_present[s] = false;
                        g_clients[client_idx].pad_last_present_us[s] = 0;
                    }
                } else if (is_extended_udp) {
                    // Extended UDP carries motion/gyro and pad-present flags, so
                    // neutral-but-connected pads can still receive rumble.
                    g_clients[client_idx].uses_pad_presence = true;
                    enable_udp_rumble_state(g_clients[client_idx]);

                    ExtendedHIDReport* dst_pads[4] = {
                        &g_clients[client_idx].report.p1,
                        &g_clients[client_idx].report.p2,
                        &g_clients[client_idx].report.p3,
                        &g_clients[client_idx].report.p4,
                    };
                    const ExtendedHIDReport* src_pads[4] = {
                        &ext_pkt.report.p1, &ext_pkt.report.p2,
                        &ext_pkt.report.p3, &ext_pkt.report.p4,
                    };

                    for (int s = 0; s < 4; ++s) {
                        bool present = extended_report_pad_present(ext_pkt.report, s);
                        if (present) {
                            *dst_pads[s] = *src_pads[s];
                            if (src_pads[s]->has_motion)
                                push_motion_history(g_clients[client_idx], s, src_pads[s]->motion);
                            g_clients[client_idx].pad_present[s] = true;
                            g_clients[client_idx].pad_last_present_us[s] = now;
                        } else {
                            g_clients[client_idx].pad_present[s] = false;
                            uint64_t last_seen = g_clients[client_idx].pad_last_present_us[s];
                            if (last_seen == 0 || now - last_seen >= WEB_PAD_ABSENT_RELEASE_US) {
                                dst_pads[s]->reset();
                                clear_motion_history(g_clients[client_idx], s);
                            }
                        }
                    }
                } else {
                    // Legacy UDP remains 100% compatible: input-only, no pad-present
                    // tracking, no gyro, and no UDP rumble replies.
                    g_clients[client_idx].uses_pad_presence = false;
                    clear_udp_rumble_state(g_clients[client_idx]);
                    for (int s = 0; s < 4; ++s) {
                        g_clients[client_idx].pad_present[s] = false;
                        g_clients[client_idx].pad_last_present_us[s] = 0;
                    }
                    clear_all_motion_history(g_clients[client_idx]);
                    legacy_multi_to_extended(pkt.report, g_clients[client_idx].report);
                }

                g_clients[client_idx].last_rx_us = now_us();
                accepted = true;
            }

            if (!accepted) continue;
            ++g_pkts_rx;

            // Extended UDP clients opted into rumble by using the new packet
            // format.  Legacy clients are not sent unexpected traffic.
            if (is_extended_udp) {
                queue_debug_test_rumble_if_needed(client_idx, 0);
                flush_rumble_to_udp(sock, client_idx);
            }
        } // drain loop
    } // epoll loop

    puts("[backend] shutting down");
    upnp_remove_mapping(port);
    close(ep); close(sock);
    wt.join(); st.join();
    if (web_thread.joinable()) web_thread.join();

    if (g_teardown_gadget_exit)
        teardown_gadget();

    return 0;
}
