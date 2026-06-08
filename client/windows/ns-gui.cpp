#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#define _WIN32_WINNT 0x0601
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif

#include <windows.h>
#include <mmsystem.h>
#include <windowsx.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dbt.h>

#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <memory>
#include <sstream>
#include <cctype>
#include <fstream>
#include <unordered_map>
#include <array>
#include <mutex>
#include <cmath>
#include <commdlg.h>

// SDL3 is the only native Windows gamepad backend. It owns controller
// discovery, buttons/sticks, optional motion sensors, and rumble.
// Link against SDL3 and ship SDL3.dll next to ns-gui.exe, or link SDL3 statically.
#ifndef SDL_MAIN_HANDLED
#  define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winmm.lib")
#ifndef NS_LINK_SDL3_MANUALLY
#  ifdef NS_SDL3_STATIC
#    pragma comment(lib, "SDL3-static.lib")
#  else
#    pragma comment(lib, "SDL3.lib")
#  endif
#endif
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ── Protocol ──
#include "../../server/rpi/include/sha256.h"
#include "../../server/rpi/include/protocol.hpp"
#include <stdexcept>
#include <limits>

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
static_assert(sizeof(ExtendedUdpPacket) == EXT_UDP_PACKET_SIZE, "ExtendedUdpPacket wire layout changed");



static void set_pad_present_flag(ns::ExtendedHIDReport& r, bool present) {
    // This project keeps the pad-present bit in HIDReport::vendor (byte 7)
    // so old HORI-compatible HIDReport stays exactly 8 bytes.
    if (present) r.input.vendor |= EXT_PAD_PRESENT;
    else         r.input.vendor &= (uint8_t)~EXT_PAD_PRESENT;
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
        dst.has_motion = 1;
    }
}


// SDL/Windows can occasionally report a held digital button as released for a
// single frame, especially while HIDAPI is also handling rumble/sensor traffic.
// A tiny release grace avoids visible "tap cuts" such as Mario Kart drifting
// dropping R while the physical shoulder is still held. Real releases are only
// delayed by this many microseconds.
static constexpr uint64_t SDL_DIGITAL_RELEASE_GRACE_US = 35000ULL;

struct DigitalReleaseFilter {
    uint64_t button_until[14]{};
    uint8_t last_hat = ns::HAT_NEUTRAL;
    uint64_t hat_until = 0;

    void reset() {
        memset(button_until, 0, sizeof(button_until));
        last_hat = ns::HAT_NEUTRAL;
        hat_until = 0;
    }

    void apply(ns::HIDReport& r, uint64_t now) {
        static const uint16_t bits[14] = {
            ns::BTN_Y, ns::BTN_B, ns::BTN_A, ns::BTN_X,
            ns::BTN_L, ns::BTN_R, ns::BTN_ZL, ns::BTN_ZR,
            ns::BTN_MINUS, ns::BTN_PLUS, ns::BTN_LSTICK, ns::BTN_RSTICK,
            ns::BTN_HOME, ns::BTN_CAPTURE
        };

        for (int i = 0; i < 14; ++i) {
            const uint16_t bit = bits[i];
            if (r.buttons & bit) {
                button_until[i] = now + SDL_DIGITAL_RELEASE_GRACE_US;
            } else if (button_until[i] != 0 && now <= button_until[i]) {
                r.buttons |= bit;
            } else {
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



// ── SDL3 unified gamepad backend ────────────────────────────────────────────
// SDL gives us one Windows receiver path for gamepads, rumble, and optional motion sensors.
// The rest of the app still deals only in our console-style HIDReport +
// MotionReport protocol.
struct SdlPadState {
    bool connected = false;
    ns::HIDReport input{};
    ns::MotionReport motion{};
    bool has_motion = false;
    uint64_t last_input_us = 0;
    std::string name;
    uint16_t vid = 0;
    uint16_t pid = 0;
    SDL_JoystickID instance_id = 0;
};

static uint8_t sdl_axis_to_byte(Sint16 val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((int)(val - deadzone) * 127) / (32767 - deadzone);
    else                 scaled = 128 - ((int)(-val - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

static int16_t clamp_motion_i16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)std::lround(v);
}

class SDLInputManager {
public:
    bool start() {
        std::lock_guard<std::mutex> lk(mtx);
        if (initialized) return true;

        // These must be set before initializing the gamepad subsystem.
        // Enhanced reports are what let SDL expose gyro on console controllers
        // and rumble/effects on several Bluetooth pads.
        SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_" "SW" "ITCH", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_" "JOY" "_CONS", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4", "0"); // avoid Sony enhanced-report reconnect loop by default
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "0"); // avoid Sony enhanced-report reconnect loop by default
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_XBOX", "1");
        SDL_SetHint("SDL_JOYSTICK_ENHANCED_REPORTS", "1");

        if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_SENSOR | SDL_INIT_HAPTIC | SDL_INIT_EVENTS)) {
            last_error = SDL_GetError() ? SDL_GetError() : "SDL_Init failed";
            clear_states_locked();
            return false;
        }

        initialized = true;
        last_error.clear();
        last_scan_us = 0;
        scan_locked(true);
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lk(mtx);
        stop_all_rumble_locked();
        close_all_locked();
        clear_states_locked();
        if (initialized) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_SENSOR | SDL_INIT_HAPTIC | SDL_INIT_EVENTS);
            initialized = false;
        }
    }

    void poll() {
        std::lock_guard<std::mutex> lk(mtx);
        if (!initialized) return;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                force_scan = true;
            }
        }

        SDL_UpdateGamepads();
        SDL_UpdateSensors();

        uint64_t now = ns::now_us();
        if (force_scan || last_scan_us == 0 || now - last_scan_us > 500000ULL) {
            scan_locked(false);
        }

        refresh_states_locked(now);
    }

    std::array<SdlPadState, 4> snapshot() {
        std::lock_guard<std::mutex> lk(mtx);
        return states;
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

        // Main left/right rumble. This is the normal path for Xbox, console Pro,
        // DualShock/DualSense, etc.  SDL returns false when the active backend or
        // controller does not expose a usable rumble path, so do not assume that
        // a connected pad can rumble.
        bool ok_main = SDL_RumbleGamepad(d->pad, stop ? 0 : low_word, stop ? 0 : high_word, duration_ms);

        // Xbox One/Series pads may also expose trigger rumble. It is not the same
        // thing as normal body rumble, but it gives useful feedback if the normal
        // path is unavailable or weak. Stopping both is harmless.
        bool ok_trigger = true;
        if (d->trigger_rumble_capable || !ok_main || stop) {
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

    std::string error() const {
        std::lock_guard<std::mutex> lk(mtx);
        return last_error;
    }

private:
    struct Device {
        SDL_Gamepad* pad = nullptr;
        SDL_JoystickID id = 0;
        int slot = -1;
        bool accel_enabled = false;
        bool gyro_enabled = false;
        bool rumble_capable = false;
        bool trigger_rumble_capable = false;
        bool gyro_filter_initialized = false;
        float gyro_bias[3] = {0.0f, 0.0f, 0.0f};

        std::string name;
        uint16_t vid = 0;
        uint16_t pid = 0;
    };

    mutable std::mutex mtx;
    bool initialized = false;
    bool force_scan = false;
    uint64_t last_scan_us = 0;
    std::string last_error;
    std::array<SdlPadState, 4> states{};
    std::vector<Device> devices;

    static WORD motor_word(uint8_t v) {
        return (WORD)((uint32_t)v * 65535u / 255u);
    }

    static bool button(SDL_Gamepad* pad, SDL_GamepadButton b) {
        return SDL_GetGamepadButton(pad, b);
    }

    static std::string upper_copy(std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }

    static bool contains_upper(const std::string& haystack, const char* needle) {
        return upper_copy(haystack).find(needle) != std::string::npos;
    }

    static bool is_sony_controller(const Device& d) {
        return d.vid == 0x054C; // Sony Interactive Entertainment
    }

    static bool has_native_home_capture(const Device& d) {
        if (d.vid == 0x057E) return true;
        const std::string n = upper_copy(d.name);
        return n.find("VENDOR") != std::string::npos ||
               n.find("CONSOLE") != std::string::npos ||
               n.find("USB GAMEPAD") != std::string::npos;
    }

    static bool should_use_combo_shortcuts(const Device& d) {
        // Some pads normally expose real HOME/CAPTURE through SDL, so do not
        // steal L3+R3 or Back+Start from them. Xbox pads often have GUIDE
        // reserved by Windows/Game Bar and older pads have no Capture button,
        // so keep the convenient aliases there.
        if (has_native_home_capture(d)) return false;
        if (d.vid == 0x045E) return true; // Microsoft/Xbox
        return contains_upper(d.name, "XBOX") || contains_upper(d.name, "XINPUT") ||
               contains_upper(d.name, "MICROSOFT");
    }

    static ns::HIDReport map_gamepad(const Device& d) {
        SDL_Gamepad* pad = d.pad;
        ns::HIDReport r;
        r.reset();

        // SDL's face buttons use physical positions: south/east/west/north.
        // Map them to Vendor labels so an Xbox A/South acts as console B,
        // Xbox B/East acts as console A, etc.
        if (button(pad, SDL_GAMEPAD_BUTTON_SOUTH)) r.buttons |= ns::BTN_B;
        if (button(pad, SDL_GAMEPAD_BUTTON_EAST))  r.buttons |= ns::BTN_A;
        if (button(pad, SDL_GAMEPAD_BUTTON_WEST))  r.buttons |= ns::BTN_Y;
        if (button(pad, SDL_GAMEPAD_BUTTON_NORTH)) r.buttons |= ns::BTN_X;

        if (button(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  r.buttons |= ns::BTN_L;
        if (button(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) r.buttons |= ns::BTN_R;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  > 16384) r.buttons |= ns::BTN_ZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) r.buttons |= ns::BTN_ZR;

        if (button(pad, SDL_GAMEPAD_BUTTON_BACK))       r.buttons |= ns::BTN_MINUS;
        if (button(pad, SDL_GAMEPAD_BUTTON_START))      r.buttons |= ns::BTN_PLUS;
        if (button(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK))  r.buttons |= ns::BTN_LSTICK;
        if (button(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) r.buttons |= ns::BTN_RSTICK;
        if (button(pad, SDL_GAMEPAD_BUTTON_GUIDE))      r.buttons |= ns::BTN_HOME;
        if (button(pad, SDL_GAMEPAD_BUTTON_MISC1))      r.buttons |= ns::BTN_CAPTURE;

        if (should_use_combo_shortcuts(d)) {
            if ((r.buttons & ns::BTN_LSTICK) && (r.buttons & ns::BTN_RSTICK)) {
                r.buttons |= ns::BTN_HOME;
                r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
            }
            if ((r.buttons & ns::BTN_MINUS) && (r.buttons & ns::BTN_PLUS)) {
                r.buttons |= ns::BTN_CAPTURE;
                r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);
            }
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

    static float motion_deadzone_float(float v, float dz) {
        if (std::fabs(v) <= dz) return 0.0f;
        return v > 0.0f ? (v - dz) : (v + dz);
    }

    static void apply_motion(Device& d, ns::MotionReport& out, bool& has_motion) {
        SDL_Gamepad* pad = d.pad;
        out.reset();
        has_motion = false;

        float accel[3] = {};
        bool got_accel = false;
        if (d.accel_enabled && SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3)) {
            out.ax = clamp_motion_i16((accel[0] / 9.80665f) * 4096.0f);
            out.ay = clamp_motion_i16((-accel[2] / 9.80665f) * 4096.0f);
            out.az = clamp_motion_i16((accel[1] / 9.80665f) * 4096.0f);
            has_motion = true;
            got_accel = true;
        }

        float gyro[3] = {};
        if (d.gyro_enabled && SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gyro, 3)) {
            constexpr float RAD_TO_DEG = 57.29577951308232f;
            constexpr float CONSOLE_GYRO_SCALE = RAD_TO_DEG * 16.384f;

            float g[3] = {
                gyro[0] * CONSOLE_GYRO_SCALE,
                gyro[1] * CONSOLE_GYRO_SCALE,
                -gyro[2] * CONSOLE_GYRO_SCALE
            };

            const float accel_mag = got_accel
                ? std::sqrt(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2])
                : 9.80665f;
            const float gyro_mag_rad =
                std::sqrt(gyro[0] * gyro[0] + gyro[1] * gyro[1] + gyro[2] * gyro[2]);

            const bool accel_is_gravity = got_accel && std::fabs(accel_mag - 9.80665f) < 1.75f;
            const bool gyro_is_quiet = gyro_mag_rad < 0.08f;
            const bool resting = accel_is_gravity && gyro_is_quiet;

            if (!d.gyro_filter_initialized) {
                d.gyro_bias[0] = resting ? g[0] : 0.0f;
                d.gyro_bias[1] = resting ? g[1] : 0.0f;
                d.gyro_bias[2] = resting ? g[2] : 0.0f;
                d.gyro_filter_initialized = true;
            }

            if (resting) {
                constexpr float BIAS_ALPHA = 0.025f;
                for (int i = 0; i < 3; ++i)
                    d.gyro_bias[i] = d.gyro_bias[i] * (1.0f - BIAS_ALPHA) + g[i] * BIAS_ALPHA;
            }

            for (int i = 0; i < 3; ++i) {
                g[i] -= d.gyro_bias[i];
                g[i] = motion_deadzone_float(g[i], 24.0f);
            }

            out.gx = clamp_motion_i16(g[0]);
            out.gy = clamp_motion_i16(g[1]);
            out.gz = clamp_motion_i16(g[2]);

            has_motion = true;
        }
    }

    Device* device_for_slot_locked(int slot) {
        for (auto& d : devices) if (d.slot == slot) return &d;
        return nullptr;
    }

    bool has_device_locked(SDL_JoystickID id) const {
        for (const auto& d : devices) if (d.id == id) return true;
        return false;
    }

    int first_free_slot_locked() const {
        bool used[4] = {false, false, false, false};
        for (const auto& d : devices) {
            if (d.slot >= 0 && d.slot < 4) used[d.slot] = true;
        }
        for (int i = 0; i < 4; ++i) if (!used[i]) return i;
        return -1;
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

            // Nintendo controllers are safe and needed for the project's main gyro path.
            // Sony controllers on Windows can disconnect/re-enumerate when SDL switches them
            // into enhanced report/sensor mode. Keep Sony sensors disabled by default so
            // clicking Connect cannot kick the controller off the PC.
            const bool allow_sensors = !is_sony_controller(d);
            if (allow_sensors && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
                d.accel_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
            }
            if (allow_sensors && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
                d.gyro_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
            }

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
            apply_motion(d, st.motion, st.has_motion);
            if (report_non_neutral(st.input) || st.has_motion) st.last_input_us = now;
            states[d.slot] = st;
        }
    }
};

