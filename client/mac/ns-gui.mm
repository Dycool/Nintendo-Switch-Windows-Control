/// ns-gui.mm  —  macOS Cocoa GUI frontend for the USB gamepad bridge
///
/// Features Smart Discovery: Automatically detects up to 4 local controllers
/// via Apple's GameController framework and seamlessly packs them into the UDP stream.
///
/// Build:
///   clang++ -std=c++17 -ObjC++ -fobjc-arc \
///           -framework Cocoa -framework GameController \
///           -framework CoreHaptics \
///           ns-gui.mm -o ns-gui
///
/// Usage:
///   ./ns-gui

#ifndef __APPLE__
#  error "ns-gui.mm is macOS-only."
#endif

// On macOS 10.15+, Bluetooth controllers may require the
// "Input Monitoring" permission under System Settings → Privacy & Security.

#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>

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
#include <cmath>
#include <map>
#include <unordered_map>
#include <mutex>
#include <cerrno>
#include <sstream>
#include <cctype>
#include <fstream>
#include <fcntl.h>
#include <dispatch/dispatch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/resource.h>
#include <CoreGraphics/CoreGraphics.h>
#include "../../server/rpi/include/sha256.h"

// Import shared wire protocol structures.
#include "../../server/rpi/include/protocol.hpp"
#include <stdexcept>
#include <limits>

// ── Keyboard mode constants ──
enum KeyboardMode {
    KB_OFF = 0,
    KB_SINGLE = 1,
    KB_OVERRIDE = 2
};

static constexpr int MAX_SLOTS = 4;
static constexpr uint8_t EXT_PAD_PRESENT = 0x01;

// ── Global macOS rumble / compatibility state ──────────────────────────────
// The haptics helpers below run outside AppDelegate, so they need a small
// process-wide map from logical player slot -> GCController/CoreHaptics state.
// Under ARC these Objective-C object references are retained automatically.
static bool g_legacyUdp = false; // hidden fallback: NSPC_LEGACY_UDP=1
static __strong GCController* g_rumbleControllers[MAX_SLOTS] = {};
static __strong CHHapticEngine* g_hapticEngines[MAX_SLOTS] = {};
static __strong id<CHHapticPatternPlayer> g_hapticPlayers[MAX_SLOTS] = {};

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
    static constexpr size_t MACRO_EXPANDED_STEP_LIMIT = 1000000;

    g_macro_last_error.clear();
    steps.clear();
    if (normalized) normalized->clear();

    std::string text, err;
    if (!macro_extract_commands_text(raw_text, text, err)) {
        macro_set_error(err);
        return false;
    }

    for (char& c : text) {
        if (c == '\n' || c == '\r') c = ';';
    }

    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t semi = text.find(';', pos);
        std::string part = macro_trim(text.substr(
            pos,
            semi == std::string::npos ? std::string::npos : semi - pos
        ));
        pos = (semi == std::string::npos) ? text.size() : semi + 1;

        // Allow readable macro files:
        //   # drift wiggle
        //   R+LSTICK_LEFT 450
        if (part.empty() || part[0] == '#') continue;

        parts.push_back(part);
        if (normalized) normalized->push_back(part);
    }

    if (parts.empty()) {
        macro_set_error("no valid macro commands found");
        return false;
    }

    auto append_segment = [&](const std::vector<MacroStep>& segment, uint32_t repeat_count) -> bool {
        if (segment.empty()) {
            macro_set_error("LOOP has no commands to repeat");
            return false;
        }
        if (repeat_count == 0) {
            macro_set_error("LOOP count must be greater than zero");
            return false;
        }
        if (segment.size() > 0 && repeat_count > MACRO_EXPANDED_STEP_LIMIT / segment.size()) {
            macro_set_error("macro expands to too many steps");
            return false;
        }
        if (steps.size() + segment.size() * (size_t)repeat_count > MACRO_EXPANDED_STEP_LIMIT) {
            macro_set_error("macro expands to too many steps");
            return false;
        }
        for (uint32_t r = 0; r < repeat_count; ++r) {
            steps.insert(steps.end(), segment.begin(), segment.end());
        }
        return true;
    };

    std::vector<MacroStep> segment;
    for (const std::string& part : parts) {
        size_t last_space = part.find_last_of(" \t");
        if (last_space == std::string::npos) {
            macro_set_error("missing duration in command: " + part);
            return false;
        }

        std::string cmd = macro_upper(macro_trim(part.substr(0, last_space)));
        if (cmd == "LOOP") {
            uint32_t repeat_count = 0;
            std::string count_s = macro_trim(part.substr(last_space + 1));
            if (!macro_parse_uint32_strict(count_s, repeat_count)) {
                macro_set_error("invalid LOOP count in command: " + part);
                return false;
            }

            // LOOP n repeats the block since the start or previous LOOP, n total times.
            if (!append_segment(segment, repeat_count)) return false;
            segment.clear();
            continue;
        }

        MacroStep st;
        if (!macro_parse_one_command(part, st, err)) {
            macro_set_error(err);
            return false;
        }
        segment.push_back(st);
    }

    if (!segment.empty()) {
        if (steps.size() + segment.size() > MACRO_EXPANDED_STEP_LIMIT) {
            macro_set_error("macro expands to too many steps");
            return false;
        }
        steps.insert(steps.end(), segment.begin(), segment.end());
    }

    if (steps.empty()) {
        macro_set_error("no valid macro commands found");
        return false;
    }
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

