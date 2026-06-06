/// ns-gamepad.mm  —  macOS frontend for the Switch wireless gamepad bridge
///
/// Uses Apple's GameController framework for controller input.
/// Natively supports Xbox, PlayStation, MFi, and Switch Pro Controllers
/// (via Bluetooth or USB, depending on macOS version).
/// Networking uses BSD sockets — identical API to the Linux version.
///
/// Build:
///   clang++ -std=c++17 -ObjC++ \
///           -framework GameController -framework Foundation -framework CoreGraphics \
///           -framework CoreHaptics \
///           ns-gamepad.mm -o ns-gamepad
///
/// Usage:
///   ./ns-gamepad <RASPBERRY_PI_IP[:PORT]> [--legacy] [-k [single|override]] [--macro macro.json [--upload-macro macro.json]]

#ifndef __APPLE__
#  error "ns-gamepad.mm is macOS-only."
#endif

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>

#include <iostream>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <signal.h>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <dispatch/dispatch.h>

#include <fstream>
#include <sstream>
#include <cctype>
#include <mach-o/dyld.h>
#include <CoreGraphics/CoreGraphics.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/resource.h>
#include "../../server/rpi/include/sha256.h"
#include "../../server/rpi/include/protocol.hpp"
#include <stdexcept>
#include <limits>

// ─────────────────────────────────────────────────────────────────────────────
//  Multi-controller state (up to 4 local physical controllers)
// ─────────────────────────────────────────────────────────────────────────────

/// Thread-safe container for one controller's input state.
/// GCController value-change handlers (main thread) write here;
/// the UDP sender thread reads with relaxed atomics.
struct GamepadState {
    std::atomic<bool>  btn_a{false}, btn_b{false}, btn_x{false}, btn_y{false};
    std::atomic<bool>  btn_l{false}, btn_r{false};
    std::atomic<float> zl{0.0f}, zr{0.0f};
    std::atomic<bool>  btn_menu{false}, btn_options{false};
    std::atomic<bool>  btn_lstick{false}, btn_rstick{false};
    std::atomic<bool>  dpad_up{false}, dpad_down{false}, dpad_left{false}, dpad_right{false};
    std::atomic<float> lx{0.0f}, ly{0.0f}, rx{0.0f}, ry{0.0f};

    // GameController motion values.  Accel is expressed in g-ish units, gyro in rad/s.
    // The sender converts these to the backend's Switch-like int16 motion report.
    std::atomic<bool>  has_motion{false};
    std::atomic<float> ax{0.0f}, ay{0.0f}, az{0.0f};
    std::atomic<float> gx{0.0f}, gy{0.0f}, gz{0.0f};
};

static void reset_gamepad_state(GamepadState& st) {
    st.btn_a.store(false); st.btn_b.store(false); st.btn_x.store(false); st.btn_y.store(false);
    st.btn_l.store(false); st.btn_r.store(false);
    st.zl.store(0.0f); st.zr.store(0.0f);
    st.btn_menu.store(false); st.btn_options.store(false);
    st.btn_lstick.store(false); st.btn_rstick.store(false);
    st.dpad_up.store(false); st.dpad_down.store(false); st.dpad_left.store(false); st.dpad_right.store(false);
    st.lx.store(0.0f); st.ly.store(0.0f); st.rx.store(0.0f); st.ry.store(0.0f);
    st.has_motion.store(false);
    st.ax.store(0.0f); st.ay.store(0.0f); st.az.store(0.0f);
    st.gx.store(0.0f); st.gy.store(0.0f); st.gz.store(0.0f);
}

static constexpr int MAX_SLOTS = 4;
static GamepadState  g_states[MAX_SLOTS];
static GCController* g_controllers[MAX_SLOTS] = {};

// FIX 1: Separate atomic flags so the sender thread can safely check slot
// occupancy without touching the ObjC pointer (which is not atomic-safe with ARC).
// Only the main thread reads/writes g_controllers[]; the sender thread only
// reads g_slot_active[].
static std::atomic<bool> g_slot_active[MAX_SLOTS] = {};
static int keyboard_mode = 0; // 0=off, 1=single, 2=override
static bool legacy_udp = false; // --legacy keeps the old input-only packet format

static constexpr uint8_t EXT_PAD_PRESENT = 0x01;

// ── Macro support ───────────────────────────────────────────────────────────
// Macro grammar is intentionally strict and shared by CLI/GUI clients:
//   WAIT 100                         -> release macro inputs for 100ms
//   A 100                            -> hold A for 100ms
//   R+LSTICK_LEFT 450                -> hold R and steer left for 450ms
// Accepted JSON:
//   {"name":"...","commands":"WAIT 100; A 100"}
//   {"name":"...","commands":["WAIT 100", "A 100"]}
static constexpr size_t MACRO_JSON_MAX_BYTES = 50ULL * 1024ULL * 1024ULL;
static std::string g_macro_last_error;

