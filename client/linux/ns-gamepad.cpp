/// ns-gamepad.cpp  —  Linux frontend for the USB gamepad bridge
///
/// Uses SDL3 Gamepad API for cross-platform controller support.
/// Automatically detects Xbox, PlayStation, and generic controllers
/// via USB or Bluetooth — no raw joystick API needed.
///
/// Build:
///   g++ -O3 -std=c++17 ns-gamepad.cpp -o ns-gamepad -lpthread -lSDL3
///
/// Usage:
///   ./ns-gamepad <RASPBERRY_PI_IP[:PORT]>
///   ./ns-gamepad <RASPBERRY_PI_IP[:PORT]> --legacy
///   ./ns-gamepad <RASPBERRY_PI_IP[:PORT]> --macro macro.json [--upload-macro macro.json]

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
#include <vector>
#include <cerrno>

#include <fstream>
#include <sstream>
#include <cctype>
#include <SDL3/SDL.h>

#define SDL_INIT_GAMECONTROLLER SDL_INIT_GAMEPAD
#define SDL_CONTROLLER_BUTTON_A SDL_GAMEPAD_BUTTON_SOUTH
#define SDL_CONTROLLER_BUTTON_B SDL_GAMEPAD_BUTTON_EAST
#define SDL_CONTROLLER_BUTTON_X SDL_GAMEPAD_BUTTON_WEST
#define SDL_CONTROLLER_BUTTON_Y SDL_GAMEPAD_BUTTON_NORTH
#define SDL_CONTROLLER_BUTTON_LEFTSHOULDER SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
#define SDL_CONTROLLER_BUTTON_RIGHTSHOULDER SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
#define SDL_CONTROLLER_BUTTON_BACK SDL_GAMEPAD_BUTTON_BACK
#define SDL_CONTROLLER_BUTTON_START SDL_GAMEPAD_BUTTON_START
#define SDL_CONTROLLER_BUTTON_LEFTSTICK SDL_GAMEPAD_BUTTON_LEFT_STICK
#define SDL_CONTROLLER_BUTTON_RIGHTSTICK SDL_GAMEPAD_BUTTON_RIGHT_STICK
#define SDL_CONTROLLER_BUTTON_GUIDE SDL_GAMEPAD_BUTTON_GUIDE
#define SDL_CONTROLLER_BUTTON_MISC1 SDL_GAMEPAD_BUTTON_MISC1
#define SDL_CONTROLLER_BUTTON_DPAD_UP SDL_GAMEPAD_BUTTON_DPAD_UP
#define SDL_CONTROLLER_BUTTON_DPAD_DOWN SDL_GAMEPAD_BUTTON_DPAD_DOWN
#define SDL_CONTROLLER_BUTTON_DPAD_LEFT SDL_GAMEPAD_BUTTON_DPAD_LEFT
#define SDL_CONTROLLER_BUTTON_DPAD_RIGHT SDL_GAMEPAD_BUTTON_DPAD_RIGHT
#define SDL_CONTROLLER_AXIS_TRIGGERLEFT SDL_GAMEPAD_AXIS_LEFT_TRIGGER
#define SDL_CONTROLLER_AXIS_TRIGGERRIGHT SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
#define SDL_CONTROLLER_AXIS_LEFTX SDL_GAMEPAD_AXIS_LEFTX
#define SDL_CONTROLLER_AXIS_LEFTY SDL_GAMEPAD_AXIS_LEFTY
#define SDL_CONTROLLER_AXIS_RIGHTX SDL_GAMEPAD_AXIS_RIGHTX
#define SDL_CONTROLLER_AXIS_RIGHTY SDL_GAMEPAD_AXIS_RIGHTY
#define SDL_TRUE true

using SDL_GameController = SDL_Gamepad;

static int SDL_NumJoysticks() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_free(ids);
    return count;
}

static bool SDL_IsGameController(int index) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_free(ids);
    return index >= 0 && index < count;
}

static SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int index) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_JoystickID id = (ids && index >= 0 && index < count) ? ids[index] : 0;
    SDL_free(ids);
    return id;
}

static SDL_GameController* SDL_GameControllerOpen(int index) {
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    return id ? SDL_OpenGamepad(id) : nullptr;
}