[[maybe_unused]] static std::vector<MacroStep> macro_load_file(const std::string& path) {
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

[[maybe_unused]] static uint64_t macro_total_ms(const std::vector<MacroStep>& steps) {
    uint64_t total = 0;
    for (const auto& s : steps) {
        if (UINT64_MAX - total < s.duration_ms) return UINT64_MAX;
        total += s.duration_ms;
    }
    return total;
}

[[maybe_unused]] static bool macro_report_at(const std::vector<MacroStep>& steps, uint64_t elapsed_ms, ns::HIDReport& out) {
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
//  Shared gamepad state
// ─────────────────────────────────────────────────────────────────────────────

/// Thread-safe container for controller input state.
struct GamepadState {
    std::atomic<bool>  btn_a{false}, btn_b{false}, btn_x{false}, btn_y{false};
    std::atomic<bool>  btn_l{false}, btn_r{false};
    std::atomic<float> zl{0.0f}, zr{0.0f};
    std::atomic<bool>  btn_menu{false}, btn_options{false};
    std::atomic<bool>  btn_lstick{false}, btn_rstick{false};
    std::atomic<bool>  dpad_up{false}, dpad_down{false}, dpad_left{false}, dpad_right{false};
    std::atomic<float> lx{0.0f}, ly{0.0f}, rx{0.0f}, ry{0.0f};

    // GameController motion values. Accel is in g-ish units, gyro is rad/s.
    std::atomic<bool>  has_motion{false};
    std::atomic<float> ax{0.0f}, ay{0.0f}, az{0.0f};
    std::atomic<float> gx{0.0f}, gy{0.0f}, gz{0.0f};

    void clear_inputs() {
        btn_a = false; btn_b = false; btn_x = false; btn_y = false;
        btn_l = false; btn_r = false; zl = 0.0f; zr = 0.0f;
        btn_menu = false; btn_options = false;
        btn_lstick = false; btn_rstick = false;
        dpad_up = false; dpad_down = false; dpad_left = false; dpad_right = false;
        lx = 0.0f; ly = 0.0f; rx = 0.0f; ry = 0.0f;
        has_motion = false;
        ax = 0.0f; ay = 0.0f; az = 0.0f;
        gx = 0.0f; gy = 0.0f; gz = 0.0f;
        // note: does not clear slotActive (managed separately)
    }

    GamepadState& operator=(const GamepadState& other) {
        if (this != &other) {
            btn_a.store(other.btn_a.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_b.store(other.btn_b.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_x.store(other.btn_x.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_y.store(other.btn_y.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_l.store(other.btn_l.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_r.store(other.btn_r.load(std::memory_order_relaxed), std::memory_order_relaxed);
            zl.store(other.zl.load(std::memory_order_relaxed), std::memory_order_relaxed);
            zr.store(other.zr.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_menu.store(other.btn_menu.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_options.store(other.btn_options.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_lstick.store(other.btn_lstick.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_rstick.store(other.btn_rstick.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dpad_up.store(other.dpad_up.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dpad_down.store(other.dpad_down.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dpad_left.store(other.dpad_left.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dpad_right.store(other.dpad_right.load(std::memory_order_relaxed), std::memory_order_relaxed);
            lx.store(other.lx.load(std::memory_order_relaxed), std::memory_order_relaxed);
            ly.store(other.ly.load(std::memory_order_relaxed), std::memory_order_relaxed);
            rx.store(other.rx.load(std::memory_order_relaxed), std::memory_order_relaxed);
            ry.store(other.ry.load(std::memory_order_relaxed), std::memory_order_relaxed);
            has_motion.store(other.has_motion.load(std::memory_order_relaxed), std::memory_order_relaxed);
            ax.store(other.ax.load(std::memory_order_relaxed), std::memory_order_relaxed);
            ay.store(other.ay.load(std::memory_order_relaxed), std::memory_order_relaxed);
            az.store(other.az.load(std::memory_order_relaxed), std::memory_order_relaxed);
            gx.store(other.gx.load(std::memory_order_relaxed), std::memory_order_relaxed);
            gy.store(other.gy.load(std::memory_order_relaxed), std::memory_order_relaxed);
            gz.store(other.gz.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Axis conversion
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t float_to_byte(float val, bool invert = false, float dz = 0.15f) {
    if (std::abs(val) < dz) return 128;
    int scaled;
    float range = 1.0f - dz;
    if (val > 0.0f) scaled = 128 + (int)(((val - dz) / range) * 127.0f);
    else scaled = 128 - (int)(((-val - dz) / range) * 128.0f);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? 255 - scaled : scaled);
}

static int16_t clamp_i16_from_float(float v) {
    if (v < -32768.0f) return -32768;
    if (v >  32767.0f) return  32767;
    return (int16_t)std::lrintf(v);
}

static ns::MotionReport map_gc_motion_to_console(const GamepadState& st) {
    ns::MotionReport m;
    m.reset();

    const float ax = st.ax.load(std::memory_order_relaxed);
    const float ay = st.ay.load(std::memory_order_relaxed);
    const float az = st.az.load(std::memory_order_relaxed);
    m.ax = clamp_i16_from_float(-ax * 4096.0f);
    m.ay = clamp_i16_from_float(-az * 4096.0f);
    m.az = clamp_i16_from_float( ay * 4096.0f);

    constexpr float RAD_TO_DEG = 57.29577951308232f;
    constexpr float GYRO_SCALE = RAD_TO_DEG * 16.384f;
    float gx = -st.gx.load(std::memory_order_relaxed);
    float gy = -st.gz.load(std::memory_order_relaxed);
    float gz =  st.gy.load(std::memory_order_relaxed);

    constexpr float GYRO_DEADZONE_RAD = 0.0f;
    if (std::fabs(gx) < GYRO_DEADZONE_RAD) gx = 0.0f;
    if (std::fabs(gy) < GYRO_DEADZONE_RAD) gy = 0.0f;
    if (std::fabs(gz) < GYRO_DEADZONE_RAD) gz = 0.0f;

    m.gx = clamp_i16_from_float(gx * GYRO_SCALE);
    m.gy = clamp_i16_from_float(gy * GYRO_SCALE);
    m.gz = clamp_i16_from_float(gz * GYRO_SCALE);
    return m;
}

static void set_pad_present_flag(ns::ExtendedHIDReport& r, bool present) {
    // Backend/web protocol uses byte 7 of ExtendedHIDReport as the pad-present flag.
    // This lets neutral-but-connected UDP pads claim a console slot and receive rumble.
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

static void send_udp_disconnect_packet(int sock, const sockaddr_in& dest, const uint8_t hmac_key[32], uint32_t seq, bool legacy_udp) {
    if (sock < 0) return;

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
        memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);

        for (int i = 0; i < 3; ++i) {
            sendto(sock, &pkt, ns::PACKET_SIZE, 0, reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
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
    memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);

    for (int i = 0; i < 3; ++i) {
        sendto(sock, &pkt, sizeof(pkt), 0, reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  GameController integration
// ─────────────────────────────────────────────────────────────────────────────

static void attach_handlers(GCController* ctrl, GCExtendedGamepad* gp, GamepadState* st) {
    st->clear_inputs();

    gp.buttonA.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { st->btn_a.store((bool)p, std::memory_order_relaxed); };
    gp.buttonB.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { st->btn_b.store((bool)p, std::memory_order_relaxed); };
    gp.buttonX.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { st->btn_x.store((bool)p, std::memory_order_relaxed); };
    gp.buttonY.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { st->btn_y.store((bool)p, std::memory_order_relaxed); };

    gp.leftShoulder.valueChangedHandler  = ^(GCControllerButtonInput*, float, BOOL p) { st->btn_l.store((bool)p, std::memory_order_relaxed); };
    gp.rightShoulder.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { st->btn_r.store((bool)p, std::memory_order_relaxed); };
    gp.leftTrigger.valueChangedHandler   = ^(GCControllerButtonInput*, float v, BOOL) { st->zl.store(v, std::memory_order_relaxed); };
    gp.rightTrigger.valueChangedHandler  = ^(GCControllerButtonInput*, float v, BOOL) { st->zr.store(v, std::memory_order_relaxed); };

    gp.buttonMenu.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { st->btn_menu.store((bool)p, std::memory_order_relaxed); };
    if (gp.buttonOptions) {
        gp.buttonOptions.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { st->btn_options.store((bool)p, std::memory_order_relaxed); };
    }

    if (gp.leftThumbstickButton) {
        gp.leftThumbstickButton.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { st->btn_lstick.store((bool)p, std::memory_order_relaxed); };
    }
    if (gp.rightThumbstickButton) {
        gp.rightThumbstickButton.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { st->btn_rstick.store((bool)p, std::memory_order_relaxed); };
    }

    gp.dpad.up.valueChangedHandler    = ^(GCControllerButtonInput*, float, BOOL p) { st->dpad_up.store((bool)p, std::memory_order_relaxed); };
    gp.dpad.down.valueChangedHandler  = ^(GCControllerButtonInput*, float, BOOL p) { st->dpad_down.store((bool)p, std::memory_order_relaxed); };
    gp.dpad.left.valueChangedHandler  = ^(GCControllerButtonInput*, float, BOOL p) { st->dpad_left.store((bool)p, std::memory_order_relaxed); };
    gp.dpad.right.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { st->dpad_right.store((bool)p, std::memory_order_relaxed); };

    gp.leftThumbstick.xAxis.valueChangedHandler  = ^(GCControllerAxisInput*, float v) { st->lx.store(v, std::memory_order_relaxed); };
    gp.leftThumbstick.yAxis.valueChangedHandler  = ^(GCControllerAxisInput*, float v) { st->ly.store(v, std::memory_order_relaxed); };
    gp.rightThumbstick.xAxis.valueChangedHandler = ^(GCControllerAxisInput*, float v) { st->rx.store(v, std::memory_order_relaxed); };
    gp.rightThumbstick.yAxis.valueChangedHandler = ^(GCControllerAxisInput*, float v) { st->ry.store(v, std::memory_order_relaxed); };

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

static ns::HIDReport map_gc_to_console(const GamepadState& st) {
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
    r.ly = float_to_byte(st.ly.load(std::memory_order_relaxed), true);   // inverted
    r.rx = float_to_byte(st.rx.load(std::memory_order_relaxed), false);
    r.ry = float_to_byte(st.ry.load(std::memory_order_relaxed), true);   // inverted

    return r;
}


// ─────────────────────────────────────────────────────────────────────────────
//  UDP rumble -> GameController haptics
// ─────────────────────────────────────────────────────────────────────────────
static void stop_haptics_for_controller_on_main(int ctrl_idx, bool release_engine) {
    if (ctrl_idx < 0 || ctrl_idx >= MAX_SLOTS) return;
    if (@available(macOS 11.0, *)) {
        NSError* err = nil;
        if (g_hapticPlayers[ctrl_idx]) {
            [g_hapticPlayers[ctrl_idx] stopAtTime:0 error:&err];
            g_hapticPlayers[ctrl_idx] = nil;
        }
        if (release_engine && g_hapticEngines[ctrl_idx]) {
            [g_hapticEngines[ctrl_idx] stopWithCompletionHandler:nil];
            g_hapticEngines[ctrl_idx] = nil;
        }
    }
}

static void set_controller_rumble_async(int ctrl_idx, uint8_t low, uint8_t high, uint64_t duration_us) {
    if (ctrl_idx < 0 || ctrl_idx >= MAX_SLOTS) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (@available(macOS 11.0, *)) {
            GCController* ctrl = g_rumbleControllers[ctrl_idx];
            if (!ctrl || !ctrl.haptics) return;

            const bool neutral = ((low == 0 && high == 0) || duration_us == 0);
            if (neutral) {
                stop_haptics_for_controller_on_main(ctrl_idx, false);
                return;
            }

            CHHapticEngine* engine = g_hapticEngines[ctrl_idx];
            if (!engine) {
                engine = [ctrl.haptics createEngineWithLocality:GCHapticsLocalityDefault];
                if (!engine) return;
                NSError* startErr = nil;
                if (![engine startAndReturnError:&startErr]) {
                    std::cerr << "GameController haptics failed to start for P" << (ctrl_idx + 1)
                              << ": " << (startErr ? startErr.localizedDescription.UTF8String : "unknown") << "\n";
                    return;
                }
                g_hapticEngines[ctrl_idx] = engine;
            }

            stop_haptics_for_controller_on_main(ctrl_idx, false);

            const float intensity = std::max((float)low, (float)high) / 255.0f;
            const float sharpness = (low || high) ? ((float)high / (float)std::max<int>(1, low + high)) : 0.0f;
            const NSTimeInterval duration = std::max<NSTimeInterval>(0.25, (NSTimeInterval)duration_us / 1000000.0);

            NSError* err = nil;
            CHHapticEventParameter* pIntensity = [[CHHapticEventParameter alloc]
                initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity];
            CHHapticEventParameter* pSharpness = [[CHHapticEventParameter alloc]
                initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness];
            CHHapticEvent* event = [[CHHapticEvent alloc]
                initWithEventType:CHHapticEventTypeHapticContinuous
                parameters:@[pIntensity, pSharpness]
                relativeTime:0
                duration:duration];
            CHHapticPattern* pattern = [[CHHapticPattern alloc]
                initWithEvents:@[event] parameters:@[] error:&err];
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
                return;
            }
            g_hapticPlayers[ctrl_idx] = player;
        }
    });
}

class RumbleManager {
public:
    void apply_precision_packet(const ns::PrecisionRumblePacket& rp, const int controller_for_slot[4]) {
        if (rp.subpad >= 4) return;
        // Normal-rumble build: treat PrecisionRumblePacket as a carrier for
        // already-decoded classic low/high magnitudes.
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
        for (int i = 0; i < 4; ++i) {
            if (states[i].last_controller >= 0)
                set_controller_rumble_async(states[i].last_controller, 0, 0, 0);
            states[i] = SlotState{};
        }
    }

private:
    struct SlotState {
        uint8_t low = 0, high = 0;
        uint64_t until_us = 0;
        uint64_t last_set_us = 0;
        int last_controller = -1;
        uint64_t suppress_classic_until_us = 0;
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
        if (n == (ssize_t)sizeof(ns::PrecisionRumblePacket)) {
            ns::PrecisionRumblePacket rp{};
            memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::PRECISION_RUMBLE_MAGIC)
                rumble.apply_precision_packet(rp, controller_for_slot);
        } else if (n == (ssize_t)sizeof(ns::RumblePacket)) {
            ns::RumblePacket rp{};
            memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::RUMBLE_MAGIC)
                rumble.apply_packet(rp, controller_for_slot);
        }
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Mac GUI macro storage / upload / live recording helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::mutex g_macroUploadMutex;
static std::string g_macroUploadPending;

static std::mutex g_macroRecordMutex;
static bool g_macroRecording = false;

static NSString* const kMacroDefaultsKey = @"macrosJson";
static NSString* const kMacroHotkeyDefaultsKey = @"macroHotkey";

static bool mac_key_down(const std::string& name);

struct MacroEntryMac {
    std::string name;
    std::string hotkey;
    std::string json;
};

static std::mutex g_macroEntriesMutex;
static std::vector<MacroEntryMac> g_macroEntriesMac;
static std::map<std::string, bool> g_macroHotkeyDownMac;
static std::atomic<bool> g_macroEntriesLoadedMac{false};

static std::string NormalizeMacroKeyMac(std::string s) {
    return macro_upper(macro_trim(s));
}

static bool JsonFindStringValueMac(const std::string& raw, const std::string& key, std::string& out) {
    out.clear();
    std::string quoted = "\"" + key + "\"";
    size_t pos = raw.find(quoted);
    if (pos == std::string::npos) return false;
    pos += quoted.size();
    macro_skip_ws(raw, pos);
    if (pos >= raw.size() || raw[pos] != ':') return false;
    ++pos;
    macro_skip_ws(raw, pos);
    std::string err;
    return macro_read_json_string_at(raw, pos, out, err);
}

static bool FindJsonArrayRangeForKeyMac(const std::string& raw, const std::string& key, size_t& begin, size_t& end) {
    begin = end = std::string::npos;
    std::string quoted = "\"" + key + "\"";
    size_t pos = raw.find(quoted);
    if (pos == std::string::npos) return false;
    pos += quoted.size();
    macro_skip_ws(raw, pos);
    if (pos >= raw.size() || raw[pos] != ':') return false;
    ++pos;
    macro_skip_ws(raw, pos);
    if (pos >= raw.size() || raw[pos] != '[') return false;
    begin = pos + 1;

    bool in_str = false, esc = false;
    int depth = 1;
    for (++pos; pos < raw.size(); ++pos) {
        char c = raw[pos];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '[') ++depth;
        else if (c == ']') {
            --depth;
            if (depth == 0) { end = pos; return true; }
        }
    }
    return false;
}

static std::vector<std::string> SplitTopLevelObjectsMac(const std::string& raw, size_t begin, size_t end) {
    std::vector<std::string> out;
    bool in_str = false, esc = false;
    int depth = 0;
    size_t obj_start = std::string::npos;
    for (size_t pos = begin; pos < end && pos < raw.size(); ++pos) {
        char c = raw[pos];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') {
            if (depth == 0) obj_start = pos;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && obj_start != std::string::npos) {
                out.push_back(raw.substr(obj_start, pos - obj_start + 1));
                obj_start = std::string::npos;
            }
        }
    }
    return out;
}

static std::string LoadSavedMacrosTextMac() {
    NSString* saved = [[NSUserDefaults standardUserDefaults] stringForKey:kMacroDefaultsKey];
    if (!saved) return std::string();
    std::string out([saved UTF8String]);
    if (out.size() > MACRO_JSON_MAX_BYTES) return std::string();
    return out;
}

static void ShowMacroErrorMac(const std::string& msg) {
    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert* a = [[NSAlert alloc] init];
        [a setMessageText:@"Macro validation"];
        [a setInformativeText:[NSString stringWithUTF8String:msg.c_str()]];
        [a addButtonWithTitle:@"OK"];
        [a runModal];
    });
}

static bool StartMacroTextMac(const std::string& txt);

static std::string LoadSavedMacroHotkeyMac() {
    NSString* saved = [[NSUserDefaults standardUserDefaults] stringForKey:kMacroHotkeyDefaultsKey];
    if (!saved) return std::string();
    return NormalizeMacroKeyMac(std::string([saved UTF8String]));
}

static int FindMacroEntryByNameMac(const std::string& name) {
    std::string wanted = macro_upper(macro_trim(name));
    if (wanted.empty()) return -1;
    for (int i = 0; i < (int)g_macroEntriesMac.size(); ++i) {
        if (macro_upper(g_macroEntriesMac[i].name) == wanted) return i;
    }
    return -1;
}

static std::string UniqueMacroNameMac(const std::string& base_raw) {
    std::string base = macro_trim(base_raw);
    if (base.empty()) base = "Recorded Macro";
    std::string name = base;
    int suffix = 2;
    while (FindMacroEntryByNameMac(name) >= 0) {
        name = base + " " + std::to_string(suffix++);
    }
    return name;
}

static void RebuildMacroHotkeyStateMac() {
    g_macroHotkeyDownMac.clear();
    for (const auto& e : g_macroEntriesMac) {
        std::string hk = NormalizeMacroKeyMac(e.hotkey);
        if (!hk.empty()) g_macroHotkeyDownMac[hk] = false;
    }
}

static std::string MacroPrettyJsonWithForcedNameMac(const std::string& raw_text, const std::string& forced_name) {
    std::vector<MacroStep> steps;
    std::vector<std::string> lines;
    if (!macro_validate_text(raw_text, steps, &lines)) lines = {"WAIT 200"};

    std::string name = macro_trim(forced_name).empty() ? "Macro" : macro_trim(forced_name);
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

static std::string MacroEntryToObjectJsonMac(const MacroEntryMac& e) {
    std::vector<MacroStep> steps;
    std::vector<std::string> lines;
    if (!macro_validate_text(e.json, steps, &lines)) lines = {"WAIT 200"};

    std::string name = macro_trim(e.name).empty() ? macro_extract_name_or_default(e.json, "Macro") : e.name;
    std::string out;
    out += "    {\n";
    out += "      \"name\": \"" + macro_escape_json(name) + "\",\n";
    out += "      \"hotkey\": \"" + macro_escape_json(NormalizeMacroKeyMac(e.hotkey)) + "\",\n";
    out += "      \"commands\": [\n";
    for (size_t i = 0; i < lines.size(); ++i) {
        out += "        \"" + macro_escape_json(lines[i]) + "\"";
        if (i + 1 < lines.size()) out += ",";
        out += "\n";
    }
    out += "      ]\n";
    out += "    }";
    return out;
}

static std::string MacroEntriesToJsonMac(const std::vector<MacroEntryMac>& entries) {
    std::string out;
    out += "{\n";
    out += "  \"macros\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        out += MacroEntryToObjectJsonMac(entries[i]);
        if (i + 1 < entries.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

static bool ParseMacroEntriesTextMac(const std::string& raw, std::vector<MacroEntryMac>& out, std::string& err) {
    out.clear();
    err.clear();
    if (raw.size() > MACRO_JSON_MAX_BYTES) { err = "macro JSON exceeds 50MB limit"; return false; }
    std::string t = macro_trim(raw);
    if (t.empty()) return true;

    size_t arr_begin = 0, arr_end = 0;
    if (FindJsonArrayRangeForKeyMac(t, "macros", arr_begin, arr_end)) {
        auto objects = SplitTopLevelObjectsMac(t, arr_begin, arr_end);
        for (const std::string& obj : objects) {
            std::string pretty;
            if (!macro_validate_to_pretty_json(obj, pretty, err, "Macro")) return false;
            MacroEntryMac e;
            e.json = pretty;
            e.name = macro_extract_name_or_default(obj, "Macro");
            JsonFindStringValueMac(obj, "hotkey", e.hotkey);
            e.hotkey = NormalizeMacroKeyMac(e.hotkey);
            out.push_back(e);
        }
        return true;
    }

    std::string pretty;
    if (!macro_validate_to_pretty_json(t, pretty, err, "Macro")) return false;
    MacroEntryMac e;
    e.json = pretty;
    e.name = macro_extract_name_or_default(t, "Macro");
    JsonFindStringValueMac(t, "hotkey", e.hotkey);
    e.hotkey = NormalizeMacroKeyMac(e.hotkey);
    out.push_back(e);
    return true;
}

static void LoadMacroEntriesMac() {
    std::string err;
    std::vector<MacroEntryMac> loaded;
    if (!ParseMacroEntriesTextMac(LoadSavedMacrosTextMac(), loaded, err)) loaded.clear();
    if (loaded.size() == 1 && loaded[0].hotkey.empty()) {
        loaded[0].hotkey = LoadSavedMacroHotkeyMac();
    }

    std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
    g_macroEntriesMac = std::move(loaded);
    RebuildMacroHotkeyStateMac();
    g_macroEntriesLoadedMac.store(true, std::memory_order_release);
}

static bool SaveMacroEntriesMac() {
    std::string json;
    {
        std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
        json = MacroEntriesToJsonMac(g_macroEntriesMac);
    }
    if (json.size() > MACRO_JSON_MAX_BYTES) {
        ShowMacroErrorMac("Macro JSON exceeds the 50MB limit.");
        return false;
    }
    NSString* ns = [NSString stringWithUTF8String:json.c_str()];
    [[NSUserDefaults standardUserDefaults] setObject:ns forKey:kMacroDefaultsKey];
    [[NSUserDefaults standardUserDefaults] synchronize];
    return true;
}

static bool ReadMacroURLLimitedMac(NSURL* url, std::string& out, std::string& err) {
    out.clear();
    err.clear();
    if (!url) { err = "No file selected."; return false; }

    NSNumber* fileSize = nil;
    NSError* sizeErr = nil;
    if ([url getResourceValue:&fileSize forKey:NSURLFileSizeKey error:&sizeErr] && fileSize) {
        unsigned long long bytes = [fileSize unsignedLongLongValue];
        if (bytes > MACRO_JSON_MAX_BYTES) {
            err = "Macro JSON exceeds the 50MB limit.";
            return false;
        }
    }

    NSError* readErr = nil;
    NSData* data = [NSData dataWithContentsOfURL:url options:0 error:&readErr];
    if (!data) {
        err = readErr ? std::string([[readErr localizedDescription] UTF8String]) : "Could not read macro file.";
        return false;
    }
    if ([data length] > MACRO_JSON_MAX_BYTES) {
        err = "Macro JSON exceeds the 50MB limit.";
        return false;
    }
    out.assign((const char*)[data bytes], (size_t)[data length]);
    return true;
}

static bool MacroHotkeyConflictsMac(const std::string& hotkey, const std::unordered_map<std::string, std::string>& bindings, std::string* conflict_name = nullptr) {
    std::string hk = NormalizeMacroKeyMac(hotkey);
    if (hk.empty()) return false;
    for (const auto& kv : bindings) {
        if (NormalizeMacroKeyMac(kv.second) == hk) {
            if (conflict_name) *conflict_name = kv.first;
            return true;
        }
    }
    return false;
}

static bool MacroEntryHotkeyConflictsMac(const std::string& hotkey, int skip_index, std::string* conflict_name = nullptr) {
    std::string hk = NormalizeMacroKeyMac(hotkey);
    if (hk.empty()) return false;
    for (int i = 0; i < (int)g_macroEntriesMac.size(); ++i) {
        if (i == skip_index) continue;
        if (NormalizeMacroKeyMac(g_macroEntriesMac[i].hotkey) == hk) {
            if (conflict_name) *conflict_name = g_macroEntriesMac[i].name.empty() ? "another macro" : g_macroEntriesMac[i].name;
            return true;
        }
    }
    return false;
}

static bool ValidateMacroHotkeyForEntryMac(const std::string& hotkey, int skip_index, const std::unordered_map<std::string, std::string>& bindings) {
    std::string conflict;
    if (MacroHotkeyConflictsMac(hotkey, bindings, &conflict)) {
        ShowMacroErrorMac("Macro keybind conflicts with keyboard binding: " + conflict);
        return false;
    }
    if (MacroEntryHotkeyConflictsMac(hotkey, skip_index, &conflict)) {
        ShowMacroErrorMac("Macro keybind is already used by: " + conflict);
        return false;
    }
    return true;
}

static bool UpsertMacroEntryMac(MacroEntryMac e, bool force_unique_name, const std::unordered_map<std::string, std::string>& bindings) {
    std::string pretty, err;
    if (!macro_validate_to_pretty_json(e.json, pretty, err, e.name.empty() ? "Macro" : e.name)) {
        ShowMacroErrorMac("Invalid macro: " + err);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
        e.name = macro_trim(e.name);
        if (e.name.empty()) e.name = macro_extract_name_or_default(pretty, "Macro");
        if (force_unique_name) e.name = UniqueMacroNameMac(e.name);
        e.hotkey = NormalizeMacroKeyMac(e.hotkey);
        e.json = MacroPrettyJsonWithForcedNameMac(pretty, e.name);

        int existing = force_unique_name ? -1 : FindMacroEntryByNameMac(e.name);
        if (!ValidateMacroHotkeyForEntryMac(e.hotkey, existing, bindings)) {
            e.hotkey.clear();
        }
        if (existing >= 0) g_macroEntriesMac[existing] = std::move(e);
        else g_macroEntriesMac.push_back(std::move(e));
        RebuildMacroHotkeyStateMac();
    }
    return SaveMacroEntriesMac();
}

static void PollMacroHotkeyMac(const std::unordered_map<std::string, std::string>& bindings) {
    if (!g_macroEntriesLoadedMac.load(std::memory_order_acquire)) LoadMacroEntriesMac();
    std::vector<std::string> to_run;
    {
        std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
        for (const auto& e : g_macroEntriesMac) {
            std::string hk = NormalizeMacroKeyMac(e.hotkey);
            if (hk.empty()) continue;
            if (MacroHotkeyConflictsMac(hk, bindings, nullptr)) continue;
            bool down = mac_key_down(hk);
            bool was_down = g_macroHotkeyDownMac[hk];
            g_macroHotkeyDownMac[hk] = down;
            if (down && !was_down) to_run.push_back(e.json);
        }
    }
    for (const auto& json : to_run) StartMacroTextMac(json);
}

static bool StartMacroTextMac(const std::string& txt) {
    std::vector<MacroStep> parsed;
    std::vector<std::string> normalized;
    if (!macro_validate_text(txt, parsed, &normalized)) {
        ShowMacroErrorMac("Invalid macro: " + macro_last_error());
        return false;
    }

    std::string pretty = macro_pretty_json(txt, "Macro");
    if (pretty.size() > MACRO_JSON_MAX_BYTES) {
        ShowMacroErrorMac("Macro JSON exceeds the 50MB limit.");
        return false;
    }

    // Modern servers execute macros server-side and merge them with live input.
    // Queue compact validated JSON for the sender thread to upload.
    std::lock_guard<std::mutex> lk(g_macroUploadMutex);
    g_macroUploadPending = pretty;
    return true;
}

struct MacroRecordFrameMac {
    uint16_t buttons = 0;
    uint8_t hat = ns::HAT_NEUTRAL;
    int8_t lx = 0;
    int8_t ly = 0;
    int8_t rx = 0;
    int8_t ry = 0;
};

static MacroRecordFrameMac g_macroRecordLastFrame{};
static bool g_macroRecordHaveFrame = false;
static bool g_macroRecordHasInput = false;
static uint64_t g_macroRecordLastChangeUs = 0;
static std::string g_macroRecordCommands;

static bool operator==(const MacroRecordFrameMac& a, const MacroRecordFrameMac& b) {
    return a.buttons == b.buttons && a.hat == b.hat &&
           a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
}

static bool operator!=(const MacroRecordFrameMac& a, const MacroRecordFrameMac& b) {
    return !(a == b);
}

static std::string MacroButtonsToTextMac(uint16_t buttons) {
    struct BtnName { uint16_t bit; const char* name; } names[] = {
        {ns::BTN_A, "A"}, {ns::BTN_B, "B"}, {ns::BTN_X, "X"}, {ns::BTN_Y, "Y"},
        {ns::BTN_L, "L"}, {ns::BTN_R, "R"}, {ns::BTN_ZL, "ZL"}, {ns::BTN_ZR, "ZR"},
        {ns::BTN_MINUS, "MINUS"}, {ns::BTN_PLUS, "PLUS"},
        {ns::BTN_LSTICK, "LSTICK"}, {ns::BTN_RSTICK, "RSTICK"},
        {ns::BTN_HOME, "HOME"}, {ns::BTN_CAPTURE, "CAPTURE"},
    };
    std::string out;
    for (const auto& n : names) {
        if (buttons & n.bit) {
            if (!out.empty()) out += "+";
            out += n.name;
        }
    }
    return out;
}

static MacroRecordFrameMac MacroRecordFrameFromReportMac(const ns::HIDReport& report) {
    auto axis_dir = [](uint8_t v) -> int8_t {
        if (v < 80) return -1;
        if (v > 176) return 1;
        return 0;
    };

    MacroRecordFrameMac f{};
    f.buttons = report.buttons;
    f.hat = report.hat;
    f.lx = axis_dir(report.lx);
    f.ly = axis_dir(report.ly);
    f.rx = axis_dir(report.rx);
    f.ry = axis_dir(report.ry);
    return f;
}

static void MacroAppendTokenMac(std::string& out, const char* token) {
    if (!out.empty()) out += "+";
    out += token;
}

static std::string MacroRecordFrameToTextMac(const MacroRecordFrameMac& f) {
    std::string out = MacroButtonsToTextMac(f.buttons);
    switch (f.hat) {
        case ns::HAT_N:  MacroAppendTokenMac(out, "DPAD_UP"); break;
        case ns::HAT_NE: MacroAppendTokenMac(out, "DPAD_UP"); MacroAppendTokenMac(out, "DPAD_RIGHT"); break;
        case ns::HAT_E:  MacroAppendTokenMac(out, "DPAD_RIGHT"); break;
        case ns::HAT_SE: MacroAppendTokenMac(out, "DPAD_DOWN"); MacroAppendTokenMac(out, "DPAD_RIGHT"); break;
        case ns::HAT_S:  MacroAppendTokenMac(out, "DPAD_DOWN"); break;
        case ns::HAT_SW: MacroAppendTokenMac(out, "DPAD_DOWN"); MacroAppendTokenMac(out, "DPAD_LEFT"); break;
        case ns::HAT_W:  MacroAppendTokenMac(out, "DPAD_LEFT"); break;
        case ns::HAT_NW: MacroAppendTokenMac(out, "DPAD_UP"); MacroAppendTokenMac(out, "DPAD_LEFT"); break;
        default: break;
    }
    if (f.lx < 0) MacroAppendTokenMac(out, "LSTICK_LEFT");
    else if (f.lx > 0) MacroAppendTokenMac(out, "LSTICK_RIGHT");
    if (f.ly < 0) MacroAppendTokenMac(out, "LSTICK_UP");
    else if (f.ly > 0) MacroAppendTokenMac(out, "LSTICK_DOWN");
    if (f.rx < 0) MacroAppendTokenMac(out, "RSTICK_LEFT");
    else if (f.rx > 0) MacroAppendTokenMac(out, "RSTICK_RIGHT");
    if (f.ry < 0) MacroAppendTokenMac(out, "RSTICK_UP");
    else if (f.ry > 0) MacroAppendTokenMac(out, "RSTICK_DOWN");
    return out;
}

static void MacroRecordAppendMacLocked(const MacroRecordFrameMac& frame, uint64_t duration_ms) {
    if (duration_ms < 10) return;
    if (!g_macroRecordCommands.empty()) g_macroRecordCommands += "; ";
    std::string combo = MacroRecordFrameToTextMac(frame);
    if (combo.empty()) {
        g_macroRecordCommands += "WAIT " + std::to_string(duration_ms);
    } else {
        g_macroRecordHasInput = true;
        g_macroRecordCommands += combo + " " + std::to_string(duration_ms);
    }
}

static void StartMacroRecordingMac() {
    std::lock_guard<std::mutex> lk(g_macroRecordMutex);
    g_macroRecording = true;
    g_macroRecordLastFrame = MacroRecordFrameMac{};
    g_macroRecordHaveFrame = false;
    g_macroRecordHasInput = false;
    g_macroRecordLastChangeUs = ns::now_us();
    g_macroRecordCommands.clear();
}

static bool IsMacroRecordingMac() {
    std::lock_guard<std::mutex> lk(g_macroRecordMutex);
    return g_macroRecording;
}

static std::string StopMacroRecordingMac() {
    std::lock_guard<std::mutex> lk(g_macroRecordMutex);
    if (g_macroRecording && g_macroRecordHaveFrame) {
        uint64_t now = ns::now_us();
        MacroRecordAppendMacLocked(g_macroRecordLastFrame, (now - g_macroRecordLastChangeUs) / 1000ULL);
    }
    g_macroRecording = false;
    g_macroRecordHaveFrame = false;
    if (!g_macroRecordHasInput) {
        g_macroRecordCommands.clear();
        return "";
    }
    std::string commands = g_macroRecordCommands;
    return macro_pretty_json(commands, "Recorded Macro");
}

static void SampleMacroRecordingMac(const ns::HIDReport& report) {
    std::lock_guard<std::mutex> lk(g_macroRecordMutex);
    if (!g_macroRecording) return;
    uint64_t now = ns::now_us();
    MacroRecordFrameMac frame = MacroRecordFrameFromReportMac(report);
    if (!g_macroRecordHaveFrame) {
        g_macroRecordLastFrame = frame;
        g_macroRecordHaveFrame = true;
        g_macroRecordLastChangeUs = now;
        return;
    }
    if (frame != g_macroRecordLastFrame) {
        MacroRecordAppendMacLocked(g_macroRecordLastFrame, (now - g_macroRecordLastChangeUs) / 1000ULL);
        g_macroRecordLastFrame = frame;
        g_macroRecordLastChangeUs = now;
    }
}

static bool ApplyMacroOverrideMac(ns::HIDReport[4], bool[4], bool[4], int[4]) {
    // Server-side macros merge with live input in the backend. The mac GUI keeps
    // this hook as a no-op so old generated call sites compile without causing
    // duplicate local playback on modern servers.
    return false;
}


// ─────────────────────────────────────────────────────────────────────────────
//  App Delegate (GUI and Core Logic)
// ─────────────────────────────────────────────────────────────────────────────

static int detect_server_udp_interval_ms(int sock, const sockaddr_in& dest, int fallback_ms, bool* out_is_hori) {
    if (out_is_hori) *out_is_hori = false;
    ns::ServerInfoProbe probe{};
    sendto(sock, reinterpret_cast<const char*>(&probe), sizeof(probe), 0,
           reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));

    const uint64_t deadline = ns::now_us() + 150000ULL;
    while (ns::now_us() < deadline) {
        ns::ServerInfoReply reply{};
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, &reply, sizeof(reply), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n == (ssize_t)sizeof(reply) &&
            reply.magic == ns::SERVER_INFO_MAGIC &&
            reply.version == ns::SERVER_INFO_VERSION &&
            reply.udp_interval_ms > 0) {
            if (out_is_hori) *out_is_hori = reply.backend == ns::SERVER_BACKEND_HORI;
            return fallback_ms;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return fallback_ms;
}

@class BindingsEditor;

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    @public
    NSTextField* ipField;
    NSButton* connectBtn;
    NSButton* macrosBtn;
    NSPopUpButton* kbCombo;
    NSButton* bindingsBtn;
    NSTextField* statusField;
    NSTextField* pktLabel;
    NSTextField* ctrlLabels[4];

    GamepadState  states[4];
    GCController* controllers[4];
    NSString* hwNames[4];
    std::atomic<bool> slotActive[4];

    std::thread senderThread;
    std::atomic<bool> connected;
    std::atomic<bool> senderRunning;
    std::atomic<int> keyboardMode;
    std::unordered_map<std::string, std::string> keyBindings;
    std::mutex macroUploadMutex;
    std::string macroUploadPending;
    int sock;
    uint8_t hmacKey[32];
    std::atomic<uint32_t> packetCount;
    BindingsEditor* _bindingsEditor;
    NSWindow* macroWindow;
    NSMutableArray* macroRowControls;
    NSButton* macroRecordBtn;
    NSTimer* macroRecordTimer;
    id macroKeyMonitor;
    NSInteger macroListeningIndex;
}
- (void)connect;
- (void)disconnect;
- (void)updateUI;
- (void)loadBindings;
- (void)saveBindings;
- (void)kbComboChanged;
- (void)openBindingsEditor;
- (void)openMacros;
- (void)refreshMacroRows;
- (void)macroRun:(id)sender;
- (void)macroKey:(id)sender;
- (void)macroRename:(id)sender;
- (void)macroExportOne:(id)sender;
- (void)macroDelete:(id)sender;
- (void)macroImport:(id)sender;
- (void)macroRecordToggle:(id)sender;
- (void)macroRecordTick:(id)sender;
- (void)macroExportAll:(id)sender;
- (void)macroClose:(id)sender;
@end

static std::unordered_map<std::string, std::string> default_key_bindings() {
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

static const std::vector<std::string> binding_keys = {
    "A","B","X","Y","L","R","ZL","ZR",
    "MINUS","PLUS","LSTICK","RSTICK",
    "HOME",
    "LSTICK_UP","LSTICK_DOWN","LSTICK_LEFT","LSTICK_RIGHT",
    "RSTICK_UP","RSTICK_DOWN","RSTICK_LEFT","RSTICK_RIGHT",
    "DPAD_UP","DPAD_DOWN","DPAD_LEFT","DPAD_RIGHT",
    "CAPTURE"
};

// ── Keyboard polling helpers (macOS) ──
static bool mac_key_down(const std::string& name) {
    struct { const char* n; CGKeyCode c; } kmap[] = {
        {"A",0x00},{"B",0x0B},{"C",0x08},{"D",0x02},{"E",0x0E},{"F",0x03},
        {"G",0x05},{"H",0x04},{"I",0x22},{"J",0x26},{"K",0x28},{"L",0x25},
        {"M",0x2E},{"N",0x2D},{"O",0x1F},{"P",0x23},{"Q",0x0C},{"R",0x0F},
        {"S",0x01},{"T",0x11},{"U",0x20},{"V",0x09},{"W",0x0D},{"X",0x07},
        {"Y",0x10},{"Z",0x06},
        {"0",0x1D},{"1",0x12},{"2",0x13},{"3",0x14},{"4",0x15},
        {"5",0x17},{"6",0x16},{"7",0x1A},{"8",0x1C},{"9",0x19},
        {"UP",0x7E},{"DOWN",0x7D},{"LEFT",0x7B},{"RIGHT",0x7C},
        {"LSHIFT",0x38},{"RSHIFT",0x3C},
        {"LCTRL",0x3B},{"RCTRL",0x3E},
        {"LALT",0x3A},{"RALT",0x3D},
        {"SPACE",0x31},{"ENTER",0x24},{"TAB",0x30},
        {"ESC",0x35},{"BACKSPACE",0x33},
        {"F1",0x7A},{"F2",0x78},{"F3",0x63},{"F4",0x76},
        {"F5",0x60},{"F6",0x61},{"F7",0x62},{"F8",0x64},
        {"F9",0x65},{"F10",0x6D},{"F11",0x67},{"F12",0x6F},
        {"HOME",0x73},{"SNAPSHOT",0x69},
    };
    for (auto& km : kmap)
        if (name == km.n) return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, km.c);
    return false;
}

static std::string MacEventKeyName(NSEvent* event) {
    switch ([event keyCode]) {
        case 0x00: return "A"; case 0x0B: return "B"; case 0x08: return "C"; case 0x02: return "D";
        case 0x0E: return "E"; case 0x03: return "F"; case 0x05: return "G"; case 0x04: return "H";
        case 0x22: return "I"; case 0x26: return "J"; case 0x28: return "K"; case 0x25: return "L";
        case 0x2E: return "M"; case 0x2D: return "N"; case 0x1F: return "O"; case 0x23: return "P";
        case 0x0C: return "Q"; case 0x0F: return "R"; case 0x01: return "S"; case 0x11: return "T";
        case 0x20: return "U"; case 0x09: return "V"; case 0x0D: return "W"; case 0x07: return "X";
        case 0x10: return "Y"; case 0x06: return "Z";
        case 0x1D: return "0"; case 0x12: return "1"; case 0x13: return "2"; case 0x14: return "3";
        case 0x15: return "4"; case 0x17: return "5"; case 0x16: return "6"; case 0x1A: return "7";
        case 0x1C: return "8"; case 0x19: return "9";
        case 0x7E: return "UP"; case 0x7D: return "DOWN"; case 0x7B: return "LEFT"; case 0x7C: return "RIGHT";
        case 0x38: return "LSHIFT"; case 0x3C: return "RSHIFT";
        case 0x3B: return "LCTRL"; case 0x3E: return "RCTRL";
        case 0x3A: return "LALT"; case 0x3D: return "RALT";
        case 0x31: return "SPACE"; case 0x24: return "ENTER"; case 0x30: return "TAB";
        case 0x35: return "ESC"; case 0x33: return "BACKSPACE";
        case 0x7A: return "F1"; case 0x78: return "F2"; case 0x63: return "F3"; case 0x76: return "F4";
        case 0x60: return "F5"; case 0x61: return "F6"; case 0x62: return "F7"; case 0x64: return "F8";
        case 0x65: return "F9"; case 0x6D: return "F10"; case 0x67: return "F11"; case 0x6F: return "F12";
        case 0x73: return "HOME"; case 0x69: return "SNAPSHOT";
        default: break;
    }
    return "";
}

static void apply_keyboard_to_report_mac(ns::HIDReport& rep, const std::unordered_map<std::string, std::string>& bindings, bool override_mode) {
    auto get = [&](const std::string& btn) -> std::string {
        auto it = bindings.find(btn);
        return it != bindings.end() ? it->second : "";
    };
    std::string k;
    k = get("Y");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_Y;
    k = get("B");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_B;
    k = get("A");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_A;
    k = get("X");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_X;
    k = get("L");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_L;
    k = get("R");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_R;
    k = get("ZL");     if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_ZL;
    k = get("ZR");     if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_ZR;
    k = get("MINUS");  if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_MINUS;
    k = get("PLUS");   if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_PLUS;
    k = get("LSTICK"); if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_LSTICK;
    k = get("RSTICK"); if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_RSTICK;
    k = get("HOME");   if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_HOME;
    k = get("CAPTURE"); if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_CAPTURE;
    bool up=false,down=false,left=false,right=false;
    k = get("DPAD_UP");    if (!k.empty()) up    = mac_key_down(k);
    k = get("DPAD_DOWN");  if (!k.empty()) down  = mac_key_down(k);
    k = get("DPAD_LEFT");  if (!k.empty()) left  = mac_key_down(k);
    k = get("DPAD_RIGHT"); if (!k.empty()) right = mac_key_down(k);
    if (up && right) rep.hat = ns::HAT_NE;
    else if (up && left) rep.hat = ns::HAT_NW;
    else if (down && right) rep.hat = ns::HAT_SE;
    else if (down && left) rep.hat = ns::HAT_SW;
    else if (up) rep.hat = ns::HAT_N;
    else if (down) rep.hat = ns::HAT_S;
    else if (left) rep.hat = ns::HAT_W;
    else if (right) rep.hat = ns::HAT_E;

    auto lsu = get("LSTICK_UP"), lsd = get("LSTICK_DOWN");
    auto lsl = get("LSTICK_LEFT"), lsr = get("LSTICK_RIGHT");
    bool lsu_dn = !lsu.empty() && mac_key_down(lsu);
    bool lsd_dn = !lsd.empty() && mac_key_down(lsd);
    bool lsl_dn = !lsl.empty() && mac_key_down(lsl);
    bool lsr_dn = !lsr.empty() && mac_key_down(lsr);
    if (lsl_dn && !lsr_dn) rep.lx = 0;
    else if (lsr_dn && !lsl_dn) rep.lx = 255;
    else if (!override_mode) rep.lx = 128;
    if (lsu_dn && !lsd_dn) rep.ly = 0;
    else if (lsd_dn && !lsu_dn) rep.ly = 255;
    else if (!override_mode) rep.ly = 128;

    auto rsu = get("RSTICK_UP"), rsd = get("RSTICK_DOWN");
    auto rsl = get("RSTICK_LEFT"), rsr = get("RSTICK_RIGHT");
    bool rsu_dn = !rsu.empty() && mac_key_down(rsu);
    bool rsd_dn = !rsd.empty() && mac_key_down(rsd);
    bool rsl_dn = !rsl.empty() && mac_key_down(rsl);
    bool rsr_dn = !rsr.empty() && mac_key_down(rsr);
    if (rsl_dn && !rsr_dn) rep.rx = 0;
    else if (rsr_dn && !rsl_dn) rep.rx = 255;
    else if (!override_mode) rep.rx = 128;
    if (rsu_dn && !rsd_dn) rep.ry = 0;
    else if (rsd_dn && !rsu_dn) rep.ry = 255;
    else if (!override_mode) rep.ry = 128;
}

// ── Bindings Editor Window Controller ──
@interface BindingsEditor : NSWindowController <NSWindowDelegate> {
    @public
    std::unordered_map<std::string, std::string> editBindings;
    std::vector<NSTextField*> keyLabels;
    int listeningIdx;
    BOOL setupMode;
    AppDelegate* parent;
}
- (instancetype)initWithBindings:(const std::unordered_map<std::string, std::string>&)bindings parent:(AppDelegate*)p;
- (void)cancel;
- (void)save;
- (void)changeClicked:(NSButton*)sender;
- (void)resetClicked;
- (void)setupClicked;
@end

// ── Custom NSView to capture key events for bindings editor ──
@class BindingsEditor;
@interface KeyCaptureView : NSView
@property (assign) BindingsEditor* editor;
@end

@implementation BindingsEditor

- (instancetype)initWithBindings:(const std::unordered_map<std::string, std::string>&)bindings parent:(AppDelegate*)p {
    self = [super init];
    if (self) {
        editBindings = bindings;
        parent = p;
        listeningIdx = -1;
        setupMode = NO;

        // 1. Expand window slightly to accommodate larger buttons and spacing
        NSRect frame = NSMakeRect(0, 0, 680, 480);
        NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        [win setTitle:@"Edit Key Bindings"];
        [win setDelegate:self];
        [win center];

        KeyCaptureView* view = [[KeyCaptureView alloc] initWithFrame:frame];
        [view setEditor:self];
        [win setContentView:view];
        
        // Start rendering lower from the top to give breathing room
        int y = (int)frame.size.height - 40;

        // 2. Adjust columns and expand btnW to 75 so "Change" fits
        int lx = 20;
        int rx = 350; // Shifted further right to make room for the wider left column
        int labelW = 115, keyW = 110, btnW = 75, gap = 5;
        int half = (int)binding_keys.size() / 2;
        
        for (int i = 0; i < half; i++) {
            int li = i, ri = i + half;
            
            // Left column
            NSTextField* ll = [[NSTextField alloc] initWithFrame:NSMakeRect(lx, y - 22, labelW, 20)];
            [ll setStringValue:[NSString stringWithUTF8String:binding_keys[li].c_str()]];
            [ll setBezeled:NO]; [ll setDrawsBackground:NO]; [ll setEditable:NO]; [ll setSelectable:NO];
            [ll setAlignment:NSTextAlignmentRight]; // Right-aligning labels looks much cleaner
            [view addSubview:ll];
            
            NSTextField* lv = [[NSTextField alloc] initWithFrame:NSMakeRect(lx + labelW + gap, y - 22, keyW, 20)];
            [lv setStringValue:[NSString stringWithUTF8String:editBindings[binding_keys[li]].c_str()]];
            [lv setBezeled:YES]; [lv setDrawsBackground:YES]; [lv setEditable:NO]; [lv setSelectable:NO];
            [lv setAlignment:NSTextAlignmentCenter];
            [view addSubview:lv];
            keyLabels.push_back(lv);
            
            NSButton* lc = [[NSButton alloc] initWithFrame:NSMakeRect(lx + labelW + gap + keyW + gap, y - 24, btnW, 24)];
            [lc setTitle:@"Change"]; [lc setBezelStyle:NSBezelStyleRounded];
            [lc setTag:(NSInteger)li]; [lc setTarget:self]; [lc setAction:@selector(changeClicked:)];
            [view addSubview:lc];
            
            // Right column
            NSTextField* rl = [[NSTextField alloc] initWithFrame:NSMakeRect(rx, y - 22, labelW, 20)];
            [rl setStringValue:[NSString stringWithUTF8String:binding_keys[ri].c_str()]];
            [rl setBezeled:NO]; [rl setDrawsBackground:NO]; [rl setEditable:NO]; [rl setSelectable:NO];
            [rl setAlignment:NSTextAlignmentRight];
            [view addSubview:rl];
            
            NSTextField* rv = [[NSTextField alloc] initWithFrame:NSMakeRect(rx + labelW + gap, y - 22, keyW, 20)];
            [rv setStringValue:[NSString stringWithUTF8String:editBindings[binding_keys[ri]].c_str()]];
            [rv setBezeled:YES]; [rv setDrawsBackground:YES]; [rv setEditable:NO]; [rv setSelectable:NO];
            [rv setAlignment:NSTextAlignmentCenter];
            [view addSubview:rv];
            keyLabels.push_back(rv);
            
            NSButton* rc = [[NSButton alloc] initWithFrame:NSMakeRect(rx + labelW + gap + keyW + gap, y - 24, btnW, 24)];
            [rc setTitle:@"Change"]; [rc setBezelStyle:NSBezelStyleRounded];
            [rc setTag:(NSInteger)ri]; [rc setTarget:self]; [rc setAction:@selector(changeClicked:)];
            [view addSubview:rc];
            
            y -= 28; // Increased from 26 for slightly better row breathing room
        }

        // 3. Move action buttons to a single clean row at the very bottom
        y -= 10;
        int aw = 85; 
        int ah = 28;
        int by = y - ah; 

        // Left Actions
        NSButton* cancelBtn = [[NSButton alloc] initWithFrame:NSMakeRect(lx, by, aw, ah)];
        [cancelBtn setTitle:@"Cancel"]; [cancelBtn setBezelStyle:NSBezelStyleRounded];
        [cancelBtn setTarget:self]; [cancelBtn setAction:@selector(cancel)];
        [view addSubview:cancelBtn];

        NSButton* saveBtn = [[NSButton alloc] initWithFrame:NSMakeRect(lx + aw + 10, by, aw, ah)];
        [saveBtn setTitle:@"Save"]; [saveBtn setBezelStyle:NSBezelStyleRounded];
        [saveBtn setTarget:self]; [saveBtn setAction:@selector(save)];
        [saveBtn setKeyEquivalent:@"\r"]; // Pressing Enter will trigger Save!
        [view addSubview:saveBtn];

        // Right Actions (anchored to the right edge of the right column)
        int rightEdge = rx + labelW + gap + keyW + gap + btnW; 
        
        NSButton* setupBtn = [[NSButton alloc] initWithFrame:NSMakeRect(rightEdge - aw, by, aw, ah)];
        [setupBtn setTitle:@"Setup"]; [setupBtn setBezelStyle:NSBezelStyleRounded];
        [setupBtn setTarget:self]; [setupBtn setAction:@selector(setupClicked)];
        [view addSubview:setupBtn];

        NSButton* resetBtn = [[NSButton alloc] initWithFrame:NSMakeRect(rightEdge - aw - 10 - aw, by, aw, ah)];
        [resetBtn setTitle:@"Reset"]; [resetBtn setBezelStyle:NSBezelStyleRounded];
        [resetBtn setTarget:self]; [resetBtn setAction:@selector(resetClicked)];
        [view addSubview:resetBtn];

        self.window = win;
    }
    return self;
}

- (void)changeClicked:(NSButton*)sender {
    setupMode = NO;
    int idx = (int)[sender tag];
    listeningIdx = idx;
    [keyLabels[idx] setStringValue:@"..."];
}

- (void)resetClicked {
    auto defs = default_key_bindings();
    for (size_t i = 0; i < binding_keys.size(); i++) {
        editBindings[binding_keys[i]] = defs[binding_keys[i]];
        [keyLabels[i] setStringValue:[NSString stringWithUTF8String:editBindings[binding_keys[i]].c_str()]];
    }
}

- (void)setupClicked {
    setupMode = YES;
    for (size_t i = 0; i < binding_keys.size(); i++) {
        editBindings[binding_keys[i]] = "";
        [keyLabels[i] setStringValue:i == 0 ? @"..." : @""];
    }
    listeningIdx = 0;
}

- (void)cancel {
    setupMode = NO;
    [self.window close];
}

- (void)save {
    parent->keyBindings = editBindings;
    [parent saveBindings];
    [self.window close];
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    return YES;
}

@end

@implementation KeyCaptureView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event {
    if (!self.editor) return;
    int lidx = self.editor->listeningIdx;
    if (lidx < 0) return;
    unsigned short kc = [event keyCode];
    if (kc == 0x35) { // ESC
        self.editor->editBindings[binding_keys[lidx]] = "";
        [self.editor->keyLabels[lidx] setStringValue:@""];
        if (self.editor->setupMode) {
            self.editor->listeningIdx++;
            if (self.editor->listeningIdx < (int)binding_keys.size()) {
                [self.editor->keyLabels[self.editor->listeningIdx] setStringValue:@"..."];
                return;
            }
        }
        self.editor->setupMode = NO;
        self.editor->listeningIdx = -1;
        return;
    }
    auto kc_to_name = [](unsigned short kc) -> std::string {
        struct { unsigned short k; const char* n; } map[] = {
            {0x00,"A"},{0x0B,"B"},{0x08,"C"},{0x02,"D"},{0x0E,"E"},{0x03,"F"},
            {0x05,"G"},{0x04,"H"},{0x22,"I"},{0x26,"J"},{0x28,"K"},{0x25,"L"},
            {0x2E,"M"},{0x2D,"N"},{0x1F,"O"},{0x23,"P"},{0x0C,"Q"},{0x0F,"R"},
            {0x01,"S"},{0x11,"T"},{0x20,"U"},{0x09,"V"},{0x0D,"W"},{0x07,"X"},
            {0x10,"Y"},{0x06,"Z"},
            {0x1D,"0"},{0x12,"1"},{0x13,"2"},{0x14,"3"},{0x15,"4"},
            {0x17,"5"},{0x16,"6"},{0x1A,"7"},{0x1C,"8"},{0x19,"9"},
            {0x7E,"UP"},{0x7D,"DOWN"},{0x7B,"LEFT"},{0x7C,"RIGHT"},
            {0x38,"LSHIFT"},{0x3C,"RSHIFT"},
            {0x3B,"LCTRL"},{0x3E,"RCTRL"},
            {0x3A,"LALT"},{0x3D,"RALT"},
            {0x31,"SPACE"},{0x24,"ENTER"},{0x30,"TAB"},{0x35,"ESC"},{0x33,"BACKSPACE"},
            {0x7A,"F1"},{0x78,"F2"},{0x63,"F3"},{0x76,"F4"},
            {0x60,"F5"},{0x61,"F6"},{0x62,"F7"},{0x64,"F8"},
            {0x65,"F9"},{0x6D,"F10"},{0x67,"F11"},{0x6F,"F12"},
            {0x73,"HOME"},{0x69,"SNAPSHOT"},
        };
        for (auto& m : map)
            if (kc == m.k) return m.n;
        return "";
    };
    std::string name = kc_to_name(kc);
    if (!name.empty()) {
        // In setup mode, skip already-bound keys
        if (self.editor->setupMode) {
            bool alreadyBound = false;
            for (auto& [k, v] : self.editor->editBindings) {
                if (k != binding_keys[lidx] && v == name) { alreadyBound = true; break; }
            }
            if (alreadyBound) return;
        }
        // Remove this key from any other binding
        for (auto& [k, v] : self.editor->editBindings) {
            if (v == name) { v = ""; break; }
        }
        self.editor->editBindings[binding_keys[lidx]] = name;
        [self.editor->keyLabels[lidx] setStringValue:[NSString stringWithUTF8String:name.c_str()]];
        // Update display for any cleared binding
        for (size_t i = 0; i < binding_keys.size(); i++) {
            if (self.editor->editBindings[binding_keys[i]].empty())
                [self.editor->keyLabels[i] setStringValue:@""];
        }
    }
    if (self.editor->setupMode) {
        self.editor->listeningIdx++;
        if (self.editor->listeningIdx < (int)binding_keys.size()) {
            [self.editor->keyLabels[self.editor->listeningIdx] setStringValue:@"..."];
            return;
        }
    }
    self.editor->setupMode = NO;
    self.editor->listeningIdx = -1;
}
@end

static NSString* MacroSafeFileNameMac(const std::string& raw_name) {
    std::string name = macro_trim(raw_name);
    if (name.empty()) name = "Macro";
    for (char& c : name) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) {
            c = '_';
        }
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty()) name = "Macro";
    if (name.size() > 180) name.resize(180);
    name += ".json";
    return [NSString stringWithUTF8String:name.c_str()];
}

static bool MacroWriteURLMac(NSURL* url, const std::string& text) {
    NSString* outText = [NSString stringWithUTF8String:text.c_str()];
    NSError* writeErr = nil;
    if (![outText writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&writeErr]) {
        ShowMacroErrorMac(writeErr ? std::string([[writeErr localizedDescription] UTF8String]) : "Could not export macro JSON.");
        return false;
    }
    return true;
}

@implementation AppDelegate

- (void)loadBindings {
    keyBindings = default_key_bindings();
    NSUserDefaults* defs = [NSUserDefaults standardUserDefaults];
    for (auto& [k, v] : keyBindings) {
        NSString* val = [defs stringForKey:[NSString stringWithUTF8String:("kb_" + k).c_str()]];
        if (val) keyBindings[k] = std::string([val UTF8String]);
    }
    NSNumber* savedMode = [defs objectForKey:@"keyboardMode"];
    if (savedMode) keyboardMode = [savedMode intValue];
}

- (void)saveBindings {
    NSUserDefaults* defs = [NSUserDefaults standardUserDefaults];
    for (auto& [k, v] : keyBindings)
        [defs setObject:[NSString stringWithUTF8String:v.c_str()] forKey:[NSString stringWithUTF8String:("kb_" + k).c_str()]];
    [defs setInteger:keyboardMode.load() forKey:@"keyboardMode"];
}

- (void)kbComboChanged {
    keyboardMode = (int)[kbCombo indexOfSelectedItem];
    if (keyboardMode.load() < 0) keyboardMode = 0;
    [bindingsBtn setEnabled:keyboardMode.load() != KB_OFF];
    [self saveBindings];
}

- (void)openBindingsEditor {
    _bindingsEditor = [[BindingsEditor alloc] initWithBindings:keyBindings parent:self];
    [_bindingsEditor showWindow:self];
}

- (void)applicationDidFinishLaunching:(NSNotification*)aNotification {
    // Elevate priority for lower input latency
    setpriority(PRIO_PROCESS, 0, -20);
    
    // Hidden fallback for old backends. UI stays unchanged; extended UDP is default.
    const char* legacyEnv = getenv("NSPC_LEGACY_UDP");
    g_legacyUdp = legacyEnv && legacyEnv[0] && strcmp(legacyEnv, "0") != 0;

    // Keep receiving gamepad input when the window loses focus
    GCController.shouldMonitorBackgroundEvents = YES;

    // Trigger the Input Monitoring permission prompt if needed
    if (!CGPreflightListenEventAccess()) CGRequestListenEventAccess();

    memset(hmacKey, 0, sizeof(hmacKey));
    keyboardMode = KB_OFF;

    for (int i = 0; i < 4; ++i) {
        controllers[i] = nil;
        g_rumbleControllers[i] = nil;
        g_hapticEngines[i] = nil;
        g_hapticPlayers[i] = nil;
        hwNames[i] = @"";
        slotActive[i] = false;
    }

    [self loadBindings];

    auto assign_controller = ^(GCController* ctrl) {
        if (!ctrl.extendedGamepad) return;
        // Prevent double-assignment (notification may fire for already-connected controllers)
        for (int i = 0; i < 4; ++i) {
            if (self->controllers[i] == ctrl) return;
        }
        for (int i = 0; i < 4; ++i) {
            if (self->controllers[i] == nil) {
                self->controllers[i] = ctrl;
                g_rumbleControllers[i] = ctrl;
                self->slotActive[i].store(true, std::memory_order_relaxed); // FIX 1
                self->hwNames[i] = ctrl.vendorName ?: @"Unknown Controller";
                attach_handlers(ctrl, ctrl.extendedGamepad, &self->states[i]);
                break;
            }
        }
    };

    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note) {
            GCController* ctrl = (GCController*)note.object;
            assign_controller(ctrl);
    }];

    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note) {
            GCController* ctrl = (GCController*)note.object;
            for (int i = 0; i < 4; ++i) {
                if (self->controllers[i] == ctrl) {
                    if (@available(macOS 10.15, *)) {
                        if (ctrl.motion) ctrl.motion.sensorsActive = NO;
                    }
                    stop_haptics_for_controller_on_main(i, true);
                    self->controllers[i] = nil;
                    g_rumbleControllers[i] = nil;
                    self->slotActive[i].store(false, std::memory_order_relaxed); // FIX 1
                    self->states[i].clear_inputs();
                    self->hwNames[i] = @"";
                    break;
                }
            }
    }];

    // Bind already connected controllers
    for (GCController* ctrl in [GCController controllers]) {
        assign_controller(ctrl);
    }

    // ── Build Window ──
    NSRect frame = NSMakeRect(0, 0, 420, 320);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    [window setTitle:@"NS PC Control (Mac)"];
    [window setDelegate:self];
    [window center];

    NSView* view = [window contentView];

    NSTextField* ipLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 280, 110, 20)];
    [ipLabel setStringValue:@"Raspberry Pi IP:"];
    [ipLabel setBezeled:NO]; [ipLabel setDrawsBackground:NO]; [ipLabel setEditable:NO]; [ipLabel setSelectable:NO];
    [view addSubview:ipLabel];

    ipField = [[NSTextField alloc] initWithFrame:NSMakeRect(130, 278, 270, 22)];
    NSString* saved = [[NSUserDefaults standardUserDefaults] stringForKey:@"lastIP"];
    [ipField setStringValue:saved ?: @"192.168.1.100"];
    [view addSubview:ipField];

    // Keyboard Mode row
    NSTextField* kbLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 252, 100, 18)];
    [kbLabel setStringValue:@"Keyboard Mode:"];
    [kbLabel setBezeled:NO]; [kbLabel setDrawsBackground:NO]; [kbLabel setEditable:NO]; [kbLabel setSelectable:NO];
    [kbLabel setAlignment:NSTextAlignmentRight];
    [view addSubview:kbLabel];

    kbCombo = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(125, 248, 175, 24)];
    [kbCombo addItemWithTitle:@"OFF"];
    [kbCombo addItemWithTitle:@"ON (single)"];
    [kbCombo addItemWithTitle:@"ON (override)"];
    [kbCombo selectItemAtIndex:keyboardMode.load()];
    [kbCombo setTarget:self];
    [kbCombo setAction:@selector(kbComboChanged)];
    [view addSubview:kbCombo];

    bindingsBtn = [[NSButton alloc] initWithFrame:NSMakeRect(310, 248, 80, 24)];
    [bindingsBtn setTitle:@"Bindings..."];
    [bindingsBtn setBezelStyle:NSBezelStyleRounded];
    [bindingsBtn setTarget:self];
    [bindingsBtn setAction:@selector(openBindingsEditor)];
    [bindingsBtn setEnabled:(keyboardMode.load() != KB_OFF)];
    [view addSubview:bindingsBtn];

    connectBtn = [[NSButton alloc] initWithFrame:NSMakeRect(125, 215, 120, 32)];
    [connectBtn setTitle:@"Connect"];
    [connectBtn setBezelStyle:NSBezelStyleRounded];
    [connectBtn setTarget:self];
    [connectBtn setAction:@selector(connectClicked)];
    [view addSubview:connectBtn];

    macrosBtn = [[NSButton alloc] initWithFrame:NSMakeRect(255, 215, 120, 32)];
    [macrosBtn setTitle:@"Macros..."];
    [macrosBtn setBezelStyle:NSBezelStyleRounded];
    [macrosBtn setTarget:self];
    [macrosBtn setAction:@selector(openMacros)];
    [view addSubview:macrosBtn];

    NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect(15, 195, 390, 1)];
    [sep setBoxType:NSBoxSeparator];
    [view addSubview:sep];

    statusField = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 165, 390, 17)];
    [statusField setStringValue:@"Ready"];
    [statusField setBezeled:NO]; [statusField setDrawsBackground:NO]; [statusField setEditable:NO]; [statusField setSelectable:NO];
    [statusField setTextColor:[NSColor grayColor]];
    [view addSubview:statusField];

    // P1 to P4 Labels
    for (int i = 0; i < 4; ++i) {
        ctrlLabels[i] = [[NSTextField alloc] initWithFrame:NSMakeRect(25, 140 - (i * 25), 380, 17)];
        [ctrlLabels[i] setStringValue:[NSString stringWithFormat:@"P%d: Waiting...", i+1]];
        [ctrlLabels[i] setBezeled:NO]; [ctrlLabels[i] setDrawsBackground:NO]; [ctrlLabels[i] setEditable:NO]; [ctrlLabels[i] setSelectable:NO];
        [view addSubview:ctrlLabels[i]];
    }

    pktLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 15, 390, 17)];
    [pktLabel setStringValue:@"Packets sent: 0"];
    [pktLabel setBezeled:NO]; [pktLabel setDrawsBackground:NO]; [pktLabel setEditable:NO]; [pktLabel setSelectable:NO];
    [view addSubview:pktLabel];

    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    [NSTimer scheduledTimerWithTimeInterval:0.1 target:self selector:@selector(updateUI) userInfo:nil repeats:YES];
}


- (void)openMacros {
    LoadMacroEntriesMac();
    if (!macroRowControls) macroRowControls = [[NSMutableArray alloc] init];
    macroListeningIndex = -1;

    if (!macroWindow) {
        macroWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 280)
                                                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        [macroWindow setTitle:@"Macros"];
        [macroWindow setDelegate:self];
        [macroWindow center];
        [macroWindow setReleasedWhenClosed:NO];
        [macroWindow setContentView:[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 620, 280)]];
    }

    AppDelegate* capturedSelf = self;
    if (!macroKeyMonitor) {
        macroKeyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent* (NSEvent* event) {
            AppDelegate* strongSelf = capturedSelf;
            if (!strongSelf || strongSelf->macroListeningIndex < 0 || [event window] != strongSelf->macroWindow) return event;
            int idx = (int)strongSelf->macroListeningIndex;
            std::string key = MacEventKeyName(event);
            bool save_needed = false;
            {
                std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
                if (idx >= 0 && idx < (int)g_macroEntriesMac.size()) {
                    if (key == "ESC") {
                        g_macroEntriesMac[idx].hotkey.clear();
                        RebuildMacroHotkeyStateMac();
                        save_needed = true;
                    } else if (!key.empty()) {
                        if (ValidateMacroHotkeyForEntryMac(key, idx, strongSelf->keyBindings)) {
                            g_macroEntriesMac[idx].hotkey = key;
                            RebuildMacroHotkeyStateMac();
                            save_needed = true;
                        }
                    }
                }
            }
            if (save_needed) SaveMacroEntriesMac();
            strongSelf->macroListeningIndex = -1;
            [strongSelf refreshMacroRows];
            return (NSEvent*)nil;
        }];
    }

    [self refreshMacroRows];
    [macroWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)refreshMacroRows {
    if (!macroWindow) return;
    for (NSView* v in macroRowControls) [v removeFromSuperview];
    [macroRowControls removeAllObjects];

    std::vector<MacroEntryMac> entries;
    {
        std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
        entries = g_macroEntriesMac;
    }

    constexpr CGFloat dialogW = 620;
    constexpr CGFloat x = 14;
    constexpr CGFloat rowH = 26;
    constexpr CGFloat nameW = 250;
    constexpr CGFloat keyW = 110;
    constexpr CGFloat btnW = 68;
    constexpr CGFloat exportW = 64;
    constexpr CGFloat delW = 64;
    constexpr CGFloat gap = 4;
    CGFloat keyX = x + nameW + gap;
    CGFloat renameX = keyX + keyW + gap;
    CGFloat exportX = renameX + btnW + gap;
    CGFloat deleteX = exportX + exportW + gap;
    CGFloat rightBtnX = deleteX + delW - 88;
    CGFloat rowsH = entries.empty() ? rowH : (CGFloat)entries.size() * rowH;
    CGFloat contentH = std::max<CGFloat>(190, rowsH + 104);
    [macroWindow setContentSize:NSMakeSize(dialogW, contentH)];
    NSView* view = [macroWindow contentView];

    CGFloat y = contentH - 38;
    if (entries.empty()) {
        NSTextField* empty = [[NSTextField alloc] initWithFrame:NSMakeRect(x, y + 2, nameW, 22)];
        [empty setStringValue:@"No macros"];
        [empty setAlignment:NSTextAlignmentCenter];
        [empty setBezeled:NO]; [empty setDrawsBackground:NO]; [empty setEditable:NO]; [empty setSelectable:NO];
        [view addSubview:empty]; [macroRowControls addObject:empty];
        y -= rowH;
    }

    for (int i = 0; i < (int)entries.size(); ++i) {
        const auto& e = entries[i];
        NSButton* run = [[NSButton alloc] initWithFrame:NSMakeRect(x, y, nameW, 24)];
        [run setTitle:[NSString stringWithUTF8String:(e.name.empty() ? "Macro" : e.name).c_str()]];
        [run setBezelStyle:NSBezelStyleRounded]; [run setTarget:self]; [run setAction:@selector(macroRun:)]; [run setTag:i];
        [view addSubview:run]; [macroRowControls addObject:run];

        NSButton* key = [[NSButton alloc] initWithFrame:NSMakeRect(keyX, y, keyW, 24)];
        std::string keyTitle = (macroListeningIndex == i) ? "..." : NormalizeMacroKeyMac(e.hotkey);
        [key setTitle:[NSString stringWithUTF8String:keyTitle.c_str()]];
        [key setBezelStyle:NSBezelStyleTexturedRounded]; [key setTarget:self]; [key setAction:@selector(macroKey:)]; [key setTag:i];
        [view addSubview:key]; [macroRowControls addObject:key];

        NSButton* rename = [[NSButton alloc] initWithFrame:NSMakeRect(renameX, y, btnW, 24)];
        [rename setTitle:@"Rename"]; [rename setBezelStyle:NSBezelStyleRounded]; [rename setTarget:self]; [rename setAction:@selector(macroRename:)]; [rename setTag:i];
        [view addSubview:rename]; [macroRowControls addObject:rename];

        NSButton* exp = [[NSButton alloc] initWithFrame:NSMakeRect(exportX, y, exportW, 24)];
        [exp setTitle:@"Export"]; [exp setBezelStyle:NSBezelStyleRounded]; [exp setTarget:self]; [exp setAction:@selector(macroExportOne:)]; [exp setTag:i];
        [view addSubview:exp]; [macroRowControls addObject:exp];

        NSButton* del = [[NSButton alloc] initWithFrame:NSMakeRect(deleteX, y, delW, 24)];
        [del setTitle:@"Delete"]; [del setBezelStyle:NSBezelStyleRounded]; [del setTarget:self]; [del setAction:@selector(macroDelete:)]; [del setTag:i];
        [view addSubview:del]; [macroRowControls addObject:del];
        y -= rowH;
    }

    NSButton* importBtn = [[NSButton alloc] initWithFrame:NSMakeRect(x, 50, 88, 30)];
    [importBtn setTitle:@"Import"]; [importBtn setBezelStyle:NSBezelStyleRounded]; [importBtn setTarget:self]; [importBtn setAction:@selector(macroImport:)];
    [view addSubview:importBtn]; [macroRowControls addObject:importBtn];

    macroRecordBtn = [[NSButton alloc] initWithFrame:NSMakeRect(rightBtnX, 50, 88, 30)];
    [macroRecordBtn setTitle:IsMacroRecordingMac() ? @"Stop" : @"Record P1"]; [macroRecordBtn setBezelStyle:NSBezelStyleRounded]; [macroRecordBtn setTarget:self]; [macroRecordBtn setAction:@selector(macroRecordToggle:)];
    [view addSubview:macroRecordBtn]; [macroRowControls addObject:macroRecordBtn];

    NSButton* exportBtn = [[NSButton alloc] initWithFrame:NSMakeRect(x, 12, 88, 30)];
    [exportBtn setTitle:@"Export"]; [exportBtn setBezelStyle:NSBezelStyleRounded]; [exportBtn setTarget:self]; [exportBtn setAction:@selector(macroExportAll:)];
    [view addSubview:exportBtn]; [macroRowControls addObject:exportBtn];

    NSButton* closeBtn = [[NSButton alloc] initWithFrame:NSMakeRect(rightBtnX, 12, 88, 30)];
    [closeBtn setTitle:@"Close"]; [closeBtn setBezelStyle:NSBezelStyleRounded]; [closeBtn setTarget:self]; [closeBtn setAction:@selector(macroClose:)];
    [view addSubview:closeBtn]; [macroRowControls addObject:closeBtn];
}