static SDLInputManager g_sdlInput;

class RumbleManager {
public:
    void apply_precision_packet(const ns::PrecisionRumblePacket& rp,
                         const int sdl_for_slot[4]) {
        if (rp.subpad >= 4) return;

        // Normal-rumble build:
        // Treat PrecisionRumblePacket as a carrier for already-decoded classic
        // low/high magnitudes. Do NOT send the 8-byte HD payload to SDL.
        //
        // The backend may send PrecisionRumblePacket followed by RumblePacket as
        // fallback. Apply the decoded classic rumble once, then suppress the
        // immediate duplicate fallback.
        ns::RumblePacket fallback{};
        fallback.magic = ns::RUMBLE_MAGIC;
        fallback.subpad = rp.subpad;
        fallback.low_freq = rp.low_freq;
        fallback.high_freq = rp.high_freq;
        fallback.duration_10ms = rp.duration_10ms;

        apply_packet(fallback, sdl_for_slot);
        states[rp.subpad].suppress_classic_until_us = ns::now_us() + 20000ULL;
    }

    void apply_packet(const ns::RumblePacket& rp,
                      const int sdl_for_slot[4]) {
        if (rp.subpad >= 4) return;
        const int slot = rp.subpad;
        if (ns::now_us() < states[slot].suppress_classic_until_us) return;

        const uint8_t low = rp.low_freq;
        const uint8_t high = rp.high_freq;
        const bool neutral = (low == 0 && high == 0) || rp.duration_10ms == 0;

        const uint64_t now = ns::now_us();
        const uint64_t dur_us = neutral ? 0ULL : (uint64_t)rp.duration_10ms * 10000ULL;
        const uint32_t dur_ms = neutral ? 0U : (uint32_t)std::min<uint64_t>(dur_us / 1000ULL, 0xFFFFFFFFULL);

        // Time-accurate normal rumble: apply every event. Do not throttle small
        // packets, because short micro-rumble is part of the game's feedback.
        states[slot].low = low;
        states[slot].high = high;
        states[slot].until_us = neutral ? 0 : now + dur_us;
        states[slot].duration_ms = dur_ms;
        states[slot].last_set_us = now;

        set_output(slot, neutral ? 0 : low, neutral ? 0 : high,
                   sdl_for_slot[slot], dur_ms);
    }

    void update_timeouts(const int sdl_for_slot[4]) {
        uint64_t now = ns::now_us();
        for (int i = 0; i < 4; ++i) {
            if (states[i].until_us != 0 && now > states[i].until_us) {
                states[i].until_us = 0;
                states[i].low = states[i].high = 0;
                states[i].duration_ms = 0;
                set_output(i, 0, 0, sdl_for_slot[i], 0);
            }
        }
    }

    void stop_all() {
        for (int i = 0; i < 4; ++i) states[i] = SlotState{};
        g_sdlInput.stop_all_rumble();
    }

private:
    struct SlotState {
        uint8_t low = 0, high = 0;
        uint64_t until_us = 0;
        uint64_t last_set_us = 0;
        uint32_t duration_ms = 0;
        int last_sdl = -1;
        uint64_t suppress_classic_until_us = 0;
    } states[4];

    void stop_previous_if_moved(int slot, int sdl_idx) {
        if (states[slot].last_sdl != -1 && states[slot].last_sdl != sdl_idx) {
            g_sdlInput.set_rumble(states[slot].last_sdl, 0, 0, 0);
        }
    }

    void set_output(int slot, uint8_t low, uint8_t high, int sdl_idx, uint32_t duration_ms) {
        stop_previous_if_moved(slot, sdl_idx);
        if (sdl_idx >= 0) {
            g_sdlInput.set_rumble(sdl_idx, low, high, duration_ms);
        }
        states[slot].last_sdl = sdl_idx;
    }
};

static void pump_udp_rumble(SOCKET sock,
                            RumbleManager& rumble,
                            const int sdl_for_slot[4]) {
    uint8_t buf[64];
    for (;;) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK || e == WSAEINTR) break;
            break;
        }
        if (n == (int)sizeof(ns::PrecisionRumblePacket)) {
            ns::PrecisionRumblePacket rp{};
            memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::PRECISION_RUMBLE_MAGIC)
                rumble.apply_precision_packet(rp, sdl_for_slot);
        } else if (n == (int)sizeof(ns::RumblePacket)) {
            ns::RumblePacket rp{};
            memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::RUMBLE_MAGIC)
                rumble.apply_packet(rp, sdl_for_slot);
        }
    }
}


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

// ── Global UI state ──
static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;
static HWND g_hIpEdit = nullptr;
static HWND g_hConnectBtn = nullptr;
static HWND g_hMacrosBtn = nullptr;
static HWND g_hStatusText = nullptr;
static HWND g_hP1Text = nullptr;
static HWND g_hP2Text = nullptr;
static HWND g_hP3Text = nullptr;
static HWND g_hP4Text = nullptr;

static std::atomic<bool> g_senderRunning{false};
static std::atomic<bool> g_connected{false};
static std::thread g_senderThread;
static SOCKET g_sock = INVALID_SOCKET;
static uint8_t g_hmacKey[32]{};
static uint32_t g_packetCount = 0;
static std::string g_targetHost;
static uint16_t g_targetPort = ns::DEFAULT_PORT;

// ── Keyboard Mode ──
enum { KB_OFF = 0, KB_SINGLE = 1, KB_OVERRIDE = 2 };
static std::atomic<int> g_keyboardMode{KB_OFF};
static std::unordered_map<std::string, std::string> g_keyBindings;
static HWND g_hKeyboardCombo = nullptr;
static HWND g_hBindingsBtn = nullptr;

static const wchar_t* REG_KEY_BIND = L"Software\\NSPCControl\\Bindings";

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

static std::wstring widen(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    return w;
}

static std::string narrow(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

static void LoadSavedBindings() {
    HKEY hKey = nullptr;
    g_keyBindings = default_key_bindings();
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_KEY_BIND, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        for (auto& [name, def] : g_keyBindings) {
            wchar_t buf[32]{};
            DWORD len = sizeof(buf);
            DWORD type = 0;
            RegQueryValueExW(hKey, widen(name).c_str(), nullptr, &type, (LPBYTE)buf, &len);
            if (type == REG_SZ)
                g_keyBindings[name] = narrow(buf);
        }
        RegCloseKey(hKey);
    }
}