static SDL_Joystick* SDL_GameControllerGetJoystick(SDL_GameController* pad) { return SDL_GetGamepadJoystick(pad); }
static SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick* js) { return SDL_GetJoystickID(js); }
static const char* SDL_GameControllerName(SDL_GameController* pad) { return SDL_GetGamepadName(pad); }
static bool SDL_GameControllerGetAttached(SDL_GameController* pad) { return SDL_GamepadConnected(pad); }
static void SDL_GameControllerClose(SDL_GameController* pad) { SDL_CloseGamepad(pad); }
static bool SDL_GameControllerGetButton(SDL_GameController* pad, SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); }
static Sint16 SDL_GameControllerGetAxis(SDL_GameController* pad, SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a); }
static bool SDL_GameControllerHasSensor(SDL_GameController* pad, SDL_SensorType sensor) { return SDL_GamepadHasSensor(pad, sensor); }
static int SDL_GameControllerSetSensorEnabled(SDL_GameController* pad, SDL_SensorType sensor, bool enabled) { return SDL_SetGamepadSensorEnabled(pad, sensor, enabled) ? 0 : -1; }
static int SDL_GameControllerGetSensorData(SDL_GameController* pad, SDL_SensorType sensor, float* data, int count) { return SDL_GetGamepadSensorData(pad, sensor, data, count) ? 0 : -1; }
static bool SDL_GameControllerHasRumble(SDL_GameController* pad) {
    SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
    return props && (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ||
                     SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false));
}
static int SDL_GameControllerRumble(SDL_GameController* pad, uint16_t low, uint16_t high, uint32_t ms) {
    const bool stop = (low == 0 && high == 0) || ms == 0;
    bool ok_main = SDL_RumbleGamepad(pad, stop ? 0 : low, stop ? 0 : high, ms);
    SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
    bool trigger_capable = props && SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
    bool ok_trigger = true;
    if (trigger_capable || !ok_main || stop) {
        ok_trigger = SDL_RumbleGamepadTriggers(pad, stop ? 0 : low, stop ? 0 : high, ms);
    }
    return (ok_main || ok_trigger) ? 0 : -1;
}
static void SDL_GameControllerUpdate() { SDL_UpdateGamepads(); }

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/resource.h>
#include <fcntl.h>
#include "../../server/rpi/include/sha256.h"

// Import shared wire protocol structures.
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
//  Shared gamepad state (SDL3)
// ─────────────────────────────────────────────────────────────────────────────

static SDL_Gamepad* g_pads[4] = {nullptr, nullptr, nullptr, nullptr};
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
//  SDL3 Discovery, Input, Sensors, Rumble
// ─────────────────────────────────────────────────────────────────────────────

static void enable_pad_sensors(int slot, SDL_GameController* pad) {
    g_pad_accel_enabled[slot] = false;
    g_pad_gyro_enabled[slot] = false;

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

static bool sdl_name_contains(SDL_GameController* pad, const char* needle) {
    const char* raw = SDL_GameControllerName(pad);
    std::string name = raw ? raw : "";
    for (char& c : name) c = (char)std::toupper((unsigned char)c);
    return name.find(needle) != std::string::npos;
}

static bool sdl_has_native_home_capture(SDL_GameController* pad) {
    if (SDL_GetGamepadVendor(pad) == 0x057E) return true;
    return sdl_name_contains(pad, "VENDOR") ||
           sdl_name_contains(pad, "CONSOLE") ||
           sdl_name_contains(pad, "USB GAMEPAD");
}

static bool sdl_should_use_combo_shortcuts(SDL_GameController* pad) {
    if (sdl_has_native_home_capture(pad)) return false;
    if (SDL_GetGamepadVendor(pad) == 0x045E) return true;
    return sdl_name_contains(pad, "XBOX") ||
           sdl_name_contains(pad, "XINPUT") ||
           sdl_name_contains(pad, "MICROSOFT");
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
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_MISC1))   rep.buttons |= ns::BTN_CAPTURE;

    if (sdl_should_use_combo_shortcuts(pad) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
        rep.buttons |= ns::BTN_HOME; rep.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
    }
    if (sdl_should_use_combo_shortcuts(pad) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) {
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
            constexpr float RAD_TO_DEG = 57.29577951308232f;
            constexpr float CONSOLE_GYRO_SCALE = RAD_TO_DEG * 16.0f;
            motion.gx = clamp_i16_from_float(gyro[0] * CONSOLE_GYRO_SCALE);
            motion.gy = clamp_i16_from_float(gyro[1] * CONSOLE_GYRO_SCALE);
            motion.gz = clamp_i16_from_float(gyro[2] * CONSOLE_GYRO_SCALE);
            any = true;
        }
    }

    return any;
}