- (void)macroRun:(id)sender {
    int idx = (int)[(NSControl*)sender tag];
    std::string json;
    {
        std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
        if (idx >= 0 && idx < (int)g_macroEntriesMac.size()) json = g_macroEntriesMac[idx].json;
    }
    if (!json.empty()) StartMacroTextMac(json);
}

- (void)macroKey:(id)sender {
    macroListeningIndex = [(NSControl*)sender tag];
    [self refreshMacroRows];
    [macroWindow makeFirstResponder:macroWindow.contentView];
}

- (void)macroRename:(id)sender {
    int idx = (int)[(NSControl*)sender tag];
    std::string oldName;
    {
        std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
        if (idx < 0 || idx >= (int)g_macroEntriesMac.size()) return;
        oldName = g_macroEntriesMac[idx].name.empty() ? "Macro" : g_macroEntriesMac[idx].name;
    }

    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Rename Macro"];
    NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    [field setStringValue:[NSString stringWithUTF8String:oldName.c_str()]];
    [alert setAccessoryView:field];
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    if ([alert runModal] != NSAlertFirstButtonReturn) return;

    std::string newName = macro_trim(std::string([[field stringValue] UTF8String]));
    if (newName.empty()) {
        ShowMacroErrorMac("Macro name cannot be empty.");
        return;
    }

    bool save_needed = false;
    {
        std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
        int duplicate = FindMacroEntryByNameMac(newName);
        if (duplicate >= 0 && duplicate != idx) {
            ShowMacroErrorMac("Another macro already uses that name.");
            return;
        }
        if (idx >= 0 && idx < (int)g_macroEntriesMac.size()) {
            g_macroEntriesMac[idx].name = newName;
            g_macroEntriesMac[idx].json = MacroPrettyJsonWithForcedNameMac(g_macroEntriesMac[idx].json, newName);
            save_needed = true;
        }
    }
    if (save_needed) SaveMacroEntriesMac();
    [self refreshMacroRows];
}

