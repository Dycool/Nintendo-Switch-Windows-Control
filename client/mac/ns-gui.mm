/// ns-gui.mm  —  macOS Cocoa GUI frontend for the Switch wireless gamepad bridge
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

// Import external protocol structures (Version 4 with MultiReport)
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

static ns::MotionReport map_gc_motion_to_switch(const GamepadState& st) {
    ns::MotionReport m;
    m.reset();

    // Match the backend's virtual IMU scale roughly: accel around 0x1000 per g,
    // gyro kept conservative so real pads do not saturate the Switch sample.
    m.ax = clamp_i16_from_float(st.ax.load(std::memory_order_relaxed) * 4096.0f);
    m.ay = clamp_i16_from_float(st.ay.load(std::memory_order_relaxed) * 4096.0f);
    m.az = clamp_i16_from_float(st.az.load(std::memory_order_relaxed) * 4096.0f);
    m.gx = clamp_i16_from_float(st.gx.load(std::memory_order_relaxed) * 1000.0f);
    m.gy = clamp_i16_from_float(st.gy.load(std::memory_order_relaxed) * 1000.0f);
    m.gz = clamp_i16_from_float(st.gz.load(std::memory_order_relaxed) * 1000.0f);
    return m;
}

static void set_pad_present_flag(ns::ExtendedHIDReport& r, bool present) {
    // Backend/web protocol uses byte 7 of ExtendedHIDReport as the pad-present flag.
    // This lets neutral-but-connected UDP pads claim a Switch slot and receive rumble.
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
//  App Delegate (GUI and Core Logic)
// ─────────────────────────────────────────────────────────────────────────────

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
}
- (void)connect;
- (void)disconnect;
- (void)updateUI;
- (void)loadBindings;
- (void)saveBindings;
- (void)kbComboChanged;
- (void)openBindingsEditor;
- (void)openMacros;
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
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Macros"];
    [alert setInformativeText:@"JSON/commands, or record live P1 buttons while connected."];
    [alert addButtonWithTitle:@"Run"];
    [alert addButtonWithTitle:@"Save/Add"];
    [alert addButtonWithTitle:@"Delete"];
    [alert addButtonWithTitle:@"Record"];
    [alert addButtonWithTitle:@"Stop Recording"];
    [alert addButtonWithTitle:@"Import JSON"];
    [alert addButtonWithTitle:@"Export JSON"];
    [alert addButtonWithTitle:@"Close"];
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,520,180)];
    NSTextView* tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0,0,520,180)];
    [scroll setDocumentView:tv]; [scroll setHasVerticalScroller:YES];
    std::string txt = LoadMacroTextMac();
    if (txt.empty()) txt = "{\"name\":\"Macro\",\"commands\":\"WAIT 200; A 100; B 100\"}";
    [tv setString:[NSString stringWithUTF8String:txt.c_str()]];
    [alert setAccessoryView:scroll];
    NSModalResponse r = [alert runModal];
    std::string out([[tv string] UTF8String]);
    if (r == NSAlertFirstButtonReturn) StartMacroTextMac(out);
    else if (r == NSAlertSecondButtonReturn) SaveMacroTextMac(out);
    else if (r == NSAlertThirdButtonReturn) SaveMacroTextMac("");
    else if (r == NSAlertThirdButtonReturn + 1) StartMacroRecordingMac();
    else if (r == NSAlertThirdButtonReturn + 2) {
        std::string rec = StopMacroRecordingMac();
        SaveMacroTextMac(rec);
    } else if (r == NSAlertThirdButtonReturn + 3) {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setAllowedFileTypes:@[@"json"]];
        if ([panel runModal] == NSModalResponseOK) {
            NSString* imported = [NSString stringWithContentsOfURL:[panel URL] encoding:NSUTF8StringEncoding error:nil];
            if (imported) SaveMacroTextMac(std::string([imported UTF8String]));
        }
    } else if (r == NSAlertThirdButtonReturn + 4) {
        NSSavePanel* panel = [NSSavePanel savePanel];
        [panel setNameFieldStringValue:@"ns-macros.json"];
        if ([panel runModal] == NSModalResponseOK) {
            std::string prettyOut = macro_pretty_json(out);
            NSString* outText = [NSString stringWithUTF8String:prettyOut.c_str()];
            [outText writeToURL:[panel URL] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        }
    }
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

        uint32_t seqCounter = 0;
        bool first_packet = true;
        RumbleManager rumble;

        while (self->senderRunning.load(std::memory_order_relaxed)) {
            {
                std::string upload;
                { std::lock_guard<std::mutex> lk(g_macroUploadMutex); upload.swap(g_macroUploadPending); }
                if (!upload.empty()) send_macro_udp_packet(self->sock, dest, self->hmacKey, upload, 0);
            }
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
            for (int i = 0; i < 4; ++i) {
                if (!self->slotActive[i].load(std::memory_order_relaxed)) continue;
                logical_reports[i] = map_gc_to_switch(self->states[i]);
                present[i] = true;
                controller_for_slot[i] = i;
                if (self->states[i].has_motion.load(std::memory_order_relaxed)) {
                    logical_motion[i] = map_gc_motion_to_switch(self->states[i]);
                    has_motion[i] = true;
                }
                active_count++;
                if (i == 0) c1 = true;
                else if (i == 1) c2 = true;
                else if (i == 2) c3 = true;
                else if (i == 3) c4 = true;
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
                pkt.version      = ns::PROTO_VERSION;
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
                ? std::chrono::milliseconds(4)
                : std::chrono::milliseconds(50); // keep connection alive below watchdog timeout
            std::this_thread::sleep_for(interval);
        }

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