static void SaveBindings() {
    HKEY hKey = nullptr;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_KEY_BIND, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        for (auto& [name, val] : g_keyBindings) {
            std::wstring wval = widen(val);
            RegSetValueExW(hKey, widen(name).c_str(), 0, REG_SZ, (const BYTE*)wval.c_str(), (DWORD)((wval.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(hKey);
    }
}

// ── Registry helpers ──
static const wchar_t* REG_KEY = L"Software\\NSPCControl";
static const wchar_t* REG_VAL_IP = L"LastIP";
static const wchar_t* REG_VAL_KB_MODE = L"KeyboardMode";

static std::wstring LoadSavedIP() {
    HKEY hKey = nullptr;
    std::wstring ip;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[64]{};
        DWORD len = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueEx(hKey, REG_VAL_IP, nullptr, &type, (LPBYTE)buf, &len) == ERROR_SUCCESS && type == REG_SZ)
            ip = buf;
        RegCloseKey(hKey);
    }
    return ip;
}

static void SaveLastIP(const wchar_t* ip) {
    HKEY hKey = nullptr;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueEx(hKey, REG_VAL_IP, 0, REG_SZ, (const BYTE*)ip, (DWORD)((wcslen(ip) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

static int LoadSavedKeyboardMode() {
    HKEY hKey = nullptr;
    int mode = 0;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 0;
        DWORD len = sizeof(val);
        DWORD type = 0;
        if (RegQueryValueEx(hKey, REG_VAL_KB_MODE, nullptr, &type, (LPBYTE)&val, &len) == ERROR_SUCCESS && type == REG_DWORD)
            mode = (int)val;
        RegCloseKey(hKey);
    }
    return mode;
}

static void SaveKeyboardMode(int mode) {
    HKEY hKey = nullptr;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD val = (DWORD)mode;
        RegSetValueEx(hKey, REG_VAL_KB_MODE, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
    }
}


enum { IDC_IP = 101, IDC_CONNECT, IDC_KEYBOARD_COMBO = 110, IDC_BINDINGS_BTN = 111, IDC_MACROS_BTN = 112, IDC_EDITOR_CHANGE = 200, IDC_EDITOR_SETUP = 400, IDC_EDITOR_RESET = 500, IDC_EDITOR_CLEAR = 501, IDC_EDITOR_KEY_START = 300, IDC_MACRO_EDIT = 600, IDC_MACRO_RUN = 601, IDC_MACRO_SAVE = 602, IDC_MACRO_DELETE = 603, IDC_MACRO_CLOSE = 604, IDC_MACRO_RECORD_START = 605, IDC_MACRO_RECORD_STOP = 606, IDC_MACRO_IMPORT = 607, IDC_MACRO_EXPORT = 608, IDC_MACRO_HOTKEY = 609, IDC_MACRO_NAME = 610, IDC_MACRO_ADD = 611, IDC_MACRO_RECORD_TOGGLE = 612, IDC_MACRO_RUN_BASE = 700, IDC_MACRO_DELETE_BASE = 800, IDC_MACRO_KEY_BASE = 900, IDC_MACRO_RENAME_BASE = 1000, IDC_MACRO_EXPORT_BASE = 1100 };
static HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id);

// ── Macro runtime/editor ────────────────────────────────────────────────────
static std::mutex g_macro_mtx;
static std::vector<MacroStep> g_macro_steps;
static bool g_macro_running = false;
static uint64_t g_macro_start_us = 0;
static std::string g_macro_text;
static std::string g_macro_upload_pending;
static const wchar_t* REG_VAL_MACROS = L"MacrosJson";
static const wchar_t* REG_VAL_MACRO_HOTKEY = L"MacroHotkey";

struct MacroEntry {
    std::string name;
    std::string hotkey;
    std::string json;
};

static std::vector<MacroEntry> g_macro_entries;
static std::unordered_map<std::string, bool> g_macro_hotkey_down;
static bool g_macro_editor_recording = false;
static HWND g_hMacroRecordToggleBtn = nullptr;
static std::vector<HWND> g_macro_row_controls;
static int g_macro_listening_idx = -1;
static HWND g_macro_listening_static = nullptr;
static constexpr UINT_PTR MACRO_RECORD_TIMER_ID = 2;

static bool key_is_down(const std::string& name);
static void apply_keyboard_to_report(ns::HIDReport& rep, bool override_mode);

static std::string MacroConfigDirA() {
    char appdata[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::string dir = std::string(appdata) + "\\NSPCControl";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }

    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string path = exe;
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

static std::string MacroFilePathA() {
    return MacroConfigDirA() + "\\macros.json";
}

static std::string normalize_key_name(std::string s) {
    s = macro_upper(macro_trim(s));
    return s;
}

static std::string vk_to_key_name(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    struct { UINT vk; const char* n; } map[] = {
        {VK_UP,"UP"},{VK_DOWN,"DOWN"},{VK_LEFT,"LEFT"},{VK_RIGHT,"RIGHT"},
        {VK_LSHIFT,"LSHIFT"},{VK_RSHIFT,"RSHIFT"},
        {VK_LCONTROL,"LCTRL"},{VK_RCONTROL,"RCTRL"},
        {VK_LMENU,"LALT"},{VK_RMENU,"RALT"},
        {VK_SPACE,"SPACE"},{VK_RETURN,"ENTER"},{VK_TAB,"TAB"},
        {VK_ESCAPE,"ESC"},{VK_BACK,"BACKSPACE"},
        {VK_F1,"F1"},{VK_F2,"F2"},{VK_F3,"F3"},{VK_F4,"F4"},
        {VK_F5,"F5"},{VK_F6,"F6"},{VK_F7,"F7"},{VK_F8,"F8"},
        {VK_F9,"F9"},{VK_F10,"F10"},{VK_F11,"F11"},{VK_F12,"F12"},
        {VK_HOME,"HOME"},{VK_SNAPSHOT,"SNAPSHOT"},
    };
    for (auto& m : map)
        if (vk == m.vk) return m.n;
    return "";
}

static bool json_find_string_value(const std::string& raw, const std::string& key, std::string& out) {
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

static bool find_json_array_range_for_key(const std::string& raw, const std::string& key, size_t& begin, size_t& end) {
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

static std::vector<std::string> split_top_level_objects(const std::string& raw, size_t begin, size_t end) {
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

static bool MacroHotkeyConflicts(const std::string& hotkey, std::string* conflict_name = nullptr) {
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

static bool MacroEntryHotkeyConflicts(const std::string& hotkey, int skip_index, std::string* conflict_name = nullptr) {
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

static bool ValidateMacroHotkeyForEntry(const std::string& hotkey, int skip_index, HWND parent) {
    std::string conflict;
    if (MacroHotkeyConflicts(hotkey, &conflict)) {
        MessageBoxW(parent, widen("Macro keybind conflicts with keyboard binding: " + conflict).c_str(),
                    L"Macro keybind", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (MacroEntryHotkeyConflicts(hotkey, skip_index, &conflict)) {
        MessageBoxW(parent, widen("Macro keybind is already used by: " + conflict).c_str(),
                    L"Macro keybind", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

static int FindMacroEntryByName(const std::string& name) {
    std::string wanted = macro_upper(macro_trim(name));
    if (wanted.empty()) return -1;
    for (int i = 0; i < (int)g_macro_entries.size(); ++i) {
        if (macro_upper(g_macro_entries[i].name) == wanted) return i;
    }
    return -1;
}

static std::string UniqueMacroName(const std::string& base_raw) {
    std::string base = macro_trim(base_raw);
    if (base.empty()) base = "Recorded Macro";
    std::string name = base;
    int suffix = 2;
    while (FindMacroEntryByName(name) >= 0) {
        name = base + " " + std::to_string(suffix++);
    }
    return name;
}

static void RebuildMacroHotkeyState() {
    g_macro_hotkey_down.clear();
    for (const auto& e : g_macro_entries) {
        std::string hk = normalize_key_name(e.hotkey);
        if (!hk.empty()) g_macro_hotkey_down[hk] = false;
    }
}

static std::string MacroEntryToObjectJson(const MacroEntry& e) {
    std::vector<MacroStep> steps;
    std::vector<std::string> lines;
    if (!macro_validate_text(e.json, steps, &lines)) lines = {"WAIT 200"};

    std::string name = macro_trim(e.name).empty() ? macro_extract_name_or_default(e.json, "Macro") : e.name;
    std::string out;
    out += "    {\n";
    out += "      \"name\": \"" + macro_escape_json(name) + "\",\n";
    out += "      \"hotkey\": \"" + macro_escape_json(normalize_key_name(e.hotkey)) + "\",\n";
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

static std::string MacroPrettyJsonWithForcedName(const std::string& raw_text, const std::string& forced_name) {
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

static std::string MacroEntriesToJson(const std::vector<MacroEntry>& entries) {
    std::string out;
    out += "{\n";
    out += "  \"macros\": [\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        out += MacroEntryToObjectJson(entries[i]);
        if (i + 1 < entries.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

static bool ParseMacroEntriesText(const std::string& raw, std::vector<MacroEntry>& out, std::string& err) {
    out.clear();
    err.clear();
    if (raw.size() > MACRO_JSON_MAX_BYTES) { err = "macro JSON exceeds 50MB limit"; return false; }
    std::string t = macro_trim(raw);
    if (t.empty()) return true;

    size_t arr_begin = 0, arr_end = 0;
    if (find_json_array_range_for_key(t, "macros", arr_begin, arr_end)) {
        auto objects = split_top_level_objects(t, arr_begin, arr_end);
        for (const std::string& obj : objects) {
            std::string pretty;
            if (!macro_validate_to_pretty_json(obj, pretty, err, "Macro")) return false;
            MacroEntry e;
            e.json = pretty;
            e.name = macro_extract_name_or_default(obj, "Macro");
            json_find_string_value(obj, "hotkey", e.hotkey);
            e.hotkey = normalize_key_name(e.hotkey);
            out.push_back(e);
        }
        return true;
    }

    // Backward compatibility: old builds stored one macro directly.
    std::string pretty;
    if (!macro_validate_to_pretty_json(t, pretty, err, "Macro")) return false;
    MacroEntry e;
    e.json = pretty;
    e.name = macro_extract_name_or_default(t, "Macro");
    json_find_string_value(t, "hotkey", e.hotkey);
    e.hotkey = normalize_key_name(e.hotkey);
    out.push_back(e);
    return true;
}

static std::string LoadSavedMacrosText() {
    std::string path = MacroFilePathA();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (f) {
        std::streamoff len = f.tellg();
        if (len >= 0 && (uint64_t)len <= MACRO_JSON_MAX_BYTES) {
            f.seekg(0, std::ios::beg);
            std::string out((size_t)len, '\0');
            if (len > 0) f.read(&out[0], len);
            if (f || len == 0) return out;
        }
    }

    HKEY hKey = nullptr;
    std::string out;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[8192]{}; DWORD len = sizeof(buf); DWORD type = 0;
        if (RegQueryValueExW(hKey, REG_VAL_MACROS, nullptr, &type, (LPBYTE)buf, &len) == ERROR_SUCCESS && type == REG_SZ)
            out = narrow(buf);
        RegCloseKey(hKey);
    }
    return out;
}

static std::string LoadSavedMacroHotkey() {
    HKEY hKey = nullptr;
    std::string out;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[64]{}; DWORD len = sizeof(buf); DWORD type = 0;
        if (RegQueryValueExW(hKey, REG_VAL_MACRO_HOTKEY, nullptr, &type, (LPBYTE)buf, &len) == ERROR_SUCCESS && type == REG_SZ)
            out = normalize_key_name(narrow(buf));
        RegCloseKey(hKey);
    }
    return out;
}

static void LoadMacroEntries() {
    std::string err;
    std::vector<MacroEntry> loaded;
    if (!ParseMacroEntriesText(LoadSavedMacrosText(), loaded, err)) loaded.clear();

    if (loaded.size() == 1 && loaded[0].hotkey.empty()) {
        // One-time migration from older single-macro registry hotkey.
        loaded[0].hotkey = LoadSavedMacroHotkey();
    }

    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_entries = std::move(loaded);
    RebuildMacroHotkeyState();
    g_macro_text = MacroEntriesToJson(g_macro_entries);
}

static bool SaveMacroEntriesToDisk(HWND parent = g_hWnd) {
    std::string json;
    {
        json = MacroEntriesToJson(g_macro_entries);
    }
    if (json.size() > MACRO_JSON_MAX_BYTES) {
        MessageBoxW(parent, L"Macro JSON exceeds the 50MB limit.", L"Macro save", MB_OK | MB_ICONWARNING);
        return false;
    }
    std::string path = MacroFilePathA();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        MessageBoxW(parent, widen("Could not save macros to " + path).c_str(), L"Macro save", MB_OK | MB_ICONERROR);
        return false;
    }
    f.write(json.data(), (std::streamsize)json.size());
    if (!f) {
        MessageBoxW(parent, L"Failed while writing macros file.", L"Macro save", MB_OK | MB_ICONERROR);
        return false;
    }
    g_macro_text = json;
    return true;
}

static bool StartMacroText(const std::string& txt) {
    std::vector<MacroStep> parsed;
    if (!macro_validate_text(txt, parsed, nullptr)) {
        MessageBoxW(g_hWnd, widen("Invalid macro: " + macro_last_error()).c_str(), L"Macro validation", MB_OK | MB_ICONWARNING);
        return false;
    }
    std::string pretty, err;
    if (!macro_validate_to_pretty_json(txt, pretty, err, "Macro")) {
        MessageBoxW(g_hWnd, widen("Invalid macro: " + err).c_str(), L"Macro validation", MB_OK | MB_ICONWARNING);
        return false;
    }
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_upload_pending = pretty;
    g_macro_steps = std::move(parsed);
    g_macro_running = false; // server-side playback handles merging; local fallback stays off by default
    g_macro_start_us = ns::now_us();
    return true;
}

static bool UpsertMacroEntry(HWND parent, MacroEntry e, bool force_unique_name) {
    std::string pretty, err;
    if (!macro_validate_to_pretty_json(e.json, pretty, err, e.name.empty() ? "Macro" : e.name)) {
        MessageBoxW(parent, widen("Invalid macro: " + err).c_str(), L"Macro validation", MB_OK | MB_ICONWARNING);
        return false;
    }

    e.name = macro_trim(e.name);
    if (e.name.empty()) e.name = macro_extract_name_or_default(pretty, "Macro");
    if (force_unique_name) e.name = UniqueMacroName(e.name);
    e.hotkey = normalize_key_name(e.hotkey);
    e.json = MacroPrettyJsonWithForcedName(pretty, e.name);

    int existing = force_unique_name ? -1 : FindMacroEntryByName(e.name);
    if (!ValidateMacroHotkeyForEntry(e.hotkey, existing, parent)) {
        e.hotkey.clear();
    }

    if (existing >= 0) g_macro_entries[existing] = std::move(e);
    else g_macro_entries.push_back(std::move(e));
    RebuildMacroHotkeyState();
    return SaveMacroEntriesToDisk(parent);
}

static bool ReadTextFileDialog(HWND parent, std::string& out) {
    out.clear();
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = parent; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0"; ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    FILE* f = _wfopen(file, L"rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0 || (uint64_t)len > MACRO_JSON_MAX_BYTES) { fclose(f); MessageBoxW(parent, L"Macro JSON exceeds the 50MB limit.", L"Macro validation", MB_OK | MB_ICONWARNING); return false; }
    out.assign((size_t)std::max<long>(0, len), '\0');
    if (len > 0) fread(&out[0], 1, (size_t)len, f);
    fclose(f);
    return true;
}

static void PollMacroEntryHotkeys() {
    std::vector<std::string> to_run;
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        for (const auto& e : g_macro_entries) {
            std::string hk = normalize_key_name(e.hotkey);
            if (hk.empty()) continue;
            if (MacroHotkeyConflicts(hk, nullptr)) continue;
            bool down = key_is_down(hk);
            bool was_down = g_macro_hotkey_down[hk];
            g_macro_hotkey_down[hk] = down;
            if (down && !was_down) to_run.push_back(e.json);
        }
    }
    for (const auto& json : to_run) StartMacroText(json);
}

static bool g_macro_recording = false;
struct MacroRecordFrame {
    uint16_t buttons = 0;
    uint8_t hat = ns::HAT_NEUTRAL;
    int8_t lx = 0;
    int8_t ly = 0;
    int8_t rx = 0;
    int8_t ry = 0;
};

static MacroRecordFrame g_macro_record_last_frame{};
static bool g_macro_record_have_frame = false;
static bool g_macro_record_has_input = false;
static uint64_t g_macro_record_last_change_us = 0;
static std::string g_macro_record_commands;

static bool operator==(const MacroRecordFrame& a, const MacroRecordFrame& b) {
    return a.buttons == b.buttons && a.hat == b.hat &&
           a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
}

static bool operator!=(const MacroRecordFrame& a, const MacroRecordFrame& b) {
    return !(a == b);
}

static std::string macro_buttons_to_text(uint16_t buttons) {
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

static MacroRecordFrame MacroRecordFrameFromReport(const ns::HIDReport& report) {
    auto axis_dir = [](uint8_t v) -> int8_t {
        if (v < 80) return -1;
        if (v > 176) return 1;
        return 0;
    };

    MacroRecordFrame f{};
    f.buttons = report.buttons;
    f.hat = report.hat;
    f.lx = axis_dir(report.lx);
    f.ly = axis_dir(report.ly);
    f.rx = axis_dir(report.rx);
    f.ry = axis_dir(report.ry);
    return f;
}

static void macro_append_token(std::string& out, const char* token) {
    if (!out.empty()) out += "+";
    out += token;
}

static std::string macro_record_frame_to_text(const MacroRecordFrame& f) {
    std::string out = macro_buttons_to_text(f.buttons);
    switch (f.hat) {
        case ns::HAT_N:  macro_append_token(out, "DPAD_UP"); break;
        case ns::HAT_NE: macro_append_token(out, "DPAD_UP"); macro_append_token(out, "DPAD_RIGHT"); break;
        case ns::HAT_E:  macro_append_token(out, "DPAD_RIGHT"); break;
        case ns::HAT_SE: macro_append_token(out, "DPAD_DOWN"); macro_append_token(out, "DPAD_RIGHT"); break;
        case ns::HAT_S:  macro_append_token(out, "DPAD_DOWN"); break;
        case ns::HAT_SW: macro_append_token(out, "DPAD_DOWN"); macro_append_token(out, "DPAD_LEFT"); break;
        case ns::HAT_W:  macro_append_token(out, "DPAD_LEFT"); break;
        case ns::HAT_NW: macro_append_token(out, "DPAD_UP"); macro_append_token(out, "DPAD_LEFT"); break;
        default: break;
    }
    if (f.lx < 0) macro_append_token(out, "LSTICK_LEFT");
    else if (f.lx > 0) macro_append_token(out, "LSTICK_RIGHT");
    if (f.ly < 0) macro_append_token(out, "LSTICK_UP");
    else if (f.ly > 0) macro_append_token(out, "LSTICK_DOWN");
    if (f.rx < 0) macro_append_token(out, "RSTICK_LEFT");
    else if (f.rx > 0) macro_append_token(out, "RSTICK_RIGHT");
    if (f.ry < 0) macro_append_token(out, "RSTICK_UP");
    else if (f.ry > 0) macro_append_token(out, "RSTICK_DOWN");
    return out;
}

static void MacroRecordAppendLocked(const MacroRecordFrame& frame, uint64_t duration_ms) {
    if (duration_ms < 10) return;
    if (!g_macro_record_commands.empty()) g_macro_record_commands += "; ";
    std::string combo = macro_record_frame_to_text(frame);
    if (combo.empty()) {
        g_macro_record_commands += "WAIT " + std::to_string(duration_ms);
    } else {
        g_macro_record_has_input = true;
        g_macro_record_commands += combo + " " + std::to_string(duration_ms);
    }
}

static void MacroRecordStart() {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    g_macro_recording = true;
    g_macro_record_last_frame = MacroRecordFrame{};
    g_macro_record_have_frame = false;
    g_macro_record_has_input = false;
    g_macro_record_last_change_us = ns::now_us();
    g_macro_record_commands.clear();
}

static std::string MacroRecordStop() {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    if (g_macro_recording && g_macro_record_have_frame) {
        uint64_t now = ns::now_us();
        MacroRecordAppendLocked(g_macro_record_last_frame, (now - g_macro_record_last_change_us) / 1000ULL);
    }
    g_macro_recording = false;
    g_macro_record_have_frame = false;
    if (!g_macro_record_has_input) {
        g_macro_record_commands.clear();
        return "";
    }
    std::string commands = g_macro_record_commands;
    return macro_pretty_json(commands, "Recorded Macro");
}

static void MacroRecordSample(const ns::HIDReport& report) {
    std::lock_guard<std::mutex> lk(g_macro_mtx);
    if (!g_macro_recording || g_macro_running) return;
    uint64_t now = ns::now_us();
    MacroRecordFrame frame = MacroRecordFrameFromReport(report);
    if (!g_macro_record_have_frame) {
        g_macro_record_last_frame = frame;
        g_macro_record_have_frame = true;
        g_macro_record_last_change_us = now;
        return;
    }
    if (frame != g_macro_record_last_frame) {
        MacroRecordAppendLocked(g_macro_record_last_frame, (now - g_macro_record_last_change_us) / 1000ULL);
        g_macro_record_last_frame = frame;
        g_macro_record_last_change_us = now;
    }
}

static bool PollMacroRecordP1(ns::HIDReport& report) {
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

static void MacroRecordSampleP1() {
    ns::HIDReport report;
    PollMacroRecordP1(report);
    MacroRecordSample(report);
}
static bool ApplyMacroOverride(ns::HIDReport logicalReports[4], bool present[4], bool hasMotion[4]) {
    (void)hasMotion;

    std::lock_guard<std::mutex> lk(g_macro_mtx);
    if (!g_macro_running) return false;

    uint64_t elapsed_ms = (ns::now_us() - g_macro_start_us) / 1000ULL;
    ns::HIDReport mr;
    bool active = macro_report_at(g_macro_steps, elapsed_ms, mr);

    if (active) {
        logicalReports[0].buttons |= mr.buttons;
        if (mr.hat != ns::HAT_NEUTRAL) logicalReports[0].hat = mr.hat;
        if (mr.lx != 128 || mr.ly != 128) {
            logicalReports[0].lx = mr.lx;
            logicalReports[0].ly = mr.ly;
        }
        if (mr.rx != 128 || mr.ry != 128) {
            logicalReports[0].rx = mr.rx;
            logicalReports[0].ry = mr.ry;
        }
        present[0] = true;
    }

    if (!active && elapsed_ms > macro_total_ms(g_macro_steps) + 120) {
        g_macro_running = false;
        return false;
    }
    return active;
}
static HFONT MacroEditorFont() {
    static HFONT hFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    return hFont;
}

static HWND CreateMacroButton(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id) {
    HWND hw = CreateWindowW(L"BUTTON", text, WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    SendMessage(hw, WM_SETFONT, (WPARAM)MacroEditorFont(), TRUE);
    return hw;
}

static HWND CreateMacroStatic(HWND parent, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
    HWND hw = CreateWindowW(L"STATIC", text, WS_VISIBLE | WS_CHILD | style,
        x, y, w, h, parent, id ? (HMENU)(INT_PTR)id : nullptr, g_hInst, nullptr);
    SendMessage(hw, WM_SETFONT, (WPARAM)MacroEditorFont(), TRUE);
    return hw;
}


struct MacroRenamePromptContext {
    HWND edit = nullptr;
    bool done = false;
    bool ok = false;
    std::string initial;
    std::string result;
};

static LRESULT CALLBACK MacroRenamePromptProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<MacroRenamePromptContext*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = reinterpret_cast<MacroRenamePromptContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        HFONT font = MacroEditorFont();

        HWND label = CreateWindowW(L"STATIC", L"Macro name:", WS_VISIBLE | WS_CHILD,
            18, 16, 304, 18, hWnd, nullptr, g_hInst, nullptr);
        SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);

        std::wstring initial = widen(ctx ? ctx->initial : std::string());
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial.c_str(),
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            18, 40, 314, 24, hWnd, (HMENU)100, g_hInst, nullptr);
        SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
        if (ctx) ctx->edit = edit;

        HWND ok = CreateWindowW(L"BUTTON", L"OK", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON,
            172, 78, 76, 28, hWnd, (HMENU)IDOK, g_hInst, nullptr);
        SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
        HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            256, 78, 76, 28, hWnd, (HMENU)IDCANCEL, g_hInst, nullptr);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);

        SendMessageW(hWnd, DM_SETDEFID, IDOK, 0);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDOK && ctx) {
            int len = GetWindowTextLengthW(ctx->edit);
            std::vector<wchar_t> buf((size_t)len + 1, L'\0');
            GetWindowTextW(ctx->edit, buf.data(), len + 1);
            ctx->result = macro_trim(narrow(buf.data()));
            ctx->ok = true;
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == IDCANCEL && ctx) {
            ctx->done = true;
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (ctx) ctx->done = true;
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool EnsureMacroRenamePromptClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = MacroRenamePromptProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NSMacroRenamePrompt";
    ATOM atom = RegisterClassW(&wc);
    registered = atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

static bool PromptForMacroName(HWND parent, const std::string& current_name, std::string& out_name) {
    out_name.clear();
    if (!EnsureMacroRenamePromptClass()) return false;

    MacroRenamePromptContext ctx;
    ctx.initial = current_name;

    constexpr int dlgW = 360;
    constexpr int dlgH = 150;
    RECT pr{};
    GetWindowRect(parent, &pr);
    int x = pr.left + ((pr.right - pr.left) - dlgW) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - dlgH) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NSMacroRenamePrompt", L"Rename Macro",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        x, y, dlgW, dlgH, parent, nullptr, g_hInst, &ctx);
    if (!dlg) return false;

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    MSG msg;
    while (!ctx.done && IsWindow(dlg)) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    SetFocus(parent);

    if (!ctx.ok) return false;
    out_name = ctx.result;
    return true;
}

static void BeginMacroHotkeyListen(HWND hWnd, int idx) {
    if (idx < 0 || idx >= (int)g_macro_entries.size()) return;
    g_macro_listening_idx = idx;
    g_macro_listening_static = GetDlgItem(hWnd, IDC_MACRO_KEY_BASE + idx);
    if (g_macro_listening_static) SetWindowTextW(g_macro_listening_static, L"...");
    SetFocus(hWnd);
}

static void DestroyMacroRows() {
    for (HWND h : g_macro_row_controls) if (h) DestroyWindow(h);
    g_macro_row_controls.clear();
}

static void RefreshMacroRows(HWND hWnd) {
    DestroyMacroRows();
    constexpr int x = 14;
    constexpr int rowH = 26;
    constexpr int nameW = 250;
    constexpr int keyW = 110;
    constexpr int btnW = 68;
    constexpr int exportW = 64;
    constexpr int delW = 64;
    constexpr int gap = 4;
    const int keyX = x + nameW + gap;
    const int renameX = keyX + keyW + gap;
    const int exportX = renameX + btnW + gap;
    const int deleteX = exportX + exportW + gap;
    int y = 12;

    if (g_macro_entries.empty()) {
        HWND empty = CreateMacroStatic(hWnd, L"No macros", SS_CENTER, x, y, nameW, 22);
        g_macro_row_controls.push_back(empty);
        y += rowH;
    }

    for (int i = 0; i < (int)g_macro_entries.size(); ++i) {
        const auto& e = g_macro_entries[i];
        std::wstring name = widen(e.name.empty() ? "Macro" : e.name);
        std::wstring key = widen(normalize_key_name(e.hotkey));
        HWND run = CreateMacroButton(hWnd, name.c_str(), x, y, nameW, 24, IDC_MACRO_RUN_BASE + i);
        HWND keyText = CreateMacroStatic(hWnd, key.c_str(), SS_CENTER | SS_NOTIFY | WS_BORDER,
                                         keyX, y, keyW, 22, IDC_MACRO_KEY_BASE + i);
        HWND rename = CreateMacroButton(hWnd, L"Rename", renameX, y, btnW, 24, IDC_MACRO_RENAME_BASE + i);
        HWND exp = CreateMacroButton(hWnd, L"Export", exportX, y, exportW, 24, IDC_MACRO_EXPORT_BASE + i);
        HWND del = CreateMacroButton(hWnd, L"Delete", deleteX, y, delW, 24, IDC_MACRO_DELETE_BASE + i);
        g_macro_row_controls.push_back(run);
        g_macro_row_controls.push_back(keyText);
        g_macro_row_controls.push_back(rename);
        g_macro_row_controls.push_back(exp);
        g_macro_row_controls.push_back(del);
        y += rowH;
    }

    y += 8;
    const int dialogW = 620;
    const int rightBtnX = deleteX + delW - 88;
    HWND importBtn = CreateMacroButton(hWnd, L"Import", x, y, 88, 30, IDC_MACRO_IMPORT);
    g_hMacroRecordToggleBtn = CreateMacroButton(hWnd, L"Record P1", rightBtnX, y, 88, 30, IDC_MACRO_RECORD_TOGGLE);
    y += 38;
    HWND exportBtn = CreateMacroButton(hWnd, L"Export", x, y, 88, 30, IDC_MACRO_EXPORT);
    HWND closeBtn = CreateMacroButton(hWnd, L"Close", rightBtnX, y, 88, 30, IDC_MACRO_CLOSE);
    g_macro_row_controls.push_back(importBtn);
    g_macro_row_controls.push_back(g_hMacroRecordToggleBtn);
    g_macro_row_controls.push_back(exportBtn);
    g_macro_row_controls.push_back(closeBtn);

    y += 38;
    RECT rc{0, 0, dialogW, std::max(190, y + 18)};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    SetWindowPos(hWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
}

static bool MacroReadFileDialog(HWND parent) {
    std::string raw;
    if (!ReadTextFileDialog(parent, raw)) return false;

    std::vector<MacroEntry> imported;
    std::string err;
    if (ParseMacroEntriesText(raw, imported, err) && !imported.empty()) {
        if (imported.size() > 1 || raw.find("\"macros\"") != std::string::npos) {
            g_macro_entries = std::move(imported);
            RebuildMacroHotkeyState();
            SaveMacroEntriesToDisk(parent);
            return true;
        }
        return UpsertMacroEntry(parent, imported[0], false);
    }

    MessageBoxW(parent, widen("Invalid macro JSON: " + err).c_str(), L"Macro validation", MB_OK | MB_ICONWARNING);
    return false;
}

static std::wstring MacroSafeFileStemW(const std::string& raw_name) {
    std::string trimmed = macro_trim(raw_name);
    if (trimmed.empty()) trimmed = "Macro";

    std::wstring name = widen(trimmed);
    for (wchar_t& c : name) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' ||
            c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|' ||
            c < 32) {
            c = L'_';
        }
    }

    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty()) name = L"Macro";
    if (name.size() > 180) name.resize(180);
    return name;
}

static bool MacroWriteFileDialog(HWND parent, const std::string& text, const std::wstring& default_file = L"ns-macros.json") {
    wchar_t file[MAX_PATH]{};
    wcsncpy_s(file, default_file.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"JSON Files\0*.json\0All Files\0*.*\0";
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return false;

    FILE* f = _wfopen(file, L"wb");
    if (!f) return false;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return true;
}

static LRESULT CALLBACK MacroEditorProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        LoadMacroEntries();
        g_macro_editor_recording = false;
        g_macro_listening_idx = -1;
        g_macro_listening_static = nullptr;
        RefreshMacroRows(hWnd);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (g_macro_editor_recording && id != IDC_MACRO_RECORD_TOGGLE && id != IDC_MACRO_CLOSE) {
            return 0;
        }
        if (id >= IDC_MACRO_RUN_BASE && id < IDC_MACRO_RUN_BASE + 100) {
            int idx = id - IDC_MACRO_RUN_BASE;
            if (idx >= 0 && idx < (int)g_macro_entries.size()) StartMacroText(g_macro_entries[idx].json);
        } else if (id >= IDC_MACRO_EXPORT_BASE && id < IDC_MACRO_EXPORT_BASE + 100) {
            int idx = id - IDC_MACRO_EXPORT_BASE;
            if (idx >= 0 && idx < (int)g_macro_entries.size()) {
                std::vector<MacroEntry> one{g_macro_entries[idx]};
                std::wstring default_file = MacroSafeFileStemW(g_macro_entries[idx].name.empty() ? "Macro" : g_macro_entries[idx].name) + L".json";
                MacroWriteFileDialog(hWnd, MacroEntriesToJson(one), default_file);
            }
        } else if (id >= IDC_MACRO_DELETE_BASE && id < IDC_MACRO_DELETE_BASE + 100) {
            int idx = id - IDC_MACRO_DELETE_BASE;
            if (idx >= 0 && idx < (int)g_macro_entries.size()) {
                g_macro_entries.erase(g_macro_entries.begin() + idx);
                RebuildMacroHotkeyState();
                g_macro_listening_idx = -1;
                g_macro_listening_static = nullptr;
                SaveMacroEntriesToDisk(hWnd);
                RefreshMacroRows(hWnd);
            }
        } else if (id >= IDC_MACRO_KEY_BASE && id < IDC_MACRO_KEY_BASE + 100) {
            BeginMacroHotkeyListen(hWnd, id - IDC_MACRO_KEY_BASE);
        } else if (id >= IDC_MACRO_RENAME_BASE && id < IDC_MACRO_RENAME_BASE + 100) {
            int idx = id - IDC_MACRO_RENAME_BASE;
            if (idx >= 0 && idx < (int)g_macro_entries.size()) {
                g_macro_listening_idx = -1;
                g_macro_listening_static = nullptr;
                std::string new_name;
                std::string old_name = g_macro_entries[idx].name.empty() ? "Macro" : g_macro_entries[idx].name;
                if (PromptForMacroName(hWnd, old_name, new_name)) {
                    if (new_name.empty()) {
                        MessageBoxW(hWnd, L"Macro name cannot be empty.", L"Rename Macro", MB_OK | MB_ICONWARNING);
                    } else {
                        int duplicate = FindMacroEntryByName(new_name);
                        if (duplicate >= 0 && duplicate != idx) {
                            MessageBoxW(hWnd, L"Another macro already uses that name.", L"Rename Macro", MB_OK | MB_ICONWARNING);
                        } else {
                            g_macro_entries[idx].name = new_name;
                            g_macro_entries[idx].json = MacroPrettyJsonWithForcedName(g_macro_entries[idx].json, new_name);
                            SaveMacroEntriesToDisk(hWnd);
                            RefreshMacroRows(hWnd);
                        }
                    }
                }
            }
        } else if (id == IDC_MACRO_RECORD_TOGGLE) {
            if (!g_macro_editor_recording) {
                MacroRecordStart();
                MacroRecordSampleP1();
                g_macro_editor_recording = true;
                SetTimer(hWnd, MACRO_RECORD_TIMER_ID, 16, nullptr);
                if (g_hMacroRecordToggleBtn) SetWindowTextW(g_hMacroRecordToggleBtn, L"Stop");
            } else {
                MacroRecordSampleP1();
                std::string recorded = MacroRecordStop();
                g_macro_editor_recording = false;
                KillTimer(hWnd, MACRO_RECORD_TIMER_ID);
                if (!recorded.empty()) {
                    MacroEntry e;
                    e.name = "Recorded Macro";
                    e.hotkey = "";
                    e.json = recorded;
                    UpsertMacroEntry(hWnd, e, true);
                }
                RefreshMacroRows(hWnd);
            }
        } else if (id == IDC_MACRO_IMPORT) {
            if (MacroReadFileDialog(hWnd)) {
                RefreshMacroRows(hWnd);
            }
        } else if (id == IDC_MACRO_EXPORT) {
            SaveMacroEntriesToDisk(hWnd);
            MacroWriteFileDialog(hWnd, MacroEntriesToJson(g_macro_entries));
        } else if (id == IDC_MACRO_CLOSE) {
            PostMessageW(hWnd, WM_CLOSE, 0, 0);
        }
        break;
    }
    case WM_TIMER:
        if (wParam == MACRO_RECORD_TIMER_ID && g_macro_editor_recording) {
            MacroRecordSampleP1();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (g_macro_listening_idx >= 0) {
            int idx = g_macro_listening_idx;
            if (idx >= 0 && idx < (int)g_macro_entries.size()) {
                if ((UINT)wParam == VK_ESCAPE) {
                    g_macro_entries[idx].hotkey.clear();
                    RebuildMacroHotkeyState();
                    SaveMacroEntriesToDisk(hWnd);
                    if (g_macro_listening_static) SetWindowTextW(g_macro_listening_static, L"");
                } else {
                    std::string key = vk_to_key_name((UINT)wParam);
                    if (!key.empty()) {
                        if (ValidateMacroHotkeyForEntry(key, idx, hWnd)) {
                            g_macro_entries[idx].hotkey = key;
                            RebuildMacroHotkeyState();
                            SaveMacroEntriesToDisk(hWnd);
                            if (g_macro_listening_static) SetWindowTextW(g_macro_listening_static, widen(key).c_str());
                        } else if (g_macro_listening_static) {
                            SetWindowTextW(g_macro_listening_static, widen(g_macro_entries[idx].hotkey).c_str());
                        }
                    }
                }
            }
            g_macro_listening_idx = -1;
            g_macro_listening_static = nullptr;
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_macro_editor_recording) {
            MacroRecordStop();
            g_macro_editor_recording = false;
            KillTimer(hWnd, MACRO_RECORD_TIMER_ID);
        }
        DestroyWindow(hWnd);
        break;
    case WM_DESTROY:
        if (GetCapture() == hWnd) ReleaseCapture();
        g_macro_listening_idx = -1;
        g_macro_listening_static = nullptr;
        break;
    default: return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}
static void ShowMacroEditor(HWND parent) {
    HWND h = CreateWindowW(L"NSMacroEditor", L"Macros", WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 620, 280, parent, nullptr, g_hInst, nullptr);
    if (h) ShowWindow(h, SW_SHOW);
}

// ── Control IDs ──

// ── Create a modern button with theme support ──
static HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id) {
    HWND hw = CreateWindow(L"BUTTON", text, WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    HFONT hBtnFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessage(hw, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
    return hw;
}

// ── Keyboard polling helpers for Windows ──
static bool key_is_down(const std::string& name) {
    if (name.size() == 1) {
        char c = name[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return GetAsyncKeyState(c) & 0x8000;
    }
    struct { const char* n; int vk; } kmap[] = {
        {"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},
        {"LSHIFT",VK_LSHIFT},{"RSHIFT",VK_RSHIFT},
        {"LCTRL",VK_LCONTROL},{"RCTRL",VK_RCONTROL},
        {"LALT",VK_LMENU},{"RALT",VK_RMENU},
        {"SPACE",VK_SPACE},{"ENTER",VK_RETURN},{"TAB",VK_TAB},
        {"ESC",VK_ESCAPE},{"BACKSPACE",VK_BACK},
        {"HOME",VK_HOME},{"SNAPSHOT",VK_SNAPSHOT},
        {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},
        {"F5",VK_F5},{"F6",VK_F6},{"F7",VK_F7},{"F8",VK_F8},
        {"F9",VK_F9},{"F10",VK_F10},{"F11",VK_F11},{"F12",VK_F12},
    };
    for (auto& km : kmap)
        if (name == km.n) return GetAsyncKeyState(km.vk) & 0x8000;
    return false;
}

static void apply_keyboard_to_report(ns::HIDReport& rep, bool override_mode) {
    auto get = [](const std::string& btn) -> std::string {
        auto it = g_keyBindings.find(btn);
        return it != g_keyBindings.end() ? it->second : "";
    };
    std::string k;
    k = get("Y");      if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_Y;
    k = get("B");      if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_B;
    k = get("A");      if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_A;
    k = get("X");      if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_X;
    k = get("L");      if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_L;
    k = get("R");      if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_R;
    k = get("ZL");     if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_ZL;
    k = get("ZR");     if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_ZR;
    k = get("MINUS");  if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_MINUS;
    k = get("PLUS");   if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_PLUS;
    k = get("LSTICK"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_LSTICK;
    k = get("RSTICK"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_RSTICK;
    k = get("HOME");   if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_HOME;
    k = get("CAPTURE"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_CAPTURE;
    bool up=false,down=false,left=false,right=false;
    k = get("DPAD_UP");    if (!k.empty()) up    = key_is_down(k);
    k = get("DPAD_DOWN");  if (!k.empty()) down  = key_is_down(k);
    k = get("DPAD_LEFT");  if (!k.empty()) left  = key_is_down(k);
    k = get("DPAD_RIGHT"); if (!k.empty()) right = key_is_down(k);
    if (up && right) rep.hat = ns::HAT_NE;
    else if (up && left) rep.hat = ns::HAT_NW;
    else if (down && right) rep.hat = ns::HAT_SE;
    else if (down && left) rep.hat = ns::HAT_SW;
    else if (up) rep.hat = ns::HAT_N;
    else if (down) rep.hat = ns::HAT_S;
    else if (left) rep.hat = ns::HAT_W;
    else if (right) rep.hat = ns::HAT_E;

    // Left stick axes
    auto lsu = get("LSTICK_UP"), lsd = get("LSTICK_DOWN");
    auto lsl = get("LSTICK_LEFT"), lsr = get("LSTICK_RIGHT");
    bool lsu_dn = !lsu.empty() && key_is_down(lsu);
    bool lsd_dn = !lsd.empty() && key_is_down(lsd);
    bool lsl_dn = !lsl.empty() && key_is_down(lsl);
    bool lsr_dn = !lsr.empty() && key_is_down(lsr);
    if (lsl_dn && !lsr_dn) rep.lx = 0;
    else if (lsr_dn && !lsl_dn) rep.lx = 255;
    else if (!override_mode) rep.lx = 128;
    if (lsu_dn && !lsd_dn) rep.ly = 0;
    else if (lsd_dn && !lsu_dn) rep.ly = 255;
    else if (!override_mode) rep.ly = 128;

    // Right stick axes
    auto rsu = get("RSTICK_UP"), rsd = get("RSTICK_DOWN");
    auto rsl = get("RSTICK_LEFT"), rsr = get("RSTICK_RIGHT");
    bool rsu_dn = !rsu.empty() && key_is_down(rsu);
    bool rsd_dn = !rsd.empty() && key_is_down(rsd);
    bool rsl_dn = !rsl.empty() && key_is_down(rsl);
    bool rsr_dn = !rsr.empty() && key_is_down(rsr);
    if (rsl_dn && !rsr_dn) rep.rx = 0;
    else if (rsr_dn && !rsl_dn) rep.rx = 255;
    else if (!override_mode) rep.rx = 128;
    if (rsu_dn && !rsd_dn) rep.ry = 0;
    else if (rsd_dn && !rsu_dn) rep.ry = 255;
    else if (!override_mode) rep.ry = 128;
}

static int detect_server_udp_interval_ms(SOCKET sock, const sockaddr_in& dest, int fallback_ms, bool* out_is_hori) {
    if (out_is_hori) *out_is_hori = false;
    ns::ServerInfoProbe probe{};
    sendto(sock, reinterpret_cast<const char*>(&probe), (int)sizeof(probe), 0,
           reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));

    const uint64_t deadline = ns::now_us() + 150000ULL;
    while (ns::now_us() < deadline) {
        ns::ServerInfoReply reply{};
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(&reply), (int)sizeof(reply), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n == (int)sizeof(reply) &&
            reply.magic == ns::SERVER_INFO_MAGIC &&
            reply.version == ns::SERVER_INFO_VERSION &&
            reply.udp_interval_ms > 0) {
            if (out_is_hori) *out_is_hori = reply.backend == ns::SERVER_BACKEND_HORI;
            return reply.udp_interval_ms;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return fallback_ms;
}

// ── Sender thread (4-Player) ──
static void SenderThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // SDL3 is initialized once on the main thread at app startup.
    // The sender thread only polls the already-initialized backend.

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        return;
    }

    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char portBuf[8];
    snprintf(portBuf, sizeof(portBuf), "%u", g_targetPort);
    if (getaddrinfo(g_targetHost.c_str(), portBuf, &hints, &res) != 0 || !res) {
        closesocket(sock);
        return;
    }
    sockaddr_in dest{};
    memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    bool serverIsHori = false;
    const int activeSendIntervalMs = detect_server_udp_interval_ms(sock, dest, ns::PRO_UDP_INTERVAL_MS, &serverIsHori);
    const bool sendMotion = !serverIsHori;

    g_sock = sock;
    g_senderRunning = true;
    uint32_t seq = 0;
    RumbleManager rumble;
    DigitalReleaseFilter sdlFilters[4];

    while (g_senderRunning) {
        {
            std::string upload;
            { std::lock_guard<std::mutex> lk(g_macro_mtx); upload.swap(g_macro_upload_pending); }
            if (!upload.empty()) send_macro_udp_packet(sock, dest, g_hmacKey, upload, 0);
        }

        PollMacroEntryHotkeys();
        g_sdlInput.poll();

        ns::HIDReport logicalReports[4];
        ns::MotionReport logicalMotion[4];
        bool present[4] = {false, false, false, false};
        bool hasMotion[4] = {false, false, false, false};
        int sdlForSlot[4] = {-1, -1, -1, -1};
        for (int i = 0; i < 4; ++i) {
            logicalReports[i].reset();
            logicalMotion[i].reset();
        }

        int activeCount = 0;

        auto sdl = g_sdlInput.snapshot();
        const uint64_t filterNow = ns::now_us();
        for (int i = 0; i < 4; ++i) {
            if (!sdl[i].connected) {
                sdlFilters[i].reset();
                continue;
            }
            logicalReports[i] = sdl[i].input;
            sdlFilters[i].apply(logicalReports[i], filterNow);
            present[i] = true;
            sdlForSlot[i] = i;
            if (sendMotion && sdl[i].has_motion) {
                logicalMotion[i] = sdl[i].motion;
                hasMotion[i] = true;
            }
            activeCount++;
        }

        // Keyboard modes are kept exactly as before; physical pads come only from SDL3.
        int km = g_keyboardMode.load();
        if (km == KB_SINGLE) {
            if (present[0]) {
                int target = -1;
                for (int s = 1; s < 4; ++s) {
                    if (!present[s]) { target = s; break; }
                }
                if (target >= 0) {
                    logicalReports[target] = logicalReports[0];
                    logicalMotion[target] = logicalMotion[0];
                    hasMotion[target] = hasMotion[0];
                    present[target] = true;
                    sdlForSlot[target] = sdlForSlot[0];
                    activeCount++;
                }
            }
            logicalReports[0].reset();
            logicalMotion[0].reset();
            apply_keyboard_to_report(logicalReports[0], false);
            present[0] = true;
            hasMotion[0] = false;
            sdlForSlot[0] = -1;
            activeCount = std::max(activeCount, 1);
        } else if (km == KB_OVERRIDE) {
            apply_keyboard_to_report(logicalReports[0], true);
            present[0] = true;
            activeCount = std::max(activeCount, 1);
        }

        MacroRecordSample(logicalReports[0]);
        if (ApplyMacroOverride(logicalReports, present, hasMotion)) activeCount = 1;

        ExtendedUdpPacket pkt{};
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = ns::PROTO_MAGIC;
        pkt.version = ns::WEB_PROTO_VERSION;
        pkt.flags = ns::FLAG_NONE;
        pkt.seq = seq++;
        pkt.timestamp_us = ns::now_us();
        pkt.report.reset();
        ns::ExtendedHIDReport* pads[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
        for (int i = 0; i < 4; ++i) {
            fill_extended_pad(*pads[i], logicalReports[i], present[i], hasMotion[i] ? &logicalMotion[i] : nullptr);
        }

        {
            uint8_t fullHmac[32];
            hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, EXT_UDP_PACKET_AUTH_SIZE, fullHmac);
            memcpy(pkt.hmac, fullHmac, ns::HMAC_TAG_SIZE);
        }

        sendto(sock, (const char*)&pkt, (int)sizeof(pkt), 0, (const sockaddr*)&dest, sizeof(dest));
        g_packetCount++;

        pump_udp_rumble(sock, rumble, sdlForSlot);
        rumble.update_timeouts(sdlForSlot);

        if (activeCount > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(activeSendIntervalMs));
        }
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    rumble.stop_all();
    closesocket(sock);
    g_sock = INVALID_SOCKET;
}

// ── Update P1-P4 status display ──
static void UpdateControllerStatus() {
    wchar_t buf[160];
    HWND hText[4] = { g_hP1Text, g_hP2Text, g_hP3Text, g_hP4Text };
    int km = g_keyboardMode.load();
    auto sdl = g_sdlInput.snapshot();
    std::string sdlErr = g_sdlInput.error();

    for (DWORD i = 0; i < 4; i++) {
        if (i == 0 && km != KB_OFF) {
            if (km == KB_SINGLE) {
                swprintf(buf, 160, L"P1: Keyboard");
            } else {
                if (sdl[0].connected)
                    swprintf(buf, 160, L"P1: SDL3 Controller / Keyboard");
                else
                    swprintf(buf, 160, L"P1: Idle / Keyboard");
            }
            SetWindowText(hText[i], buf);
            continue;
        }

        if (sdl[i].connected) {
            std::wstring name = widen(sdl[i].name.empty() ? "SDL3 Gamepad" : sdl[i].name);
            const wchar_t* motion = sdl[i].has_motion ? L" + gyro" : L"";
            swprintf(buf, 160, L"P%d: %ls%ls", i + 1, name.c_str(), motion);
        } else if (!sdlErr.empty() && i == 0) {
            swprintf(buf, 160, L"P1: SDL3 error");
        } else {
            swprintf(buf, 160, L"P%d: Not connected", i + 1);
        }
        SetWindowText(hText[i], buf);
    }
}
// ── Connect ──
static void DoConnect(HWND hWnd) {
    if (g_connected) return;

    wchar_t ipBuf[64]{};
    GetWindowText(g_hIpEdit, ipBuf, 64);

    if (ipBuf[0] == 0) {
        MessageBox(hWnd, L"Please enter a Raspberry Pi IP address.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    int port = ns::DEFAULT_PORT;
    wchar_t* colon = wcschr(ipBuf, L':');
    if (colon) {
        *colon = L'\0';
        port = (int)_wtoi(colon + 1);
        if (port <= 0 || port > 65535) port = ns::DEFAULT_PORT;
    }

    derive_key(ns::DEFAULT_SECRET, g_hmacKey);

    SaveLastIP(ipBuf);

    char hostA[64]{};
    WideCharToMultiByte(CP_UTF8, 0, ipBuf, -1, hostA, sizeof(hostA), nullptr, nullptr);
    g_targetHost = hostA;
    g_targetPort = (uint16_t)port;
    g_packetCount = 0;
    LoadMacroEntries();

    g_connected = true;

    if (g_senderThread.joinable()) g_senderThread.join();
    g_senderThread = std::thread(SenderThread);

    SetWindowText(g_hConnectBtn, L"Disconnect");
    EnableWindow(g_hIpEdit, FALSE);
    EnableWindow(g_hKeyboardCombo, FALSE);
    EnableWindow(g_hBindingsBtn, FALSE);
    EnableWindow(g_hMacrosBtn, TRUE);

    std::wstring status = L"Connected to " + std::wstring(ipBuf) + L":" + std::to_wstring(port);
    SetWindowText(g_hStatusText, status.c_str());
}

// ── Disconnect ──
static void DoDisconnect() {
    if (!g_connected) return;
    g_connected = false;
    g_senderRunning = false;
    if (g_senderThread.joinable()) g_senderThread.join();

    SetWindowText(g_hConnectBtn, L"Connect");
    EnableWindow(g_hIpEdit, TRUE);
    EnableWindow(g_hKeyboardCombo, TRUE);
    if (g_keyboardMode.load() != KB_OFF) EnableWindow(g_hBindingsBtn, TRUE);
    EnableWindow(g_hMacrosBtn, TRUE);
    SetWindowText(g_hStatusText, L"Disconnected");
    SetWindowText(g_hP1Text, L"");
    SetWindowText(g_hP2Text, L"");
    SetWindowText(g_hP3Text, L"");
    SetWindowText(g_hP4Text, L"");
    UpdateControllerStatus();
}

// ── Theme colors (white background) ──
static const COLORREF ACCENT_RED  = RGB(0xCC, 0x00, 0x00);
static const COLORREF TEXT_BLACK  = RGB(0x1A, 0x1A, 0x1A);
static const COLORREF GRAY_LINE   = RGB(0xDD, 0xDD, 0xDD);

// ── Window procedure ──
// Keyboard bindings editor globals
static std::vector<std::pair<std::string, int>> g_bindingKeys = {
    {"A", 'V'}, {"B", 'X'}, {"X", 'C'}, {"Y", 'Z'},
    {"L", 'Q'}, {"R", 'E'},
    {"ZL", '1'}, {"ZR", '2'},
    {"MINUS", '3'}, {"PLUS", '4'},
    {"LSTICK", VK_LSHIFT}, {"RSTICK", VK_RSHIFT},
    {"HOME", VK_HOME},
    {"LSTICK_UP", 'W'}, {"LSTICK_DOWN", 'S'},
    {"LSTICK_LEFT", 'A'}, {"LSTICK_RIGHT", 'D'},
    {"RSTICK_UP", 'I'}, {"RSTICK_DOWN", 'K'},
    {"RSTICK_LEFT", 'J'}, {"RSTICK_RIGHT", 'L'},
    {"DPAD_UP", VK_UP}, {"DPAD_DOWN", VK_DOWN},
    {"DPAD_LEFT", VK_LEFT}, {"DPAD_RIGHT", VK_RIGHT},
    {"CAPTURE", VK_SNAPSHOT},
};
static std::unordered_map<std::string, std::string> g_editBindings;
static int g_listeningIdx = -1;
static HWND g_listeningStatic = nullptr;
static bool g_setupMode = false;
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Enable visual styles
            INITCOMMONCONTROLSEX icc{ sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES };
            InitCommonControlsEx(&icc);

            int x = 16, y = 12;

            // ── Title ──
            HFONT hTitleFont = CreateFont(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HWND hTitle = CreateWindow(L"STATIC", L"NS PC Control",
                WS_VISIBLE | WS_CHILD, x, y, 380, 30, hWnd, nullptr, g_hInst, nullptr);
            SendMessage(hTitle, WM_SETFONT, (WPARAM)hTitleFont, TRUE);
            y += 40;

            HFONT hLabelFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT hFieldFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");

            auto makeLabel = [&](const wchar_t* text, int x, int y, int w, int h) {
                HWND hw = CreateWindow(L"STATIC", text, WS_VISIBLE | WS_CHILD | SS_RIGHT,
                    x, y, w, h, hWnd, nullptr, g_hInst, nullptr);
                SendMessage(hw, WM_SETFONT, (WPARAM)hLabelFont, TRUE);
                return hw;
            };
            auto makeEdit = [&](int id, int x, int y, int w, int h, const wchar_t* def) {
                HWND hw = CreateWindow(L"EDIT", def, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
                    x, y, w, h, hWnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
                SendMessage(hw, WM_SETFONT, (WPARAM)hFieldFont, TRUE);
                return hw;
            };
            // ── IP row ──
            makeLabel(L"Raspberry Pi IP:", x, y + 4, 110, 22);
            std::wstring savedIp = LoadSavedIP();
            if (savedIp.empty()) savedIp = L"192.168.1.100";
            g_hIpEdit = makeEdit(IDC_IP, x + 115, y, 265, 24, savedIp.c_str());
            y += 36;

            // ── Keyboard Mode row ──
            HWND hKbLabel = CreateWindow(L"STATIC", L"Keyboard Mode:",
                WS_VISIBLE | WS_CHILD | SS_RIGHT,
                x, y + 4, 110, 22, hWnd, nullptr, g_hInst, nullptr);
            SendMessage(hKbLabel, WM_SETFONT, (WPARAM)hLabelFont, TRUE);

            g_hKeyboardCombo = CreateWindow(L"COMBOBOX", nullptr,
                WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_TABSTOP,
                x + 115, y, 155, 200, hWnd, (HMENU)(INT_PTR)IDC_KEYBOARD_COMBO, g_hInst, nullptr);
            SendMessage(g_hKeyboardCombo, WM_SETFONT, (WPARAM)hLabelFont, TRUE);
            SendMessage(g_hKeyboardCombo, CB_ADDSTRING, 0, (LPARAM)L"OFF");
            SendMessage(g_hKeyboardCombo, CB_ADDSTRING, 0, (LPARAM)L"ON (single)");
            SendMessage(g_hKeyboardCombo, CB_ADDSTRING, 0, (LPARAM)L"ON (override)");
            int savedMode = LoadSavedKeyboardMode();
            g_keyboardMode = savedMode;
            SendMessage(g_hKeyboardCombo, CB_SETCURSEL, savedMode, 0);

            g_hBindingsBtn = CreateButton(hWnd, L"Bindings...", x + 285, y, 80, 24, IDC_BINDINGS_BTN);
            EnableWindow(g_hBindingsBtn, savedMode != KB_OFF);
            y += 32;
            g_hMacrosBtn = CreateButton(hWnd, L"Macros...", x + 115, y, 120, 24, IDC_MACROS_BTN);
            y += 32;

            // ── Connect / Quit buttons ──
            g_hConnectBtn = CreateButton(hWnd, L"Connect", x + 115, y, 100, 30, IDC_CONNECT);
            CreateButton(hWnd, L"Quit", x + 285, y, 100, 30, 1002);
            y += 42;

            // ── Separator ──
            CreateWindow(L"STATIC", nullptr, WS_VISIBLE | WS_CHILD | SS_OWNERDRAW,
                x, y, 370, 2, hWnd, (HMENU)1003, g_hInst, nullptr);
            y += 14;

            // ── Status ──
            HFONT hStatusFont = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            auto makeStatus = [&](const wchar_t* text, int y) {
                HWND hw = CreateWindow(L"STATIC", text, WS_VISIBLE | WS_CHILD,
                    x + 4, y, 360, 20, hWnd, nullptr, g_hInst, nullptr);
                SendMessage(hw, WM_SETFONT, (WPARAM)hStatusFont, TRUE);
                return hw;
            };

            g_hStatusText = makeStatus(L"Ready", y);
            y += 22;

            g_hP1Text = makeStatus(L"", y); y += 18;
            g_hP2Text = makeStatus(L"", y); y += 18;
            g_hP3Text = makeStatus(L"", y); y += 18;
            g_hP4Text = makeStatus(L"", y);

            UpdateControllerStatus();
            EnableWindow(g_hConnectBtn, TRUE);
            break;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_CONNECT) {
                if (!g_connected) DoConnect(hWnd);
                else DoDisconnect();
            } else if (id == 1002) {
                PostMessage(hWnd, WM_CLOSE, 0, 0);
            } else if (id == IDC_KEYBOARD_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = (int)SendMessage(g_hKeyboardCombo, CB_GETCURSEL, 0, 0);
                if (sel == CB_ERR) sel = 0;
                g_keyboardMode = sel;
                EnableWindow(g_hBindingsBtn, sel != KB_OFF);
                SaveKeyboardMode(sel);
            } else if (id == IDC_MACROS_BTN) {
                ShowMacroEditor(hWnd);
            } else if (id == IDC_BINDINGS_BTN) {
                g_editBindings = g_keyBindings;
                g_listeningIdx = -1;
                HWND hDlg = CreateWindow(L"NSBindingEditor", L"Edit Key Bindings",
                    WS_CAPTION | WS_SYSMENU | WS_POPUP,
                    CW_USEDEFAULT, CW_USEDEFAULT, 620, 480,
                    hWnd, nullptr, g_hInst, nullptr);
                if (hDlg) {
                    ShowWindow(hDlg, SW_SHOW);
                }
            }
            break;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
            if (dis->CtlID == 1003) {
                HBRUSH hLine = CreateSolidBrush(GRAY_LINE);
                FillRect(dis->hDC, &dis->rcItem, hLine);
                DeleteObject(hLine);
                return TRUE;
            }
            break;
        }

        case WM_TIMER: {
            if (g_connected) UpdateControllerStatus();
            break;
        }

        case WM_DEVICECHANGE: {
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
                g_sdlInput.request_rescan();
                UpdateControllerStatus();
            }
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            HWND hCtrl = (HWND)lParam;
            SetTextColor(hdc, TEXT_BLACK);
            SetBkMode(hdc, TRANSPARENT);
            // Title label gets red accent
            wchar_t buf[64];
            GetWindowText(hCtrl, buf, 64);
            if (wcscmp(buf, L"NS PC Control") == 0) {
                SetTextColor(hdc, ACCENT_RED);
            }
            static HBRUSH hWhiteBrush = []{ return CreateSolidBrush(RGB(255,255,255)); }();
            return (LRESULT)hWhiteBrush;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, TEXT_BLACK);
            SetBkColor(hdc, RGB(255,255,255));
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }

        case WM_CLOSE:
            DoDisconnect();
            DestroyWindow(hWnd);
            break;

        case WM_DESTROY:
            DoDisconnect();
            g_sdlInput.stop();
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

static const int EDITOR_ROW_H = 26;
static const int EDITOR_X = 14;
static const int EDITOR_X2 = 300;       // Shifted right column to make room
static const int EDITOR_LABEL_W = 100;  // Widened the text labels so nothing is cut off
static const int EDITOR_KEY_W = 110;
static const int EDITOR_BTN_W = 54;
static const int EDITOR_GAP = 4;

static LRESULT CALLBACK BindingsEditorProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            int y = 12;
            int xLeftKey = EDITOR_X + EDITOR_LABEL_W + EDITOR_GAP;
            int xLeftChg = xLeftKey + EDITOR_KEY_W + EDITOR_GAP;
            int xRightLabel = EDITOR_X2;
            int xRightKey = xRightLabel + EDITOR_LABEL_W + EDITOR_GAP;
            int xRightChg = xRightKey + EDITOR_KEY_W + EDITOR_GAP;
            HFONT hFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
            int half = (int)g_bindingKeys.size() / 2;
            for (int i = 0; i < half; i++) {
                int li = i, ri = i + half;
                // Left column
                HWND hLLabel = CreateWindow(L"STATIC", widen(g_bindingKeys[li].first).c_str(),
                    WS_VISIBLE | WS_CHILD | SS_CENTER, EDITOR_X, y, EDITOR_LABEL_W, 22,
                    hDlg, nullptr, g_hInst, nullptr);
                SendMessage(hLLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
                std::wstring lk = widen(g_editBindings[g_bindingKeys[li].first]);
                HWND hLKey = CreateWindow(L"STATIC", lk.c_str(),
                    WS_VISIBLE | WS_CHILD | SS_CENTER | WS_BORDER,
                    xLeftKey, y, EDITOR_KEY_W, 22, hDlg, (HMENU)(INT_PTR)(IDC_EDITOR_KEY_START + li), g_hInst, nullptr);
                SendMessage(hLKey, WM_SETFONT, (WPARAM)hFont, TRUE);
                HWND hLBtn = CreateWindow(L"BUTTON", L"Change",
                    WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                    xLeftChg, y, EDITOR_BTN_W, 24, hDlg, (HMENU)(INT_PTR)(IDC_EDITOR_CHANGE + li), g_hInst, nullptr);
                SendMessage(hLBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
                // Right column
                HWND hRLabel = CreateWindow(L"STATIC", widen(g_bindingKeys[ri].first).c_str(),
                    WS_VISIBLE | WS_CHILD | SS_CENTER, xRightLabel, y, EDITOR_LABEL_W, 22,
                    hDlg, nullptr, g_hInst, nullptr);
                SendMessage(hRLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
                std::wstring rk = widen(g_editBindings[g_bindingKeys[ri].first]);
                HWND hRKey = CreateWindow(L"STATIC", rk.c_str(),
                    WS_VISIBLE | WS_CHILD | SS_CENTER | WS_BORDER,
                    xRightKey, y, EDITOR_KEY_W, 22, hDlg, (HMENU)(INT_PTR)(IDC_EDITOR_KEY_START + ri), g_hInst, nullptr);
                SendMessage(hRKey, WM_SETFONT, (WPARAM)hFont, TRUE);
                HWND hRBtn = CreateWindow(L"BUTTON", L"Change",
                    WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                    xRightChg, y, EDITOR_BTN_W, 24, hDlg, (HMENU)(INT_PTR)(IDC_EDITOR_CHANGE + ri), g_hInst, nullptr);
                SendMessage(hRBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
                y += EDITOR_ROW_H;
            }

            y += 8;
            int btnW = 74;
            int leftBtnX = EDITOR_X;
            int rightBtnX = xRightChg + EDITOR_BTN_W - btnW;
            HFONT hBtnFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
            // Left column: Save (above) Cancel (below)
            HWND hSave = CreateWindow(L"BUTTON", L"Save",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                leftBtnX, y, btnW, 30, hDlg, (HMENU)1001, g_hInst, nullptr);
            SendMessage(hSave, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
            HWND hSetup = CreateWindow(L"BUTTON", L"Setup",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                rightBtnX, y, btnW, 30, hDlg, (HMENU)IDC_EDITOR_SETUP, g_hInst, nullptr);
            SendMessage(hSetup, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
            // Row 2
            y += 38;
            HWND hCancel = CreateWindow(L"BUTTON", L"Cancel",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                leftBtnX, y, btnW, 30, hDlg, (HMENU)1000, g_hInst, nullptr);
            SendMessage(hCancel, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
            HWND hClear = CreateWindow(L"BUTTON", L"Clear",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                rightBtnX - btnW - 8, y, btnW, 30, hDlg, (HMENU)IDC_EDITOR_CLEAR, g_hInst, nullptr);
            SendMessage(hClear, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
            HWND hReset = CreateWindow(L"BUTTON", L"Reset",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                rightBtnX, y, btnW, 30, hDlg, (HMENU)IDC_EDITOR_RESET, g_hInst, nullptr);
            SendMessage(hReset, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
            break;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            
            auto updateKeyDisplays = [&]() {
                for (int i = 0; i < (int)g_bindingKeys.size(); i++) {
                    const std::string& val = g_editBindings[g_bindingKeys[i].first];
                    SetDlgItemText(hDlg, IDC_EDITOR_KEY_START + i, val.empty() ? L"" : widen(val).c_str());
                }
            };

            if (id == 1000) { // Cancel
                DestroyWindow(hDlg);
            } else if (id == 1001) { // Save
                g_keyBindings = g_editBindings;
                SaveBindings();
                DestroyWindow(hDlg);
            } else if (id == IDC_EDITOR_SETUP) { // Setup
                g_setupMode = true;
                for (size_t i = 0; i < g_bindingKeys.size(); i++)
                    g_editBindings[g_bindingKeys[i].first] = "";
                updateKeyDisplays();
                g_listeningIdx = 0;
                g_listeningStatic = GetDlgItem(hDlg, IDC_EDITOR_KEY_START);
                if (g_listeningStatic) SetWindowText(g_listeningStatic, L"...");
                SetFocus(hDlg);
            } else if (id == IDC_EDITOR_CLEAR) { // Clear keyboard binds only; macro hotkeys are stored separately.
                g_setupMode = false;
                g_listeningIdx = -1;
                g_listeningStatic = nullptr;
                for (const auto& kv : g_bindingKeys)
                    g_editBindings[kv.first].clear();
                updateKeyDisplays();
                SetFocus(hDlg);
            } else if (id == IDC_EDITOR_RESET) { // Reset
                auto defs = default_key_bindings();
                for (auto& [k, _] : g_bindingKeys)
                    g_editBindings[k] = defs[k];
                updateKeyDisplays();
            } else if (id >= IDC_EDITOR_CHANGE && id < IDC_EDITOR_CHANGE + (int)g_bindingKeys.size()) {
                g_setupMode = false;
                int idx = id - IDC_EDITOR_CHANGE;
                if (idx >= 0 && idx < (int)g_bindingKeys.size()) {
                    g_listeningIdx = idx;
                    g_listeningStatic = GetDlgItem(hDlg, IDC_EDITOR_KEY_START + idx);
                    if (g_listeningStatic) SetWindowText(g_listeningStatic, L"...");
                    SetFocus(hDlg);
                }
            }
            break;
        }

        case WM_KEYDOWN: {
            if (g_listeningIdx >= 0) {
                auto updateKeyDisplays = [&]() {
                    for (int i = 0; i < (int)g_bindingKeys.size(); i++) {
                        const std::string& val = g_editBindings[g_bindingKeys[i].first];
                        SetDlgItemText(hDlg, IDC_EDITOR_KEY_START + i, val.empty() ? L"" : widen(val).c_str());
                    }
                };

                UINT vk = (UINT)wParam;
                if (vk == VK_ESCAPE) {
                    g_editBindings[g_bindingKeys[g_listeningIdx].first] = "";
                    if (g_listeningStatic) SetWindowText(g_listeningStatic, L"");
                    
                    if (g_setupMode) {
                        g_listeningIdx++;
                        if (g_listeningIdx < (int)g_bindingKeys.size()) {
                            g_listeningStatic = GetDlgItem(hDlg, IDC_EDITOR_KEY_START + g_listeningIdx);
                            if (g_listeningStatic) SetWindowText(g_listeningStatic, L"...");
                            return 0;
                        }
                    }
                    g_setupMode = false;
                    g_listeningIdx = -1;
                    g_listeningStatic = nullptr;
                    return 0;
                }

                // Map VK code to key name
                auto vk_to_name = [](UINT vk) -> std::string {
                    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
                    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
                    struct { UINT vk; const char* n; } map[] = {
                        {VK_UP,"UP"},{VK_DOWN,"DOWN"},{VK_LEFT,"LEFT"},{VK_RIGHT,"RIGHT"},
                        {VK_LSHIFT,"LSHIFT"},{VK_RSHIFT,"RSHIFT"},
                        {VK_LCONTROL,"LCTRL"},{VK_RCONTROL,"RCTRL"},
                        {VK_LMENU,"LALT"},{VK_RMENU,"RALT"},
                        {VK_SPACE,"SPACE"},{VK_RETURN,"ENTER"},{VK_TAB,"TAB"},
                        {VK_ESCAPE,"ESC"},{VK_BACK,"BACKSPACE"},
        {VK_F1,"F1"},{VK_F2,"F2"},{VK_F3,"F3"},{VK_F4,"F4"},
        {VK_F5,"F5"},{VK_F6,"F6"},{VK_F7,"F7"},{VK_F8,"F8"},
        {VK_F9,"F9"},{VK_F10,"F10"},{VK_F11,"F11"},{VK_F12,"F12"},
        {VK_HOME,"HOME"},{VK_SNAPSHOT,"SNAPSHOT"},
                    };
                    for (auto& m : map)
                        if (vk == m.vk) return m.n;
                    return "";
                };

                std::string name = vk_to_name((UINT)wParam);
                if (!name.empty()) {
                    bool alreadyBound = false;
                    for (auto& [k, v] : g_editBindings) {
                        if (k != g_bindingKeys[g_listeningIdx].first && v == name) { alreadyBound = true; break; }
                    }
                    if (alreadyBound && g_setupMode) { return 0; }
                    
                    for (auto& [k, v] : g_editBindings) {
                        if (v == name) { v = ""; break; }
                    }
                    
                    g_editBindings[g_bindingKeys[g_listeningIdx].first] = name;
                    updateKeyDisplays();

                    if (g_setupMode) {
                        g_listeningIdx++;
                        if (g_listeningIdx < (int)g_bindingKeys.size()) {
                            g_listeningStatic = GetDlgItem(hDlg, IDC_EDITOR_KEY_START + g_listeningIdx);
                            if (g_listeningStatic) SetWindowText(g_listeningStatic, L"...");
                            return 0;
                        }
                    }
                }
                
                if (!g_setupMode || g_listeningIdx >= (int)g_bindingKeys.size()) {
                    g_setupMode = false;
                    g_listeningIdx = -1;
                    g_listeningStatic = nullptr;
                }
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hDlg);
            break;

        case WM_DESTROY:
            if (GetCapture() == hDlg) ReleaseCapture();
            g_setupMode = false;
            g_listeningIdx = -1;
            g_listeningStatic = nullptr;
            break;
    }
    return DefWindowProc(hDlg, msg, wParam, lParam);
}

// ── Entry point ──
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SDL_SetMainReady();
    g_sdlInput.start();
    g_hInst = hInst;

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    // SDL3 owns all native controller discovery/input.

    // Load icon from embedded resource (ID 1 from ns-gui.rc)
    HICON hIcon =     LoadIcon(hInst, MAKEINTRESOURCE(1));

    LoadSavedBindings();

    const wchar_t CLASS_NAME[] = L"NSGamepadWindow";
    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hIcon = hIcon;
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);

    // Register bindings editor class
    WNDCLASS ec{};
    ec.lpfnWndProc = BindingsEditorProc;
    ec.hInstance = hInst;
    ec.hCursor = LoadCursor(nullptr, IDC_ARROW);
    ec.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    ec.lpszClassName = L"NSBindingEditor";
    RegisterClass(&ec);

    WNDCLASS mc{};
    mc.lpfnWndProc = MacroEditorProc;
    mc.hInstance = hInst;
    mc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    mc.lpszClassName = L"NSMacroEditor";
    RegisterClass(&mc);

    RECT rc{0, 0, 410, 345};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);

    HWND hWnd = CreateWindowEx(0, CLASS_NAME, L"NS PC Control",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    if (!hWnd) return 1;

    g_hWnd = hWnd;
    // Set the icon for the title bar and taskbar
    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    SetTimer(hWnd, 1, 100, nullptr);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DoDisconnect();
    WSACleanup();
    timeEndPeriod(1); return 0;
}