- (void)macroExportOne:(id)sender {
    int idx = (int)[(NSControl*)sender tag];
    std::vector<MacroEntryMac> one;
    std::string name = "Macro";
    {
        std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
        if (idx < 0 || idx >= (int)g_macroEntriesMac.size()) return;
        one.push_back(g_macroEntriesMac[idx]);
        name = g_macroEntriesMac[idx].name.empty() ? "Macro" : g_macroEntriesMac[idx].name;
    }
    NSSavePanel* panel = [NSSavePanel savePanel];
    [panel setNameFieldStringValue:MacroSafeFileNameMac(name)];
    if ([panel runModal] == NSModalResponseOK) {
        MacroWriteURLMac([panel URL], MacroEntriesToJsonMac(one));
    }
}

- (void)macroDelete:(id)sender {
    int idx = (int)[(NSControl*)sender tag];
    bool save_needed = false;
    {
        std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
        if (idx >= 0 && idx < (int)g_macroEntriesMac.size()) {
            g_macroEntriesMac.erase(g_macroEntriesMac.begin() + idx);
            RebuildMacroHotkeyStateMac();
            macroListeningIndex = -1;
            save_needed = true;
        }
    }
    if (save_needed) SaveMacroEntriesMac();
    [self refreshMacroRows];
}