static void set_pad_rumble(int index, uint8_t low, uint8_t high, uint32_t duration_ms) {
    if (index < 0 || index >= 4) return;
    SDL_GameController* pad = g_pads[index];
    if (!pad) return;

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
}

class RumbleManager {
public:
    void apply_precision_packet(const ns::PrecisionRumblePacket& rp, const int controller_for_slot[4]) {
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
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    std::string host;
    int port = ns::DEFAULT_PORT;
    bool macro_mode = false;
    std::string macro_path;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--legacy") == 0) {
            g_legacy_udp = true;
        } else if ((std::strcmp(argv[i], "--macro") == 0 ||
                    std::strcmp(argv[i], "--upload-macro") == 0 ||
                    std::strcmp(argv[i], "--server-macro") == 0)) {
            if (i + 1 >= argc) {
                std::cerr << argv[i] << " requires a macro JSON/commands file path\n";
                return 1;
            }
            macro_mode = true;
            macro_path = argv[++i];
        } else if (host.empty()) {
            host = argv[i];
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            return 1;
        }
    }

    if (host.empty()) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [--legacy] [--macro file.json [--upload-macro file.json]]\n";
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

    // Initialise SDL3 Gamepad subsystem.
    SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_" "SW" "ITCH", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_" "JOY" "_CONS", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_XBOX", "1");
    SDL_SetHint("SDL_JOYSTICK_ENHANCED_REPORTS", "1");
    Uint32 sdl_flags = SDL_INIT_GAMECONTROLLER;
#ifdef SDL_INIT_SENSOR
    sdl_flags |= SDL_INIT_SENSOR;
#endif
#ifdef SDL_INIT_HAPTIC
    sdl_flags |= SDL_INIT_HAPTIC;
#endif
    if (!SDL_Init(sdl_flags)) {
        std::cerr << "Failed to initialise SDL3: " << SDL_GetError() << "\n";
        close(sock); return 1;
    }

    if (g_legacy_udp)
        std::cout << "Legacy UDP mode: input only. UDP rumble and gyro are disabled.\n";
    else
        std::cout << "Extended UDP mode: SDL3 rumble replies + SDL sensor gyro enabled where supported.\n";

    if (macro_mode) {
        std::string macro_raw = macro_read_file(macro_path);
        if (macro_raw.empty()) {
            std::cerr << "Macro file is empty or cannot be read: " << macro_path
                      << " (" << macro_last_error() << ")\n";
            close(sock);
            SDL_Quit();
            return 1;
        }

        auto macro_steps_for_wait = macro_parse_text(macro_raw);
        if (macro_steps_for_wait.empty()) {
            std::cerr << "Macro file has no usable commands: " << macro_path
                      << " (" << macro_last_error() << ")\n";
            close(sock);
            SDL_Quit();
            return 1;
        }

        bool sent = send_macro_udp_packet(sock, dest, hmac_key, macro_raw, 0);
        std::cout << (sent ? "Uploaded server-side macro to P1.\n" : "Failed to upload server-side macro.\n");

        // Keep the CLI alive long enough for old/simple server macro runners,
        // using the locally expanded duration. Modern servers keep playing the
        // uploaded macro server-side even if the client exits after this wait.
        uint64_t wait_ms = macro_total_ms(macro_steps_for_wait);
        if (wait_ms > 600000ULL) wait_ms = 600000ULL; // avoid waiting forever on absurd macros
        std::this_thread::sleep_for(std::chrono::milliseconds((int)wait_ms + 180));

        close(sock);
        SDL_Quit();
        return sent ? 0 : 1;
    }

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
            ns::Packet pkt{};
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
            ExtendedUdpPacket pkt{};
            pkt.magic        = ns::PROTO_MAGIC;
            pkt.version      = ns::WEB_PROTO_VERSION;
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

        // Hori/legacy servers want 250Hz; Pro/modern servers want ~66Hz.
        if (active_count > 0) {
            const int send_interval_ms = g_legacy_udp ? ns::HORI_UDP_INTERVAL_MS : ns::PRO_UDP_INTERVAL_MS;
            next_tick += std::chrono::milliseconds(send_interval_ms);
        }
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