struct MacroStep {
    uint16_t buttons = 0;
    uint8_t hat = ns::HAT_NEUTRAL;
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

static bool macro_apply_token(const std::string& raw_tok, MacroStep& st, std::string& err,
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

static bool macro_parse_one_command(const std::string& part, MacroStep& st, std::string& err) {
    size_t last_space = part.find_last_of(" \t");
    if (last_space == std::string::npos) { err = "missing duration in command: " + part; return false; }
    std::string cmd = macro_trim(part.substr(0, last_space));
    std::string ms_s = macro_trim(part.substr(last_space + 1));
    uint32_t ms = 0;
    if (!macro_parse_uint32_strict(ms_s, ms)) { err = "invalid duration in command: " + part; return false; }
    st = MacroStep{};
    st.duration_ms = ms;
    std::string up = macro_upper(cmd);
    if (up == "WAIT" || up == "LOOP") return true;
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

    if (du && dr) st.hat = ns::HAT_NE;
    else if (du && dl) st.hat = ns::HAT_NW;
    else if (dd && dr) st.hat = ns::HAT_SE;
    else if (dd && dl) st.hat = ns::HAT_SW;
    else if (du) st.hat = ns::HAT_N;
    else if (dd) st.hat = ns::HAT_S;
    else if (dr) st.hat = ns::HAT_E;
    else if (dl) st.hat = ns::HAT_W;

    if (st.has_lstick) { st.lx = lll ? 0 : (llr ? 255 : 128); st.ly = llu ? 0 : (lld ? 255 : 128); }
    if (st.has_rstick) { st.rx = rrl ? 0 : (rrr ? 255 : 128); st.ry = rru ? 0 : (rrd ? 255 : 128); }
    return true;
}

static bool macro_validate_text(const std::string& raw_text, std::vector<MacroStep>& steps, std::vector<std::string>* normalized = nullptr) {
    g_macro_last_error.clear();
    steps.clear();
    if (normalized) normalized->clear();
    std::string text, err;
    if (!macro_extract_commands_text(raw_text, text, err)) { macro_set_error(err); return false; }
    for (char& c : text) if (c == '\n' || c == '\r') c = ';';
    size_t pos = 0;
    while (pos < text.size()) {
        size_t semi = text.find(';', pos);
        std::string part = macro_trim(text.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
        pos = (semi == std::string::npos) ? text.size() : semi + 1;
        if (part.empty()) continue;
        MacroStep st;
        if (!macro_parse_one_command(part, st, err)) { macro_set_error(err); return false; }
        steps.push_back(st);
        if (normalized) normalized->push_back(part);
    }
    if (steps.empty()) { macro_set_error("no valid macro commands found"); return false; }
    return true;
}

static std::vector<MacroStep> macro_parse_text(const std::string& raw_text) {
    std::vector<MacroStep> steps;
    macro_validate_text(raw_text, steps, nullptr);
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

static std::vector<MacroStep> macro_load_file(const std::string& path) {
    std::string txt = macro_read_file(path);
    if (txt.empty()) return {};
    return macro_parse_text(txt);
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
    std::vector<MacroStep> steps;
    std::vector<std::string> lines;
    if (!macro_validate_text(raw_text, steps, &lines)) {
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

static bool macro_validate_to_pretty_json(const std::string& raw_text, std::string& pretty, std::string& err, const std::string& fallback_name = "Macro") {
    std::vector<MacroStep> steps;
    if (!macro_validate_text(raw_text, steps, nullptr)) { err = macro_last_error(); return false; }
    pretty = macro_pretty_json(raw_text, fallback_name);
    err.clear();
    return true;
}

static uint64_t macro_total_ms(const std::vector<MacroStep>& steps) {
    uint64_t total = 0;
    for (const auto& s : steps) {
        if (UINT64_MAX - total < s.duration_ms) return UINT64_MAX;
        total += s.duration_ms;
    }
    return total;
}

static bool macro_report_at(const std::vector<MacroStep>& steps, uint64_t elapsed_ms, ns::HIDReport& out) {
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



// ── Server-side macro upload packet ─────────────────────────────────────────
static constexpr uint32_t MACRO_UDP_MAGIC       = 0x4E534D43u; // 'NSMC' legacy one-datagram upload
static constexpr uint32_t MACRO_UDP_CHUNK_MAGIC = 0x4E534D4Bu; // 'NSMK' chunked upload
static constexpr size_t   MACRO_UDP_TEXT_MAX    = MACRO_JSON_MAX_BYTES;
static constexpr size_t   MACRO_UDP_CHUNK_MAX   = 1200;
#pragma pack(push, 1)
struct MacroUdpHeaderWire {
    uint32_t magic;
    uint8_t version;
    uint8_t subpad;
    uint32_t text_len;
    uint32_t seq;
};
struct MacroUdpChunkHeaderWire {
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
#pragma pack(pop)
static constexpr size_t MACRO_UDP_HEADER_SIZE = sizeof(MacroUdpHeaderWire);
static constexpr size_t MACRO_CHUNK_HEADER_SIZE = sizeof(MacroUdpChunkHeaderWire);
static_assert(MACRO_CHUNK_HEADER_SIZE == 30, "Macro chunk header wire size changed");
static uint32_t g_macro_udp_seq = 0;

static uint32_t next_macro_upload_id() {
    uint32_t a = (uint32_t)ns::now_us();
    uint32_t b = ++g_macro_udp_seq;
    return a ^ (b * 2654435761u);
}

template<typename SockT>
static bool send_macro_udp_packet(SockT sock, const sockaddr_in& dest, const uint8_t hmac_key[32], const std::string& json_or_commands, uint8_t subpad = 0) {
    std::string text = json_or_commands;
    if (text.size() > MACRO_UDP_TEXT_MAX) {
        macro_set_error("macro JSON exceeds 50MB limit");
        return false;
    }

    const uint8_t safe_subpad = subpad < 4 ? subpad : 0;
    if (text.size() <= 900) {
        const size_t total = MACRO_UDP_HEADER_SIZE + text.size() + ns::HMAC_TAG_SIZE;
        std::vector<uint8_t> pkt(total, 0);
        MacroUdpHeaderWire h{};
        h.magic = MACRO_UDP_MAGIC;
        h.version = ns::PROTO_VERSION;
        h.subpad = safe_subpad;
        h.text_len = (uint32_t)text.size();
        h.seq = g_macro_udp_seq++;
        memcpy(pkt.data(), &h, sizeof(h));
        if (!text.empty()) memcpy(pkt.data() + MACRO_UDP_HEADER_SIZE, text.data(), text.size());
        uint8_t full_hmac[32];
        hmac_sha256(hmac_key, 32, pkt.data(), MACRO_UDP_HEADER_SIZE + text.size(), full_hmac);
        memcpy(pkt.data() + MACRO_UDP_HEADER_SIZE + text.size(), full_hmac, ns::HMAC_TAG_SIZE);
        return sendto(sock, reinterpret_cast<const char*>(pkt.data()), (int)pkt.size(), 0,
                      reinterpret_cast<const sockaddr*>(&dest), sizeof(dest)) == (int)pkt.size();
    }

    const uint32_t upload_id = next_macro_upload_id();
    const uint32_t chunk_count = (uint32_t)((text.size() + MACRO_UDP_CHUNK_MAX - 1) / MACRO_UDP_CHUNK_MAX);
    for (uint32_t idx = 0; idx < chunk_count; ++idx) {
        size_t off = (size_t)idx * MACRO_UDP_CHUNK_MAX;
        uint16_t chunk_len = (uint16_t)std::min(MACRO_UDP_CHUNK_MAX, text.size() - off);
        const size_t total = MACRO_CHUNK_HEADER_SIZE + chunk_len + ns::HMAC_TAG_SIZE;
        std::vector<uint8_t> pkt(total, 0);
        MacroUdpChunkHeaderWire h{};
        h.magic = MACRO_UDP_CHUNK_MAGIC;
        h.version = ns::PROTO_VERSION;
        h.subpad = safe_subpad;
        h.flags = (idx + 1 == chunk_count) ? 1 : 0;
        h.upload_id = upload_id;
        h.chunk_index = idx;
        h.chunk_count = chunk_count;
        h.total_len = (uint32_t)text.size();
        h.chunk_len = chunk_len;
        h.seq = g_macro_udp_seq++;
        memcpy(pkt.data(), &h, sizeof(h));
        memcpy(pkt.data() + MACRO_CHUNK_HEADER_SIZE, text.data() + off, chunk_len);
        uint8_t full_hmac[32];
        hmac_sha256(hmac_key, 32, pkt.data(), MACRO_CHUNK_HEADER_SIZE + chunk_len, full_hmac);
        memcpy(pkt.data() + MACRO_CHUNK_HEADER_SIZE + chunk_len, full_hmac, ns::HMAC_TAG_SIZE);
        int sent = sendto(sock, reinterpret_cast<const char*>(pkt.data()), (int)pkt.size(), 0,
                          reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (sent != (int)pkt.size()) return false;
        if ((idx % 16) == 15) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Signal handling
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ─────────────────────────────────────────────────────────────────────────────
//  Axis conversion
// ─────────────────────────────────────────────────────────────────────────────

/// Convert a GCController float axis (-1.0..1.0) to an HID unsigned byte (0-255) with deadzone.
static uint8_t float_to_byte(float val, bool invert = false, float dz = 0.15f) {
    if (std::abs(val) < dz) return 128;
    int scaled;
    float range = 1.0f - dz;
    if (val > 0.0f)
        scaled = 128 + (int)(((val - dz) / range) * 127.0f);
    else
        scaled = 128 - (int)(((-val - dz) / range) * 128.0f);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? 255 - scaled : scaled);
}

static int16_t clamp_i16_from_float(float v) {
    if (v < -32768.0f) return -32768;
    if (v >  32767.0f) return  32767;
    return (int16_t)std::lrintf(v);
}

static ns::MotionReport map_gc_motion_to_switch(const GamepadState& st) {
    ns::MotionReport m; m.reset();

    // Switch calibration in the backend uses 0x1000-ish accel units, so convert
    // GameController gravity/userAcceleration units to roughly the same scale.
    m.ax = clamp_i16_from_float(st.ax.load(std::memory_order_relaxed) * 4096.0f);
    m.ay = clamp_i16_from_float(st.ay.load(std::memory_order_relaxed) * 4096.0f);
    m.az = clamp_i16_from_float(st.az.load(std::memory_order_relaxed) * 4096.0f);

    // GameController rotationRate is rad/s.  The backend just forwards int16
    // gyro samples, so this scale is intentionally conservative and tunable.
    m.gx = clamp_i16_from_float(st.gx.load(std::memory_order_relaxed) * 1000.0f);
    m.gy = clamp_i16_from_float(st.gy.load(std::memory_order_relaxed) * 1000.0f);
    m.gz = clamp_i16_from_float(st.gz.load(std::memory_order_relaxed) * 1000.0f);
    return m;
}

static void set_pad_present_flag(ns::ExtendedHIDReport& r, bool present) {
    // The backend/web protocol uses byte 7 of each ExtendedHIDReport as the
    // pad-present flag.  This lets neutral-but-connected UDP pads still claim
    // a Switch slot and receive rumble.
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
//  GameController integration
// ─────────────────────────────────────────────────────────────────────────────

/// Register value-change handlers for every input on a GCExtendedGamepad.
/// Handlers run on the main thread and atomically update the given GamepadState.
static void attach_handlers(GCController* ctrl, GCExtendedGamepad* gp, GamepadState* st) {
    gp.buttonA.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->btn_a.store((bool)p, std::memory_order_relaxed);
    };
    gp.buttonB.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->btn_b.store((bool)p, std::memory_order_relaxed);
    };
    gp.buttonX.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->btn_x.store((bool)p, std::memory_order_relaxed);
    };
    gp.buttonY.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->btn_y.store((bool)p, std::memory_order_relaxed);
    };
    gp.leftShoulder.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->btn_l.store((bool)p, std::memory_order_relaxed);
    };
    gp.rightShoulder.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->btn_r.store((bool)p, std::memory_order_relaxed);
    };
    gp.leftTrigger.valueChangedHandler = ^(GCControllerButtonInput*, float v, BOOL) {
        st->zl.store(v, std::memory_order_relaxed);
    };
    gp.rightTrigger.valueChangedHandler = ^(GCControllerButtonInput*, float v, BOOL) {
        st->zr.store(v, std::memory_order_relaxed);
    };
    gp.buttonMenu.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->btn_menu.store((bool)p, std::memory_order_relaxed);
    };
    if (gp.buttonOptions) {
        gp.buttonOptions.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
            st->btn_options.store((bool)p, std::memory_order_relaxed);
        };
    }
    if (gp.leftThumbstickButton) {
        gp.leftThumbstickButton.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
            st->btn_lstick.store((bool)p, std::memory_order_relaxed);
        };
    }
    if (gp.rightThumbstickButton) {
        gp.rightThumbstickButton.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
            st->btn_rstick.store((bool)p, std::memory_order_relaxed);
        };
    }
    gp.dpad.up.valueChangedHandler    = ^(GCControllerButtonInput*, float, BOOL p) {
        st->dpad_up.store((bool)p, std::memory_order_relaxed);
    };
    gp.dpad.down.valueChangedHandler  = ^(GCControllerButtonInput*, float, BOOL p) {
        st->dpad_down.store((bool)p, std::memory_order_relaxed);
    };
    gp.dpad.left.valueChangedHandler  = ^(GCControllerButtonInput*, float, BOOL p) {
        st->dpad_left.store((bool)p, std::memory_order_relaxed);
    };
    gp.dpad.right.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->dpad_right.store((bool)p, std::memory_order_relaxed);
    };
    gp.leftThumbstick.xAxis.valueChangedHandler  = ^(GCControllerAxisInput*, float v) {
        st->lx.store(v, std::memory_order_relaxed);
    };
    gp.leftThumbstick.yAxis.valueChangedHandler  = ^(GCControllerAxisInput*, float v) {
        st->ly.store(v, std::memory_order_relaxed);
    };
    gp.rightThumbstick.xAxis.valueChangedHandler = ^(GCControllerAxisInput*, float v) {
        st->rx.store(v, std::memory_order_relaxed);
    };
    gp.rightThumbstick.yAxis.valueChangedHandler = ^(GCControllerAxisInput*, float v) {
        st->ry.store(v, std::memory_order_relaxed);
    };