- (void)macroImport:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    if (@available(macOS 11.0, *)) {
        Class UTTypeClass = NSClassFromString(@"UTType");
        id jsonType = UTTypeClass ? [UTTypeClass valueForKey:@"JSON"] : nil;
        if (jsonType) [panel setValue:@[jsonType] forKey:@"allowedContentTypes"];
    }
    if ([panel runModal] != NSModalResponseOK) return;

    std::string raw, err;
    if (!ReadMacroURLLimitedMac([panel URL], raw, err)) {
        ShowMacroErrorMac(err.empty() ? "Invalid or empty macro file." : err);
        return;
    }
    std::vector<MacroEntryMac> imported;
    if (!ParseMacroEntriesTextMac(raw, imported, err) || imported.empty()) {
        ShowMacroErrorMac("Invalid macro JSON: " + err);
        return;
    }

    if (imported.size() > 1 || raw.find("\"macros\"") != std::string::npos) {
        {
            std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
            g_macroEntriesMac = std::move(imported);
            RebuildMacroHotkeyStateMac();
        }
        SaveMacroEntriesMac();
    } else {
        UpsertMacroEntryMac(imported[0], false, keyBindings);
    }
    [self refreshMacroRows];
}

- (void)macroRecordTick:(id)sender {
    ns::HIDReport report;
    report.reset();
    if (slotActive[0].load(std::memory_order_relaxed)) {
        report = map_gc_to_console(states[0]);
    }
    int km = keyboardMode.load();
    if (km == KB_SINGLE) {
        report.reset();
        apply_keyboard_to_report_mac(report, keyBindings, false);
    } else if (km == KB_OVERRIDE) {
        apply_keyboard_to_report_mac(report, keyBindings, true);
    }
    SampleMacroRecordingMac(report);
}

