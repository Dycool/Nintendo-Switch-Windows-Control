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
static std::string g_macro_upload_pending;
static std::string macros_file_path() { return get_config_dir() + "/macros.json"; }
static void load_macro_text() { g_macro_text = macro_read_file(macros_file_path()); }
static void save_macro_text(const std::string& txt) { std::string dir = get_config_dir(); g_mkdir_with_parents(dir.c_str(), 0755); FILE* f = fopen(macros_file_path().c_str(), "w"); if (f) { fwrite(txt.data(), 1, txt.size(), f); fclose(f); } g_macro_text = txt; }
static void start_macro_text(const std::string& txt) { std::lock_guard<std::mutex> lk(g_macro_mtx); g_macro_upload_pending = txt; g_macro_steps = macro_parse_text(txt); g_macro_running = g_macro_steps.empty(); g_macro_start_us = ns::now_us(); }
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
    return macro_pretty_json(commands, "Recorded Macro");
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

        {
            std::string upload;
            { std::lock_guard<std::mutex> lk(g_macro_mtx); upload.swap(g_macro_upload_pending); }
            if (!upload.empty()) send_macro_udp_packet(sock, dest, g_hmacKey, upload, 0);
        }

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
        else if (r == 6) { GtkWidget* fc = gtk_file_chooser_dialog_new("Import Macros JSON", nullptr, GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL, "Open", GTK_RESPONSE_ACCEPT, nullptr); if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) { char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc)); gchar* contents=nullptr; gsize len=0; if (g_file_get_contents(fn, &contents, &len, nullptr)) { save_macro_text(std::string(contents, len)); gtk_text_buffer_set_text(buf, contents, (gint)len); g_free(contents); } g_free(fn); } gtk_widget_destroy(fc); }
        else if (r == 7) { GtkWidget* fc = gtk_file_chooser_dialog_new("Export Macros JSON", nullptr, GTK_FILE_CHOOSER_ACTION_SAVE, "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, nullptr); gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), "ns-macros.json"); if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) { char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc)); std::string pretty = macro_pretty_json(txt); g_file_set_contents(fn, pretty.c_str(), (gssize)pretty.size(), nullptr); g_free(fn); } gtk_widget_destroy(fc); }
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