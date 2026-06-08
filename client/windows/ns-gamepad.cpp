#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef SDL_MAIN_HANDLED
#  define SDL_MAIN_HANDLED
#endif

#include <windows.h>
#include <mmsystem.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <iostream>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <array>
#include <mutex>
#include <atomic>
#include <cmath>
#include <fstream>
#include <sstream>
#include <cctype>
#include <stdexcept>
#include <limits>

#include "../../server/rpi/include/sha256.h"
#include "../../server/rpi/include/protocol.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")
#ifndef NS_LINK_SDL3_MANUALLY
#  ifdef NS_SDL3_STATIC
#    pragma comment(lib, "SDL3-static.lib")
#  else
#    pragma comment(lib, "SDL3.lib")
#  endif
#endif
#endif

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

        // Allow readable macro files with comments:
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

            // LOOP n repeats the block since the start or the previous LOOP,
            // n total times. The block is then closed and the next command
            // starts a new block.
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

[[maybe_unused]] static bool macro_validate_to_pretty_json(const std::string& raw_text, std::string& pretty, std::string& err, const std::string& fallback_name = "Macro") {
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

static int16_t read_le16(const uint8_t* p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void set_pad_present_flag(ns::ExtendedHIDReport& r, bool present) {
    // ExtendedHIDReport wire layout starts with the 7-byte HIDReport.
    // Byte +7 is the pad-present flag used by the backend/web protocol.
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
// One input path for the CLI client: buttons/sticks, rumble, and optional
// accelerometer/gyro when SDL3 exposes those sensors for the controller.
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

        // Must be set before SDL_Init. These make vendor HIDAPI paths
        // available, including enhanced reports where supported.
        SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_" "SW" "ITCH", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_" "JOY" "_CONS", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_XBOX", "1");
        SDL_SetHint("SDL_JOYSTICK_ENHANCED_REPORTS", "1");

        if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_SENSOR | SDL_INIT_HAPTIC | SDL_INIT_EVENTS)) {
            const char* e = SDL_GetError();
            last_error = (e && *e) ? e : "SDL_Init failed";
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

    static Uint16 motor_word(uint8_t v) {
        return (Uint16)((uint32_t)v * 65535u / 255u);
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

        // SDL face buttons are physical positions. This preserves console layout:
        // Xbox A/South -> console B, Xbox B/East -> console A, etc.
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

    static void apply_motion(SDL_Gamepad* pad, bool accel_enabled, bool gyro_enabled,
                             ns::MotionReport& out, bool& has_motion) {
        out.reset();
        has_motion = false;

        float accel[3] = {};
        if (accel_enabled && SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3)) {
            out.ax = clamp_motion_i16((accel[0] / 9.80665f) * 4096.0f);
            out.ay = clamp_motion_i16((accel[1] / 9.80665f) * 4096.0f);
            out.az = clamp_motion_i16((accel[2] / 9.80665f) * 4096.0f);
            has_motion = true;
        }

        float gyro[3] = {};
        if (gyro_enabled && SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gyro, 3)) {
            constexpr float RAD_TO_DEG = 57.29577951308232f;
            constexpr float CONSOLE_GYRO_SCALE = RAD_TO_DEG * 16.0f;
            out.gx = clamp_motion_i16(gyro[0] * CONSOLE_GYRO_SCALE);
            out.gy = clamp_motion_i16(gyro[1] * CONSOLE_GYRO_SCALE);
            out.gz = clamp_motion_i16(gyro[2] * CONSOLE_GYRO_SCALE);
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
                if (it->slot >= 0 && it->slot < 4) {
                    std::cout << "Controller P" << (it->slot + 1) << " disconnected: " << it->name << "\n";
                    states[it->slot] = SdlPadState{};
                }
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

            if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
                d.accel_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
            }
            if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
                d.gyro_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
            }

            std::cout << "Mapped SDL3 controller to local slot P" << (slot + 1)
                      << ": " << d.name;
            if (d.gyro_enabled || d.accel_enabled) std::cout << " + motion";
            if (d.rumble_capable) std::cout << " + rumble";
            if (d.trigger_rumble_capable) std::cout << " + trigger-rumble";
            std::cout << "\n";

            devices.push_back(d);
        }
        SDL_free(ids);
    }

    void refresh_states_locked(uint64_t now) {
        clear_states_locked();
        for (const auto& d : devices) {
            if (!d.pad || d.slot < 0 || d.slot >= 4 || !SDL_GamepadConnected(d.pad)) continue;
            SdlPadState st{};
            st.connected = true;
            st.input = map_gamepad(d);
            st.name = d.name;
            st.vid = d.vid;
            st.pid = d.pid;
            st.instance_id = d.id;
            apply_motion(d.pad, d.accel_enabled, d.gyro_enabled, st.motion, st.has_motion);
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
        // Normal-rumble build: treat PrecisionRumblePacket as a carrier for
        // already-decoded classic low/high magnitudes. Do not send Nintendo
        // HD/precision effect bytes through SDL.
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
        uint8_t low = rp.low_freq;
        uint8_t high = rp.high_freq;
        bool neutral = (low == 0 && high == 0) || rp.duration_10ms == 0;
        uint64_t now = ns::now_us();
        uint64_t dur_us = neutral ? 0ULL : std::max<uint64_t>(250000ULL, (uint64_t)rp.duration_10ms * 10000ULL);
        uint32_t dur_ms = neutral ? 0U : (uint32_t)std::min<uint64_t>(dur_us / 1000ULL, 0xFFFFFFFFULL);

        if (!neutral && now - states[slot].last_set_us < 30000ULL) {
            states[slot].until_us = now + dur_us;
            states[slot].duration_ms = dur_ms;
            return;
        }

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
            std::cerr << "UDP receive error: " << e << "\n";
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

static int keyboard_mode = 0; // 0=off, 1=single, 2=override

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
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string p(path);
        size_t pos = p.find_last_of("\\/");
        return (pos != std::string::npos ? p.substr(0, pos) : ".") + "\\bindings.json";
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

    // Poll keyboard and fill HIDReport for player 1
    void apply(ns::HIDReport& rep) const {
        // Helper: check if a named key is pressed
        auto is_down = [](const std::string& name) -> bool {
            // Letter keys
            if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
                return GetAsyncKeyState(name[0]) & 0x8000;
            if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
                return GetAsyncKeyState(name[0]) & 0x8000;
            // Named keys
            struct KeyMap { const char* n; int vk; };
            static const KeyMap kmap[] = {
                {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
                {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT},
                {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
                {"LALT", VK_LMENU}, {"RALT", VK_RMENU},
                {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"TAB", VK_TAB},
                {"ESC", VK_ESCAPE}, {"BACKSPACE", VK_BACK},
                {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
                {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
                {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
                {"HOME", VK_HOME}, {"SNAPSHOT", VK_SNAPSHOT},
            };
            for (auto& km : kmap)
                if (name == km.n) return GetAsyncKeyState(km.vk) & 0x8000;
            // Number key alternative: '0'..'9'  
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

        // D-Pad
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


int main(int argc, char** argv) {
    timeBeginPeriod(1);
    SDL_SetMainReady();

    auto cleanup_and_exit = [](int code, SOCKET sock = INVALID_SOCKET) -> int {
        if (sock != INVALID_SOCKET) closesocket(sock);
        g_sdlInput.stop();
        WSACleanup();
        timeEndPeriod(1);
        return code;
    };

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]] [--legacy] [--macro file.json]\n";
        std::cerr << "  -k          Enable keyboard mode (default: single)\n";
        std::cerr << "  --legacy    Send old input-only UDP packets; disables UDP rumble/gyro\n";
        std::cerr << "  --macro     Upload a P1 server-side macro JSON/string, wait for it, then exit\n";
        timeEndPeriod(1);
        return 1;
    }

    std::string host;
    int port = ns::DEFAULT_PORT;
    bool legacy_udp = false;
    bool macro_mode = false;
    std::string macro_path;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--legacy") == 0) {
            legacy_udp = true;
        } else if (strcmp(argv[i], "--macro") == 0 ||
                   strcmp(argv[i], "--upload-macro") == 0 ||
                   strcmp(argv[i], "--server-macro") == 0) {
            if (i + 1 >= argc) {
                std::cerr << argv[i] << " requires a macro JSON/commands file path\n";
                timeEndPeriod(1);
                return 1;
            }
            macro_mode = true;
            macro_path = argv[++i];
        } else if (strcmp(argv[i], "-k") == 0) {
            keyboard_mode = 1;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                if (strcmp(argv[i+1], "override") == 0) keyboard_mode = 2;
                else if (strcmp(argv[i+1], "single") == 0) keyboard_mode = 1;
                else {
                    std::cerr << "Unknown keyboard mode: " << argv[i+1] << "\n";
                    timeEndPeriod(1);
                    return 1;
                }
                i++;
            }
        } else if (host.empty()) {
            host = argv[i];
            size_t colon = host.find(':');
            if (colon != std::string::npos) {
                port = std::atoi(host.c_str() + colon + 1);
                if (port < 1 || port > 65535) {
                    std::cerr << "Invalid port: " << port << " (must be 1-65535)\n";
                    timeEndPeriod(1);
                    return 1;
                }
                host.resize(colon);
            }
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            timeEndPeriod(1);
            return 1;
        }
    }

    if (host.empty()) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]] [--legacy] [--macro file.json]\n";
        timeEndPeriod(1);
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
        std::cout << "Extended UDP mode: SDL3 rumble replies + gyro/motion enabled when the controller/API exposes them.\n";
    }

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "ERROR: WSAStartup failed\n";
        timeEndPeriod(1);
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "ERROR: socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        timeEndPeriod(1);
        return 1;
    }

    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    char port_buf[8];
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    if (getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0 || res == nullptr) {
        std::cerr << "ERROR: Unable to resolve IP: " << host << "\n";
        return cleanup_and_exit(1, sock);
    }

    sockaddr_in dest{};
    memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    if (macro_mode) {
        std::string macro_raw = macro_read_file(macro_path);
        if (macro_raw.empty()) {
            std::cerr << "Macro file is empty or cannot be read: " << macro_path << "\n";
            return cleanup_and_exit(1, sock);
        }
        auto macro_steps_for_wait = macro_parse_text(macro_raw);
        if (macro_steps_for_wait.empty()) {
            std::cerr << "Macro file has no usable commands: " << macro_path << "\n";
            return cleanup_and_exit(1, sock);
        }
        bool sent = send_macro_udp_packet(sock, dest, hmac_key, macro_raw, 0);
        std::cout << (sent ? "Uploaded server-side macro to P1.\n" : "Failed to upload server-side macro.\n");
        std::this_thread::sleep_for(std::chrono::milliseconds((int)macro_total_ms(macro_steps_for_wait) + 180));
        return cleanup_and_exit(sent ? 0 : 1, sock);
    }

    if (!g_sdlInput.start()) {
        std::cerr << "ERROR: SDL3 input failed: " << g_sdlInput.error() << "\n";
        return cleanup_and_exit(1, sock);
    }

    std::cout << "Started... Press Ctrl+C to stop\n";
    uint32_t seq = 0;
    RumbleManager rumble;
    DigitalReleaseFilter sdl_filters[4];
    static bool no_controllers_printed = false;

    while (true) {
        g_sdlInput.poll();

        ns::HIDReport logical_reports[4];
        ns::MotionReport logical_motion[4];
        bool present[4] = {false, false, false, false};
        bool has_motion[4] = {false, false, false, false};
        int sdl_for_slot[4] = {-1, -1, -1, -1};

        for (int i = 0; i < 4; ++i) {
            logical_reports[i].reset();
            logical_motion[i].reset();
        }

        int active_count = 0;

        auto sdl = g_sdlInput.snapshot();
        const uint64_t filter_now = ns::now_us();
        for (int i = 0; i < 4; ++i) {
            if (!sdl[i].connected) {
                sdl_filters[i].reset();
                continue;
            }
            logical_reports[i] = sdl[i].input;
            sdl_filters[i].apply(logical_reports[i], filter_now);
            logical_motion[i] = sdl[i].motion;
            present[i] = true;
            has_motion[i] = sdl[i].has_motion;
            sdl_for_slot[i] = i;
            active_count++;
        }

        // Keyboard single mode replaces P1 and moves the previous P1 controller
        // to the next free slot when possible.
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
                    sdl_for_slot[target] = sdl_for_slot[0];
                    active_count++;
                }
            }
            logical_reports[0].reset();
            logical_motion[0].reset();
            kb.apply(logical_reports[0]);
            present[0] = true;
            has_motion[0] = false;
            sdl_for_slot[0] = -1;
            active_count = std::max(active_count, 1);
        } else if (kb.mode == 2) {
            kb.apply(logical_reports[0]);
            present[0] = true;
            active_count = std::max(active_count, 1);
        }

        if (legacy_udp) {
            ns::Packet pkt{};
            pkt.magic   = ns::PROTO_MAGIC;
            pkt.version = ns::PROTO_VERSION;
            pkt.flags   = ns::FLAG_NONE;
            pkt.seq     = seq++;
            pkt.ts_us   = ns::now_us();
            pkt.report.reset();
            ns::HIDReport* legacy_pads[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
            for (int i = 0; i < 4; ++i) *legacy_pads[i] = logical_reports[i];

            uint8_t full_hmac[32];
            hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
            sendto(sock, (const char*)&pkt, (int)ns::PACKET_SIZE, 0, (const sockaddr*)&dest, sizeof(dest));
        } else {
            ExtendedUdpPacket pkt{};
            pkt.magic = ns::PROTO_MAGIC;
            pkt.version = ns::WEB_PROTO_VERSION;
            pkt.flags = ns::FLAG_NONE;
            pkt.seq = seq++;
            pkt.timestamp_us = ns::now_us();
            pkt.report.reset();
            ns::ExtendedHIDReport* ext_pads[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
            for (int i = 0; i < 4; ++i) {
                fill_extended_pad(*ext_pads[i], logical_reports[i], present[i], has_motion[i] ? &logical_motion[i] : nullptr);
            }

            uint8_t full_hmac[32];
            hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, EXT_UDP_PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
            sendto(sock, (const char*)&pkt, (int)sizeof(pkt), 0, (const sockaddr*)&dest, sizeof(dest));

            pump_udp_rumble(sock, rumble, sdl_for_slot);
            rumble.update_timeouts(sdl_for_slot);
        }

        if (active_count > 0) {
            no_controllers_printed = false;
            const int send_interval_ms = legacy_udp ? ns::HORI_UDP_INTERVAL_MS : ns::PRO_UDP_INTERVAL_MS;
            std::this_thread::sleep_for(std::chrono::milliseconds(send_interval_ms));
        } else {
            if (!no_controllers_printed) {
                std::cout << "No controllers detected - waiting for connections...\n";
                no_controllers_printed = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    // Not reached during normal Ctrl+C termination, but useful if this loop is
    // ever made stoppable.
    rumble.stop_all();
    std::cout << "\nShutting down...\n";
    return cleanup_and_exit(0, sock);
}