- (void)macroRecordToggle:(id)sender {
    if (!IsMacroRecordingMac()) {
        StartMacroRecordingMac();
        [self macroRecordTick:nil];
        macroRecordTimer = [NSTimer scheduledTimerWithTimeInterval:0.016 target:self selector:@selector(macroRecordTick:) userInfo:nil repeats:YES];
        [macroRecordBtn setTitle:@"Stop"];
    } else {
        [self macroRecordTick:nil];
        if (macroRecordTimer) {
            [macroRecordTimer invalidate];
            macroRecordTimer = nil;
        }
        std::string recorded = StopMacroRecordingMac();
        if (!recorded.empty()) {
            MacroEntryMac e;
            e.name = "Recorded Macro";
            e.hotkey = "";
            e.json = recorded;
            UpsertMacroEntryMac(e, true, keyBindings);
        }
        [self refreshMacroRows];
    }
}

- (void)macroExportAll:(id)sender {
    SaveMacroEntriesMac();
    std::vector<MacroEntryMac> entries;
    {
        std::lock_guard<std::mutex> lk(g_macroEntriesMutex);
        entries = g_macroEntriesMac;
    }
    NSSavePanel* panel = [NSSavePanel savePanel];
    [panel setNameFieldStringValue:@"ns-macros.json"];
    if ([panel runModal] == NSModalResponseOK) {
        MacroWriteURLMac([panel URL], MacroEntriesToJsonMac(entries));
    }
}