    if (@available(macOS 10.15, *)) {
        GCMotion* motion = ctrl.motion;
        if (motion) {
            motion.valueChangedHandler = ^(GCMotion* m) {
                GCAcceleration gravity = m.gravity;
                GCAcceleration user    = m.userAcceleration;
                GCRotationRate rate     = m.rotationRate;

                st->ax.store((float)(gravity.x + user.x), std::memory_order_relaxed);
                st->ay.store((float)(gravity.y + user.y), std::memory_order_relaxed);
                st->az.store((float)(gravity.z + user.z), std::memory_order_relaxed);
                st->gx.store((float)rate.x, std::memory_order_relaxed);
                st->gy.store((float)rate.y, std::memory_order_relaxed);
                st->gz.store((float)rate.z, std::memory_order_relaxed);
                st->has_motion.store(true, std::memory_order_relaxed);
            };
            motion.sensorsActive = YES;
        } else {
            st->has_motion.store(false, std::memory_order_relaxed);
        }
    } else {
        st->has_motion.store(false, std::memory_order_relaxed);
    }
}

/// Translate GameController state into a Switch Pro Controller HID report.
static ns::HIDReport map_gc_to_switch(const GamepadState& st) {
    ns::HIDReport r; r.reset();

    if (st.btn_a.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_B;
    if (st.btn_b.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_A;
    if (st.btn_x.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_Y;
    if (st.btn_y.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_X;

    if (st.btn_l.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_L;
    if (st.btn_r.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_R;
    if (st.zl.load(std::memory_order_relaxed) > 0.5f) r.buttons |= ns::BTN_ZL;
    if (st.zr.load(std::memory_order_relaxed) > 0.5f) r.buttons |= ns::BTN_ZR;

    bool plus  = st.btn_menu.load(std::memory_order_relaxed);
    bool minus = st.btn_options.load(std::memory_order_relaxed);
    if (plus)  r.buttons |= ns::BTN_PLUS;
    if (minus) r.buttons |= ns::BTN_MINUS;

    bool ls = st.btn_lstick.load(std::memory_order_relaxed);
    bool rs = st.btn_rstick.load(std::memory_order_relaxed);
    if (ls) r.buttons |= ns::BTN_LSTICK;
    if (rs) r.buttons |= ns::BTN_RSTICK;
    if (ls && rs) { r.buttons |= ns::BTN_HOME; r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK); }
    if (plus && minus) { r.buttons |= ns::BTN_CAPTURE; r.buttons &= ~(ns::BTN_PLUS | ns::BTN_MINUS); }

    bool up = st.dpad_up.load(std::memory_order_relaxed), down = st.dpad_down.load(std::memory_order_relaxed);
    bool left = st.dpad_left.load(std::memory_order_relaxed), right = st.dpad_right.load(std::memory_order_relaxed);
    if      (up   && right) r.hat = ns::HAT_NE;
    else if (up   && left)  r.hat = ns::HAT_NW;
    else if (down && right) r.hat = ns::HAT_SE;
    else if (down && left)  r.hat = ns::HAT_SW;
    else if (up)            r.hat = ns::HAT_N;
    else if (down)          r.hat = ns::HAT_S;
    else if (left)          r.hat = ns::HAT_W;
    else if (right)         r.hat = ns::HAT_E;

    r.lx = float_to_byte(st.lx.load(std::memory_order_relaxed), false);
    r.ly = float_to_byte(st.ly.load(std::memory_order_relaxed), true);
    r.rx = float_to_byte(st.rx.load(std::memory_order_relaxed), false);
    r.ry = float_to_byte(st.ry.load(std::memory_order_relaxed), true);

    return r;
}

// ── Keyboard Binding Support ──────────────────────────────────────────────
struct KeyBindings {
    std::unordered_map<std::string, std::string> map;
    int mode = 0; // 0=off, 1=single, 2=override

    static std::unordered_map<std::string, std::string> defaults() {
        return {
            {"Y","Z"}, {"B","X"}, {"A","V"}, {"X","C"},
            {"L","Q"}, {"R","E"}, {"ZL","1"}, {"ZR","2"},
            {"MINUS","3"}, {"PLUS","4"},
            {"LSTICK","LSHIFT"}, {"RSTICK","RSHIFT"},
            {"HOME","HOME"}, {"CAPTURE","SNAPSHOT"},
            {"LSTICK_UP","W"}, {"LSTICK_DOWN","S"},
            {"LSTICK_LEFT","A"}, {"LSTICK_RIGHT","D"},
            {"RSTICK_UP","I"}, {"RSTICK_DOWN","K"},
            {"RSTICK_LEFT","J"}, {"RSTICK_RIGHT","L"},
            {"DPAD_UP","UP"}, {"DPAD_DOWN","DOWN"},
            {"DPAD_LEFT","LEFT"}, {"DPAD_RIGHT","RIGHT"}
        };
    }

    std::string get_bindings_path() const {
        char buf[1024];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0) {
            std::string p(buf);
            size_t pos = p.find_last_of('/');
            return (pos != std::string::npos ? p.substr(0, pos) : ".") + "/bindings.json";
        }
        return "./bindings.json";
    }

    void load_or_create() {
        std::string path = get_bindings_path();
        FILE* f = fopen(path.c_str(), "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len > 0) {
                std::string content((size_t)len, '\0');
                fread(&content[0], 1, (size_t)len, f);
                size_t pos = 0;
                while ((pos = content.find('"', pos)) != std::string::npos) {
                    size_t ks = pos + 1, ke = content.find('"', ks);
                    if (ke == std::string::npos) break;
                    std::string k = content.substr(ks, ke - ks);
                    pos = content.find('"', ke + 1);
                    if (pos == std::string::npos) break;
                    size_t vs = pos + 1, ve = content.find('"', vs);
                    if (ve == std::string::npos) break;
                    map[k] = content.substr(vs, ve - vs);
                    pos = ve + 1;
                }
            }
            fclose(f);
        }
        if (map.empty()) {
            map = defaults();
            f = fopen(path.c_str(), "w");
            if (f) {
                std::string json = "{\n";
                size_t i = 0;
                for (auto& [k, v] : map) {
                    json += "    \"" + k + "\": \"" + v + "\"";
                    if (++i < map.size()) json += ",";
                    json += "\n";
                }
                json += "}\n";
                fputs(json.c_str(), f);
                fclose(f);
            }
            std::cout << "Created default bindings: " << path << "\n";
        }
    }

    CGEventSourceRef src;

    KeyBindings() {
        src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    }

    ~KeyBindings() {
        if (src) CFRelease(src);
    }

    // Poll keyboard via CoreGraphics and fill HIDReport for player 1
    void apply(ns::HIDReport& rep) const {
        if (!src) return;

        auto is_down = [this](const std::string& name) -> bool {
            // Map key names to CGKeyCode
            struct KeyMap { const char* n; CGKeyCode code; };
            static const KeyMap kmap[] = {
                {"A",0x00}, {"B",0x0B}, {"C",0x08}, {"D",0x02},
                {"E",0x0E}, {"F",0x03}, {"G",0x05}, {"H",0x04},
                {"I",0x22}, {"J",0x26}, {"K",0x28}, {"L",0x25},
                {"M",0x2E}, {"N",0x2D}, {"O",0x1F}, {"P",0x23},
                {"Q",0x0C}, {"R",0x0F}, {"S",0x01}, {"T",0x11},
                {"U",0x20}, {"V",0x09}, {"W",0x0D}, {"X",0x07},
                {"Y",0x10}, {"Z",0x06},
                {"0",0x1D}, {"1",0x12}, {"2",0x13}, {"3",0x14},
                {"4",0x15}, {"5",0x17}, {"6",0x16}, {"7",0x1A},
                {"8",0x1C}, {"9",0x19},
                {"UP",0x7E}, {"DOWN",0x7D}, {"LEFT",0x7B}, {"RIGHT",0x7C},
                {"LSHIFT",0x38}, {"RSHIFT",0x3C},
                {"LCTRL",0x3B}, {"RCTRL",0x3E},
                {"LALT",0x3A}, {"RALT",0x3D},
                {"SPACE",0x31}, {"ENTER",0x24}, {"TAB",0x30},
                {"ESC",0x35}, {"BACKSPACE",0x33},
                {"F1",0x7A}, {"F2",0x78}, {"F3",0x63}, {"F4",0x76},
                {"F5",0x60}, {"F6",0x61}, {"F7",0x62}, {"F8",0x64},
                {"F9",0x65}, {"F10",0x6D}, {"F11",0x67}, {"F12",0x6F},
                {"HOME",0x73}, {"SNAPSHOT",0x69},
            };
            for (auto& km : kmap)
                if (name == km.n) return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, km.code);
            return false;
        };

        auto get_key = [&](const std::string& btn) -> std::string {
            auto it = map.find(btn);
            return it != map.end() ? it->second : "";
        };

        std::string k;

        k = get_key("Y");      if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_Y;
        k = get_key("B");      if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_B;
        k = get_key("A");      if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_A;
        k = get_key("X");      if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_X;
        k = get_key("L");      if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_L;
        k = get_key("R");      if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_R;
        k = get_key("ZL");     if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_ZL;
        k = get_key("ZR");     if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_ZR;
        k = get_key("MINUS");  if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_MINUS;
        k = get_key("PLUS");   if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_PLUS;
        k = get_key("LSTICK"); if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_LSTICK;
        k = get_key("RSTICK"); if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_RSTICK;
        k = get_key("HOME");   if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_HOME;
        k = get_key("CAPTURE"); if (!k.empty() && is_down(k)) rep.buttons |= ns::BTN_CAPTURE;

        bool up = false, down = false, left = false, right = false;
        k = get_key("DPAD_UP");    if (!k.empty()) up    = is_down(k);
        k = get_key("DPAD_DOWN");  if (!k.empty()) down  = is_down(k);
        k = get_key("DPAD_LEFT");  if (!k.empty()) left  = is_down(k);
        k = get_key("DPAD_RIGHT"); if (!k.empty()) right = is_down(k);

        if (up && right) rep.hat = ns::HAT_NE;
        else if (up && left) rep.hat = ns::HAT_NW;
        else if (down && right) rep.hat = ns::HAT_SE;
        else if (down && left) rep.hat = ns::HAT_SW;
        else if (up) rep.hat = ns::HAT_N;
        else if (down) rep.hat = ns::HAT_S;
        else if (left) rep.hat = ns::HAT_W;
        else if (right) rep.hat = ns::HAT_E;

        // Left stick axes (only center in single mode)
        auto lsu = get_key("LSTICK_UP"), lsd = get_key("LSTICK_DOWN");
        auto lsl = get_key("LSTICK_LEFT"), lsr = get_key("LSTICK_RIGHT");
        bool lsu_dn = !lsu.empty() && is_down(lsu);
        bool lsd_dn = !lsd.empty() && is_down(lsd);
        bool lsl_dn = !lsl.empty() && is_down(lsl);
        bool lsr_dn = !lsr.empty() && is_down(lsr);
        if (lsl_dn && !lsr_dn) rep.lx = 0;
        else if (lsr_dn && !lsl_dn) rep.lx = 255;
        else if (mode != 2) rep.lx = 128;
        if (lsu_dn && !lsd_dn) rep.ly = 0;
        else if (lsd_dn && !lsu_dn) rep.ly = 255;
        else if (mode != 2) rep.ly = 128;

        // Right stick axes (only center in single mode)
        auto rsu = get_key("RSTICK_UP"), rsd = get_key("RSTICK_DOWN");
        auto rsl = get_key("RSTICK_LEFT"), rsr = get_key("RSTICK_RIGHT");
        bool rsu_dn = !rsu.empty() && is_down(rsu);
        bool rsd_dn = !rsd.empty() && is_down(rsd);
        bool rsl_dn = !rsl.empty() && is_down(rsl);
        bool rsr_dn = !rsr.empty() && is_down(rsr);
        if (rsl_dn && !rsr_dn) rep.rx = 0;
        else if (rsr_dn && !rsl_dn) rep.rx = 255;
        else if (mode != 2) rep.rx = 128;
        if (rsu_dn && !rsd_dn) rep.ry = 0;
        else if (rsd_dn && !rsu_dn) rep.ry = 255;
        else if (mode != 2) rep.ry = 128;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  UDP rumble -> GameController haptics
// ─────────────────────────────────────────────────────────────────────────────
static void stop_haptics_for_controller_on_main(int ctrl_idx, bool release_engine) {
    if (ctrl_idx < 0 || ctrl_idx >= MAX_SLOTS) return;
    if (@available(macOS 11.0, *)) {
        NSError* err = nil;
        if (g_haptic_players[ctrl_idx]) {
            [g_haptic_players[ctrl_idx] stopAtTime:0 error:&err];
            [g_haptic_players[ctrl_idx] release];
            g_haptic_players[ctrl_idx] = nil;
        }
        if (release_engine && g_haptic_engines[ctrl_idx]) {
            [g_haptic_engines[ctrl_idx] stopWithCompletionHandler:nil];
            [g_haptic_engines[ctrl_idx] release];
            g_haptic_engines[ctrl_idx] = nil;
        }
    }
}

static void set_controller_rumble_async(int ctrl_idx, uint8_t low, uint8_t high, uint64_t duration_us) {
    if (ctrl_idx < 0 || ctrl_idx >= MAX_SLOTS) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (@available(macOS 11.0, *)) {
            GCController* ctrl = g_controllers[ctrl_idx];
            if (!ctrl || !ctrl.haptics) return;

            const bool neutral = (low == 0 && high == 0 || duration_us == 0);
            if (neutral) {
                stop_haptics_for_controller_on_main(ctrl_idx, false);
                return;
            }

            CHHapticEngine* engine = g_haptic_engines[ctrl_idx];
            if (!engine) {
                engine = [ctrl.haptics createEngineWithLocality:GCHapticsLocalityDefault];
                if (!engine) return;
                NSError* startErr = nil;
                if (![engine startAndReturnError:&startErr]) {
                    std::cerr << "GameController haptics failed to start for P" << (ctrl_idx + 1)
                              << ": " << (startErr ? startErr.localizedDescription.UTF8String : "unknown") << "\n";
                    [engine release];
                    return;
                }
                g_haptic_engines[ctrl_idx] = engine;
            }

            stop_haptics_for_controller_on_main(ctrl_idx, false);

            const float intensity = std::max((float)low, (float)high) / 255.0f;
            const float sharpness = (low || high) ? ((float)high / (float)std::max<int>(1, low + high)) : 0.0f;
            const NSTimeInterval duration = std::max<NSTimeInterval>(0.25, (NSTimeInterval)duration_us / 1000000.0);

            NSError* err = nil;
            CHHapticEventParameter* pIntensity = [[[CHHapticEventParameter alloc]
                initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity] autorelease];
            CHHapticEventParameter* pSharpness = [[[CHHapticEventParameter alloc]
                initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness] autorelease];
            CHHapticEvent* event = [[[CHHapticEvent alloc]
                initWithEventType:CHHapticEventTypeHapticContinuous
                parameters:@[pIntensity, pSharpness]
                relativeTime:0
                duration:duration] autorelease];
            CHHapticPattern* pattern = [[[CHHapticPattern alloc]
                initWithEvents:@[event] parameters:@[] error:&err] autorelease];
            if (!pattern || err) {
                std::cerr << "GameController haptic pattern failed for P" << (ctrl_idx + 1)
                          << ": " << (err ? err.localizedDescription.UTF8String : "unknown") << "\n";
                return;
            }

            id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&err];
            if (!player || err) {
                std::cerr << "GameController haptic player failed for P" << (ctrl_idx + 1)
                          << ": " << (err ? err.localizedDescription.UTF8String : "unknown") << "\n";
                return;
            }
            if (![player startAtTime:0 error:&err]) {
                std::cerr << "GameController haptic start failed for P" << (ctrl_idx + 1)
                          << ": " << (err ? err.localizedDescription.UTF8String : "unknown") << "\n";
                [player release];
                return;
            }
            g_haptic_players[ctrl_idx] = player;
        }
    });
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

    void set_output(int slot, uint8_t low, uint8_t high, int ctrl_idx) {
        if (states[slot].last_controller != -1 && states[slot].last_controller != ctrl_idx)
            set_controller_rumble_async(states[slot].last_controller, 0, 0, 0);
        if (ctrl_idx >= 0)
            set_controller_rumble_async(ctrl_idx, low, high, (low || high) ? 250000ULL : 0ULL);
        states[slot].last_controller = ctrl_idx;
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
    // Single-instance lock
    int lock_fd = open("/tmp/ns-gamepad.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        std::cerr << "Another instance is already running.\n";
        if (lock_fd >= 0) close(lock_fd);
        return 1;
    }

    std::string host;
    int port = ns::DEFAULT_PORT;
    bool macro_mode = false;
    std::string macro_path;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--legacy") == 0) {
            legacy_udp = true;
        } else if ((strcmp(argv[i], "--macro") == 0 || strcmp(argv[i], "--upload-macro") == 0 || strcmp(argv[i], "--server-macro") == 0) && i + 1 < argc) {
            macro_mode = true;
            macro_path = argv[++i];
        } else if (strcmp(argv[i], "-k") == 0) {
            keyboard_mode = 1;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                if (strcmp(argv[i+1], "override") == 0) keyboard_mode = 2;
                i++;
            }
        } else if (host.empty()) {
            host = argv[i];
            size_t colon = host.find(':');
            if (colon != std::string::npos) {
                port = std::atoi(host.c_str() + colon + 1);
                if (port < 1 || port > 65535) {
                    std::cerr << "Invalid port: " << port << " (must be 1–65535)\n";
                    close(lock_fd); return 1;
                }
                host.resize(colon);
            }
        }
    }

    if (host.empty()) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]] [--legacy] [--macro file.json [--upload-macro file.json]]\n";
        std::cerr << "  -k        Enable keyboard mode (default: single)\n";
        std::cerr << "  --legacy  Send old input-only UDP packets; disables UDP rumble/gyro\n";
        return 1;
    }

    KeyBindings kb;
    if (keyboard_mode) {
        kb.load_or_create();
        kb.mode = keyboard_mode;
        std::cout << "Keyboard mode enabled (" << (keyboard_mode == 1 ? "single" : "override") << ") - ";
        std::cout << (keyboard_mode == 1 ? "replaces" : "augments") << " Player 1\n";
    }

    if (legacy_udp) {
        std::cout << "Legacy UDP mode: input only. UDP rumble and gyro are disabled.\n";
    } else {
        std::cout << "Extended UDP mode: rumble replies + GameController motion enabled.\n";
    }

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    // ── UDP socket ────────────────────────────────────────────────────────────
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create UDP socket.\n";
        return 1;
    }
    int sock_flags = fcntl(sock, F_GETFL, 0);
    if (sock_flags >= 0) fcntl(sock, F_SETFL, sock_flags | O_NONBLOCK);

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        std::cerr << "Cannot resolve address: " << host << "\n";
        close(sock); return 1;
    }

    sockaddr_in dest{};
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);


    if (macro_mode) {
        std::string macro_raw = macro_read_file(macro_path);
        if (macro_raw.empty()) {
            std::cerr << "Macro file is empty or cannot be read: " << macro_path << "\n";
#ifdef _WIN32
            closesocket(sock); WSACleanup(); timeEndPeriod(1);
#else
            close(sock);
#endif
            return 1;
        }
        auto macro_steps_for_wait = macro_parse_text(macro_raw);
        if (macro_steps_for_wait.empty()) {
            std::cerr << "Macro file has no usable commands: " << macro_path << "\n";
#ifdef _WIN32
            closesocket(sock); WSACleanup(); timeEndPeriod(1);
#else
            close(sock);
#endif
            return 1;
        }
        bool sent = send_macro_udp_packet(sock, dest, hmac_key, macro_raw, 0);
        std::cout << (sent ? "Uploaded server-side macro to P1.\n" : "Failed to upload server-side macro.\n");
        std::this_thread::sleep_for(std::chrono::milliseconds((int)macro_total_ms(macro_steps_for_wait) + 180));
#ifdef _WIN32
        closesocket(sock); WSACleanup(); timeEndPeriod(1);
#else
        close(sock);
#endif
        return sent ? 0 : 1;
    }

    std::cout << "Started... Press Ctrl+C to stop\n";

    setpriority(PRIO_PROCESS, 0, -20);

    // ── Dedicated UDP sender thread ───────────────────────────────────────────
    std::thread sender([&]() {
        uint32_t seq = 0;
        RumbleManager rumble;
        std::vector<MacroStep> macro_steps;
        uint64_t macro_start_us = 0;
        bool macro_stop_after_send = false;
        if (macro_mode) {
            macro_steps = macro_load_file(macro_path);
            if (macro_steps.empty()) { std::cerr << "Macro file has no usable commands: " << macro_path << "\n"; g_running.store(false); return; }
            macro_start_us = ns::now_us();
            std::cout << "Macro mode: executing " << macro_steps.size() << " steps on P1, then exiting.\n";
        }

        while (g_running.load(std::memory_order_relaxed)) {
            ns::HIDReport logical_reports[4];
            ns::MotionReport logical_motion[4];
            bool present[4] = {false, false, false, false};
            bool has_motion[4] = {false, false, false, false};
            int controller_for_slot[4] = {-1, -1, -1, -1};
            for (int i = 0; i < 4; ++i) {
                logical_reports[i].reset();
                logical_motion[i].reset();
            }

            int active_count = 0;
            bool c1 = false, c2 = false, c3 = false, c4 = false;
            for (int i = 0; i < MAX_SLOTS; ++i) {
                if (!g_slot_active[i].load(std::memory_order_relaxed)) continue;
                logical_reports[i] = map_gc_to_switch(g_states[i]);
                present[i] = true;
                controller_for_slot[i] = i;
                if (g_states[i].has_motion.load(std::memory_order_relaxed)) {
                    logical_motion[i] = map_gc_motion_to_switch(g_states[i]);
                    has_motion[i] = true;
                }
                active_count++;
                if (i == 0) c1 = true;
                else if (i == 1) c2 = true;
                else if (i == 2) c3 = true;
                else if (i == 3) c4 = true;
            }

            // Keyboard overrides Player 1
            if (kb.mode == 1) {
                if (present[0]) {
                    int target = -1;
                    for (int s = 1; s < 4; ++s) {
                        if (!present[s]) { target = s; break; }
                    }
                    if (target >= 0) {
                        logical_reports[target] = logical_reports[0];
                        logical_motion[target] = logical_motion[0];
                        has_motion[target] = has_motion[0];
                        present[target] = true;
                        controller_for_slot[target] = controller_for_slot[0];
                        active_count++;
                    }
                }
                logical_reports[0].reset();
                logical_motion[0].reset();
                kb.apply(logical_reports[0]);
                present[0] = true;
                has_motion[0] = false;
                controller_for_slot[0] = -1;
                active_count = std::max(active_count, 1);
            } else if (kb.mode == 2) {
                kb.apply(logical_reports[0]);
                present[0] = true;
                active_count = std::max(active_count, 1);
            }


            // Macro mode overrides P1 and exits after the script plus a short neutral release.
            macro_stop_after_send = false;
            if (macro_mode) {
                uint64_t elapsed_ms = (ns::now_us() - macro_start_us) / 1000ULL;
                ns::HIDReport macro_rep;
                bool active_macro = macro_report_at(macro_steps, elapsed_ms, macro_rep);
                for (int i = 0; i < MAX_SLOTS; ++i) { logical_reports[i].reset(); logical_motion[i].reset(); present[i] = false; has_motion[i] = false; controller_for_slot[i] = -1; }
                logical_reports[0] = macro_rep;
                present[0] = true;
                active_count = 1;
                if (!active_macro && elapsed_ms > macro_total_ms(macro_steps) + 120) macro_stop_after_send = true;
            }

            if (legacy_udp) {
                ns::Packet pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.magic   = ns::PROTO_MAGIC;
                pkt.version = ns::PROTO_VERSION;
                pkt.flags   = ns::FLAG_NONE;
                pkt.seq     = seq++;
                pkt.ts_us   = ns::now_us();
                pkt.report.reset();
                ns::HIDReport* pads[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
                for (int i = 0; i < 4; ++i) *pads[i] = logical_reports[i];

                uint8_t full_hmac[32];
                hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
                sendto(sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
            } else {
                ExtendedUdpPacket pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.magic        = ns::PROTO_MAGIC;
                pkt.version      = ns::PROTO_VERSION;
                pkt.flags        = ns::FLAG_NONE;
                pkt.seq          = seq++;
                pkt.timestamp_us = ns::now_us();
                pkt.report.reset();

                ns::ExtendedHIDReport* pads[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
                for (int i = 0; i < 4; ++i)
                    fill_extended_pad(*pads[i], logical_reports[i], present[i], has_motion[i] ? &logical_motion[i] : nullptr);

                uint8_t full_hmac[32];
                hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, EXT_UDP_PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
                sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&dest, sizeof(dest));

                pump_udp_rumble(sock, rumble, controller_for_slot);
                rumble.update_timeouts(controller_for_slot);
            }

            if (macro_stop_after_send) { g_running.store(false, std::memory_order_relaxed); break; }
            auto interval = (active_count > 0)
                ? std::chrono::milliseconds(4)
                : std::chrono::milliseconds(500);
            std::this_thread::sleep_for(interval);
        }
    });

    // ── GameController notification setup (runs on main thread) ───────────────
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

    [nc addObserverForName:GCControllerDidConnectNotification
                    object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
        GCController* ctrl = (GCController*)note.object;
        if (!ctrl.extendedGamepad) return;

        // Prevent double-assignment: check if this controller is already mapped
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (g_controllers[i] == ctrl) return;
        }

        // Find a free slot
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (g_controllers[i] == nullptr) {
                g_controllers[i] = ctrl;
                reset_gamepad_state(g_states[i]);
                g_slot_active[i].store(true, std::memory_order_relaxed);
                NSString* name = ctrl.vendorName ?: @"Unknown Controller";
                int display_slot = i + 1;
                if (keyboard_mode == 1 && i == 0) {
                    int free_idx = 1;
                    while (free_idx < MAX_SLOTS && g_controllers[free_idx]) free_idx++;
                    display_slot = free_idx + 1;
                }
                std::cout << "Mapped '" << name.UTF8String << "' to local slot P" << display_slot << "\n";
                attach_handlers(ctrl, ctrl.extendedGamepad, &g_states[i]);
                break;
            }
        }
    }];

    [nc addObserverForName:GCControllerDidDisconnectNotification
                    object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
        GCController* ctrl = (GCController*)note.object;
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (g_controllers[i] == ctrl) {
                g_controllers[i] = nullptr;
                g_slot_active[i].store(false, std::memory_order_relaxed); // FIX 1
                reset_gamepad_state(g_states[i]);
                stop_haptics_for_controller_on_main(i, true);
                std::cout << "Controller in slot P" << (i + 1) << " disconnected.\n";
                break;
            }
        }
    }];

    // Handle controllers that were already connected when the program launched
    for (GCController* ctrl in [GCController controllers]) {
        if (!ctrl.extendedGamepad) continue;
        // Check not already assigned (notification may have fired during registration)
        bool already = false;
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (g_controllers[i] == ctrl) { already = true; break; }
        }
        if (already) continue;
        // Find a free slot
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (g_controllers[i] == nullptr) {
                g_controllers[i] = ctrl;
                reset_gamepad_state(g_states[i]);
                g_slot_active[i].store(true, std::memory_order_relaxed);
                NSString* name = ctrl.vendorName ?: @"Unknown Controller";
                int display_slot = i + 1;
                if (keyboard_mode == 1 && i == 0) {
                    int free_idx = 1;
                    while (free_idx < MAX_SLOTS && g_controllers[free_idx]) free_idx++;
                    display_slot = free_idx + 1;
                }
                std::cout << "Mapped '" << name.UTF8String << "' to local slot P" << display_slot << "\n";
                attach_handlers(ctrl, ctrl.extendedGamepad, &g_states[i]);
                break;
            }
        }
    }

    if ([GCController controllers].count == 0) {
        std::cout << "No controllers detected - waiting for connections...\n";
    }

    // ── Main NSRunLoop ─────────────────────────────────────────────────────────
    // GCController notifications require an active NSRunLoop on the main thread.
    while (g_running.load(std::memory_order_relaxed))
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];

    // ── Graceful shutdown ──────────────────────────────────────────────────────
    std::cout << "\nShutting down...\n";
    g_running.store(false, std::memory_order_relaxed);
    if (sender.joinable()) sender.join();
    for (int i = 0; i < MAX_SLOTS; ++i)
        stop_haptics_for_controller_on_main(i, true);
    close(sock);
    close(lock_fd);
    unlink("/tmp/ns-gamepad.lock");
    return 0;
}