- (void)macroClose:(id)sender {
    if (IsMacroRecordingMac()) {
        if (macroRecordTimer) {
            [macroRecordTimer invalidate];
            macroRecordTimer = nil;
        }
        StopMacroRecordingMac();
    }
    macroListeningIndex = -1;
    [macroWindow close];
}


- (void)connectClicked {
    if (connected) [self disconnect];
    else [self connect];
}

- (void)connect {
    NSString* ipStr = [ipField stringValue];
    if ([ipStr length] == 0) return;

    // Parse ip:port safely
    NSArray<NSString*> *parts = [ipStr componentsSeparatedByString:@":"];
    NSString *host = parts.firstObject;
    int port = ns::DEFAULT_PORT;
    if (parts.count > 1) {
        port = [parts.lastObject intValue];
        if (port <= 0 || port > 65535) port = ns::DEFAULT_PORT;
    }
    std::string stdIp([host UTF8String]);

    [[NSUserDefaults standardUserDefaults] setObject:ipStr forKey:@"lastIP"];
    derive_key(ns::DEFAULT_SECRET, hmacKey);

    packetCount = 0;
    connected = true;
    senderRunning = true;

    senderThread = std::thread([self, stdIp, port] {
        self->sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (self->sock < 0) return;

        // Bind to a fixed local port so the backend identifies reconnects as the same PC.
        int opt = 1;
        setsockopt(self->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(self->sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        struct sockaddr_in local_bind{};
        local_bind.sin_family = AF_INET;
        local_bind.sin_addr.s_addr = INADDR_ANY;
        local_bind.sin_port = htons(42069);
        ::bind(self->sock, (struct sockaddr*)&local_bind, sizeof(local_bind));

        int flags = fcntl(self->sock, F_GETFL, 0);
        if (flags >= 0) fcntl(self->sock, F_SETFL, flags | O_NONBLOCK);

        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char portStr[8]; snprintf(portStr, sizeof(portStr), "%u", port);
        
        if (getaddrinfo(stdIp.c_str(), portStr, &hints, &res) != 0 || !res) {
            close(self->sock); self->sock = -1; return;
        }
        
        sockaddr_in dest{};
        memcpy(&dest, res->ai_addr, sizeof(dest));
        freeaddrinfo(res);

        bool serverIsHori = false;
        const int activeSendIntervalMs = detect_server_udp_interval_ms(
            self->sock, dest, ns::HORI_UDP_INTERVAL_MS, &serverIsHori);
        const bool sendMotion = !serverIsHori;

        uint32_t seqCounter = 0;
        bool first_packet = true;
        RumbleManager rumble;

        while (self->senderRunning.load(std::memory_order_relaxed)) {
            {
                std::string upload;
                { std::lock_guard<std::mutex> lk(g_macroUploadMutex); upload.swap(g_macroUploadPending); }
                if (!upload.empty()) send_macro_udp_packet(self->sock, dest, self->hmacKey, upload, 0);
            }

            PollMacroHotkeyMac(self->keyBindings);

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
            for (int i = 0; i < 4; ++i) {
                if (!self->slotActive[i].load(std::memory_order_relaxed)) continue;
                logical_reports[i] = map_gc_to_console(self->states[i]);
                present[i] = true;
                controller_for_slot[i] = i;
                if (sendMotion && self->states[i].has_motion.load(std::memory_order_relaxed)) {
                    logical_motion[i] = map_gc_motion_to_console(self->states[i]);
                    has_motion[i] = true;
                }
                active_count++;
            }

            // Keyboard overrides Player 1
            int km = self->keyboardMode.load();
            if (km == KB_SINGLE) {
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
                apply_keyboard_to_report_mac(logical_reports[0], self->keyBindings, false);
                present[0] = true;
                has_motion[0] = false;
                controller_for_slot[0] = -1;
                active_count = std::max(active_count, 1);
            } else if (km == KB_OVERRIDE) {
                apply_keyboard_to_report_mac(logical_reports[0], self->keyBindings, true);
                present[0] = true;
                active_count = std::max(active_count, 1);
            }

            SampleMacroRecordingMac(logical_reports[0]);
            if (ApplyMacroOverrideMac(logical_reports, present, has_motion, controller_for_slot)) active_count = 1;

            if (g_legacyUdp) {
                ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet));
                pkt.magic         = ns::PROTO_MAGIC;
                pkt.version       = ns::PROTO_VERSION;
                pkt.flags         = first_packet ? ns::FLAG_RESET : ns::FLAG_NONE;
                first_packet      = false;
                pkt.seq           = seqCounter++;
                pkt.ts_us         = ns::now_us();
                pkt.report.reset();

                ns::HIDReport* pads[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
                for (int i = 0; i < 4; ++i) *pads[i] = logical_reports[i];

                uint8_t full_hmac[32];
                hmac_sha256(self->hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
                sendto(self->sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
            } else {
                ExtendedUdpPacket pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.magic        = ns::PROTO_MAGIC;
                pkt.version      = ns::WEB_PROTO_VERSION;
                pkt.flags        = first_packet ? ns::FLAG_RESET : ns::FLAG_NONE;
                first_packet     = false;
                pkt.seq          = seqCounter++;
                pkt.timestamp_us = ns::now_us();
                pkt.report.reset();

                ns::ExtendedHIDReport* pads[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
                for (int i = 0; i < 4; ++i)
                    fill_extended_pad(*pads[i], logical_reports[i], present[i], has_motion[i] ? &logical_motion[i] : nullptr);

                uint8_t full_hmac[32];
                hmac_sha256(self->hmacKey, 32, (const uint8_t*)&pkt, EXT_UDP_PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
                sendto(self->sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&dest, sizeof(dest));

                pump_udp_rumble(self->sock, rumble, controller_for_slot);
                rumble.update_timeouts(controller_for_slot);
            }

            self->packetCount++;

            auto interval = (active_count > 0)
                ? std::chrono::milliseconds(activeSendIntervalMs)
                : std::chrono::milliseconds(50); // keep connection alive below watchdog timeout
            std::this_thread::sleep_for(interval);
        }

        send_udp_disconnect_packet(self->sock, dest, self->hmacKey, seqCounter++, g_legacyUdp);
        rumble.stop_all();
        close(self->sock);
        self->sock = -1;
    });

    [connectBtn setTitle:@"Disconnect"];
    [ipField setEnabled:NO];
    [kbCombo setEnabled:NO];
    [bindingsBtn setEnabled:NO];
    [statusField setStringValue:[NSString stringWithFormat:@"Connected to %@:%d", host, port]];
    [statusField setTextColor:[NSColor systemGreenColor]];
}

- (void)disconnect {
    connected = false;
    senderRunning = false;
    if (senderThread.joinable()) senderThread.join();

    for (int i = 0; i < 4; ++i)
        stop_haptics_for_controller_on_main(i, false);

    [connectBtn setTitle:@"Connect"];
    [ipField setEnabled:YES];
    [kbCombo setEnabled:YES];
    [bindingsBtn setEnabled:keyboardMode.load() != KB_OFF];
    [statusField setStringValue:@"Disconnected"];
    [statusField setTextColor:[NSColor grayColor]];
}

- (void)updateUI {
    if (connected) {
        [pktLabel setStringValue:[NSString stringWithFormat:@"Packets sent: %u", packetCount.load()]];
    }
    
    int km = keyboardMode.load();
    
    // Figure out where P1's physical controller is being shifted
    int shifted_p1_target = -1;
    if (km == KB_SINGLE && slotActive[0].load(std::memory_order_relaxed)) {
        if (!slotActive[1].load(std::memory_order_relaxed)) shifted_p1_target = 1;
        else if (!slotActive[2].load(std::memory_order_relaxed)) shifted_p1_target = 2;
        else if (!slotActive[3].load(std::memory_order_relaxed)) shifted_p1_target = 3;
    }

    for (int i = 0; i < 4; ++i) {
        NSString* text;
        NSColor* color;
        
        if (i == 0 && km != KB_OFF) {
            text = (km == KB_SINGLE) ? @"P1: Keyboard (Single)" : @"P1: Keyboard (Override)";
            color = [NSColor textColor];
        } 
        else if (i == shifted_p1_target) {
            // Visually insert the shifted controller
            text = [NSString stringWithFormat:@"P%d: %@ (Shifted)", i+1, hwNames[0]];
            color = [NSColor textColor];
        }
        else if (slotActive[i].load(std::memory_order_relaxed)) {
            text = [NSString stringWithFormat:@"P%d: %@", i+1, hwNames[i]];
            color = [NSColor textColor];
        } 
        else {
            text = [NSString stringWithFormat:@"P%d: Waiting...", i+1];
            color = [NSColor disabledControlTextColor];
        }
        
        [ctrlLabels[i] setStringValue:text];
        [ctrlLabels[i] setTextColor:color];
    }
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    if (sender == macroWindow) {
        if (IsMacroRecordingMac()) {
            if (macroRecordTimer) {
                [macroRecordTimer invalidate];
                macroRecordTimer = nil;
            }
            StopMacroRecordingMac();
        }
        macroListeningIndex = -1;
        return YES;
    }
    if (macroKeyMonitor) {
        [NSEvent removeMonitor:macroKeyMonitor];
        macroKeyMonitor = nil;
    }
    [self disconnect];
    [NSApp terminate:self];
    return YES;
}

@end

// ── Entry point ──
int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        AppDelegate* delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}
