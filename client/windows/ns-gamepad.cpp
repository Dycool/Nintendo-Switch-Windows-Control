#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>           
#include <mmsystem.h>          
#include <winsock2.h>          
#include <ws2tcpip.h>          
#include <xinput.h>            

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
#include <memory>
#include <hidsdi.h>
#include <setupapi.h>
#include <fstream>
#include <sstream>
#include <cctype>
#include "../../server/rpi/include/sha256.h"

// Import external protocol structures
#include "../../server/rpi/include/protocol.hpp"
#include <stdexcept>
#include <limits>


#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
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

static uint8_t raw12_to_axis8(uint16_t raw) {
    int delta = (int)raw - 0x800;
    int v = 128;
    if (delta > 0) v = 128 + (delta * 127) / 0x600;
    else if (delta < 0) v = 128 + (delta * 128) / 0x600;
    return (uint8_t)std::clamp(v, 0, 255);
}

static uint8_t invert_axis8_centered(uint8_t v) {
    return v == 128 ? 128 : (uint8_t)(255 - v);
}

static void sony_dpad_to_hat(uint8_t dpad, ns::HIDReport& r) {
    switch (dpad & 0x0F) {
        case 0: r.hat = ns::HAT_N;  break;
        case 1: r.hat = ns::HAT_NE; break;
        case 2: r.hat = ns::HAT_E;  break;
        case 3: r.hat = ns::HAT_SE; break;
        case 4: r.hat = ns::HAT_S;  break;
        case 5: r.hat = ns::HAT_SW; break;
        case 6: r.hat = ns::HAT_W;  break;
        case 7: r.hat = ns::HAT_NW; break;
        default: r.hat = ns::HAT_NEUTRAL; break;
    }
}

struct RawPadState {
    bool connected = false;
    ns::HIDReport input{};
    ns::MotionReport motion{};
    bool has_motion = false;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string name;
};

struct RawHidDeviceInfo {
    std::string path;
    uint16_t vid = 0;
    uint16_t pid = 0;
    USHORT input_len = 64;
    USHORT output_len = 64;
};

class RawHidManager {
public:
    void start() {
        running.store(true);
        auto infos = enumerate_supported_devices();
        int slot = 0;
        for (const auto& info : infos) {
            if (slot >= 4) break;
            auto dev = std::make_unique<Device>();
            dev->slot = slot;
            dev->info = info;
            dev->handle = CreateFileA(info.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (dev->handle == INVALID_HANDLE_VALUE) {
                dev->handle = CreateFileA(info.path.c_str(), GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            if (dev->handle == INVALID_HANDLE_VALUE) continue;

            {
                std::lock_guard<std::mutex> lk(mtx);
                states[slot].connected = true;
                states[slot].vid = info.vid;
                states[slot].pid = info.pid;
                states[slot].name = device_name(info.vid, info.pid);
            }

            std::cout << "Raw HID controller P" << (slot + 1) << ": "
                      << device_name(info.vid, info.pid)
                      << " (VID 0x" << std::hex << info.vid
                      << " PID 0x" << info.pid << std::dec << ")\n";

            dev->thread = std::thread([this, d = dev.get()] { read_loop(d); });
            devices.push_back(std::move(dev));
            slot++;
        }
        if (slot == 0)
            std::cout << "No raw HID gyro controllers detected. XInput still works.\n";
    }

    std::array<RawPadState, 4> snapshot() {
        std::lock_guard<std::mutex> lk(mtx);
        return states;
    }

    int count_connected() {
        std::lock_guard<std::mutex> lk(mtx);
        int n = 0;
        for (auto& s : states) if (s.connected) n++;
        return n;
    }

    void set_rumble(int raw_slot, uint8_t low, uint8_t high) {
        if (raw_slot < 0 || raw_slot >= (int)devices.size()) return;
        Device* d = devices[raw_slot].get();
        if (!d || d->handle == INVALID_HANDLE_VALUE) return;

        if (is_ds4(d->info.vid, d->info.pid)) {
            // DualShock 4 USB output report 0x05. Bluetooth rumble needs CRC and is
            // intentionally not attempted here; USB is reliable and app-independent.
            uint8_t out[32] = {};
            out[0] = 0x05;
            out[1] = 0xFF;
            out[4] = high; // small/high-frequency motor
            out[5] = low;  // large/low-frequency motor
            DWORD written = 0;
            WriteFile(d->handle, out, sizeof(out), &written, nullptr);
            HidD_SetOutputReport(d->handle, out, sizeof(out));
        } else if (is_dualsense(d->info.vid, d->info.pid)) {
            // Best-effort DualSense USB rumble. Different firmware/BT modes may need
            // fuller output reports, but this works for many wired devices.
            uint8_t out[48] = {};
            out[0] = 0x02;
            out[1] = 0xFF;
            out[2] = 0x04;
            out[3] = high;
            out[4] = low;
            DWORD written = 0;
            WriteFile(d->handle, out, sizeof(out), &written, nullptr);
            HidD_SetOutputReport(d->handle, out, sizeof(out));
        }
    }

private:
    struct Device {
        HANDLE handle = INVALID_HANDLE_VALUE;
        RawHidDeviceInfo info{};
        int slot = -1;
        std::thread thread;
        ~Device() {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            if (thread.joinable()) thread.detach();
        }
    };

    std::atomic<bool> running{false};
    std::mutex mtx;
    std::array<RawPadState, 4> states{};
    std::vector<std::unique_ptr<Device>> devices;

    static bool is_ds4(uint16_t vid, uint16_t pid) {
        return vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC);
    }
    static bool is_dualsense(uint16_t vid, uint16_t pid) {
        return vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2);
    }
    static bool is_switch_pro(uint16_t vid, uint16_t pid) {
        return vid == 0x057E && pid == 0x2009;
    }
    static bool is_supported(uint16_t vid, uint16_t pid) {
        return is_ds4(vid, pid) || is_dualsense(vid, pid) || is_switch_pro(vid, pid);
    }
    static std::string device_name(uint16_t vid, uint16_t pid) {
        if (is_ds4(vid, pid)) return "DualShock 4 / DS4-compatible";
        if (is_dualsense(vid, pid)) return "DualSense";
        if (is_switch_pro(vid, pid)) return "Nintendo Switch Pro Controller";
        return "Raw HID controller";
    }

    static std::vector<RawHidDeviceInfo> enumerate_supported_devices() {
        std::vector<RawHidDeviceInfo> out;
        GUID hid_guid;
        HidD_GetHidGuid(&hid_guid);
        HDEVINFO devs = SetupDiGetClassDevsA(&hid_guid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devs == INVALID_HANDLE_VALUE) return out;

        for (DWORD i = 0; ; ++i) {
            SP_DEVICE_INTERFACE_DATA ifdata{};
            ifdata.cbSize = sizeof(ifdata);
            if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &hid_guid, i, &ifdata)) break;

            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailA(devs, &ifdata, nullptr, 0, &needed, nullptr);
            if (!needed) continue;
            std::vector<uint8_t> detail_buf(needed);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(detail_buf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            if (!SetupDiGetDeviceInterfaceDetailA(devs, &ifdata, detail, needed, nullptr, nullptr)) continue;

            HANDLE h = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                h = CreateFileA(detail->DevicePath, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
            if (h == INVALID_HANDLE_VALUE) continue;

            HIDD_ATTRIBUTES attr{};
            attr.Size = sizeof(attr);
            if (!HidD_GetAttributes(h, &attr) || !is_supported(attr.VendorID, attr.ProductID)) {
                CloseHandle(h);
                continue;
            }

            RawHidDeviceInfo info;
            info.path = detail->DevicePath;
            info.vid = attr.VendorID;
            info.pid = attr.ProductID;

            PHIDP_PREPARSED_DATA pp = nullptr;
            if (HidD_GetPreparsedData(h, &pp)) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                    info.input_len = caps.InputReportByteLength ? caps.InputReportByteLength : 64;
                    info.output_len = caps.OutputReportByteLength ? caps.OutputReportByteLength : 64;
                }
                HidD_FreePreparsedData(pp);
            }
            CloseHandle(h);
            out.push_back(info);
        }
        SetupDiDestroyDeviceInfoList(devs);
        return out;
    }

    static bool parse_ds4(const uint8_t* b, DWORD len, ns::HIDReport& r, ns::MotionReport& m, bool& has_motion) {
        r.reset(); m.reset(); has_motion = false;
        int o = -1, motion_o = -1;
        if (len >= 25 && b[0] == 0x01) { o = 0; motion_o = 13; }       // USB
        else if (len >= 78 && b[0] == 0x11) { o = 2; motion_o = 15; }  // BT-ish best effort
        else return false;

        r.lx = b[o + 1]; r.ly = b[o + 2]; r.rx = b[o + 3]; r.ry = b[o + 4];
        uint8_t btn0 = b[o + 5], btn1 = b[o + 6], btn2 = b[o + 7];
        sony_dpad_to_hat(btn0, r);
        if (btn0 & 0x10) r.buttons |= ns::BTN_Y; // Square
        if (btn0 & 0x20) r.buttons |= ns::BTN_B; // Cross
        if (btn0 & 0x40) r.buttons |= ns::BTN_A; // Circle
        if (btn0 & 0x80) r.buttons |= ns::BTN_X; // Triangle
        if (btn1 & 0x01) r.buttons |= ns::BTN_L;
        if (btn1 & 0x02) r.buttons |= ns::BTN_R;
        if ((btn1 & 0x04) || b[o + 8] > 128) r.buttons |= ns::BTN_ZL;
        if ((btn1 & 0x08) || b[o + 9] > 128) r.buttons |= ns::BTN_ZR;
        if (btn1 & 0x10) r.buttons |= ns::BTN_MINUS;
        if (btn1 & 0x20) r.buttons |= ns::BTN_PLUS;
        if (btn1 & 0x40) r.buttons |= ns::BTN_LSTICK;
        if (btn1 & 0x80) r.buttons |= ns::BTN_RSTICK;
        if (btn2 & 0x01) r.buttons |= ns::BTN_HOME;
        if (btn2 & 0x02) r.buttons |= ns::BTN_CAPTURE; // touchpad click

        if ((int)len >= motion_o + 12) {
            m.gx = read_le16(b + motion_o + 0);
            m.gy = read_le16(b + motion_o + 2);
            m.gz = read_le16(b + motion_o + 4);
            m.ax = read_le16(b + motion_o + 6);
            m.ay = read_le16(b + motion_o + 8);
            m.az = read_le16(b + motion_o + 10);
            has_motion = true;
        }
        return true;
    }

    static bool parse_dualsense(const uint8_t* b, DWORD len, ns::HIDReport& r, ns::MotionReport& m, bool& has_motion) {
        r.reset(); m.reset(); has_motion = false;
        int o = -1, motion_o = -1;
        if (len >= 40 && b[0] == 0x01) { o = 0; motion_o = 16; }       // USB
        else if (len >= 78 && b[0] == 0x31) { o = 1; motion_o = 17; }  // BT-ish best effort
        else return false;

        r.lx = b[o + 1]; r.ly = b[o + 2]; r.rx = b[o + 3]; r.ry = b[o + 4];
        uint8_t l2 = b[o + 5], r2 = b[o + 6];
        uint8_t btn0 = b[o + 8], btn1 = b[o + 9], btn2 = b[o + 10];
        sony_dpad_to_hat(btn0, r);
        if (btn0 & 0x10) r.buttons |= ns::BTN_Y; // Square
        if (btn0 & 0x20) r.buttons |= ns::BTN_B; // Cross
        if (btn0 & 0x40) r.buttons |= ns::BTN_A; // Circle
        if (btn0 & 0x80) r.buttons |= ns::BTN_X; // Triangle
        if (btn1 & 0x01) r.buttons |= ns::BTN_L;
        if (btn1 & 0x02) r.buttons |= ns::BTN_R;
        if ((btn1 & 0x04) || l2 > 128) r.buttons |= ns::BTN_ZL;
        if ((btn1 & 0x08) || r2 > 128) r.buttons |= ns::BTN_ZR;
        if (btn1 & 0x10) r.buttons |= ns::BTN_MINUS;
        if (btn1 & 0x20) r.buttons |= ns::BTN_PLUS;
        if (btn1 & 0x40) r.buttons |= ns::BTN_LSTICK;
        if (btn1 & 0x80) r.buttons |= ns::BTN_RSTICK;
        if (btn2 & 0x01) r.buttons |= ns::BTN_HOME;
        if (btn2 & 0x02) r.buttons |= ns::BTN_CAPTURE;

        if ((int)len >= motion_o + 12) {
            m.gx = read_le16(b + motion_o + 0);
            m.gy = read_le16(b + motion_o + 2);
            m.gz = read_le16(b + motion_o + 4);
            m.ax = read_le16(b + motion_o + 6);
            m.ay = read_le16(b + motion_o + 8);
            m.az = read_le16(b + motion_o + 10);
            has_motion = true;
        }
        return true;
    }

    static bool parse_switch_pro(const uint8_t* b, DWORD len, ns::HIDReport& r, ns::MotionReport& m, bool& has_motion) {
        r.reset(); m.reset(); has_motion = false;
        if (len < 25 || b[0] != 0x30) return false;

        uint8_t br = b[3], bm = b[4], bl = b[5];
        if (br & 0x01) r.buttons |= ns::BTN_Y;
        if (br & 0x02) r.buttons |= ns::BTN_X;
        if (br & 0x04) r.buttons |= ns::BTN_B;
        if (br & 0x08) r.buttons |= ns::BTN_A;
        if (br & 0x40) r.buttons |= ns::BTN_R;
        if (br & 0x80) r.buttons |= ns::BTN_ZR;
        if (bm & 0x01) r.buttons |= ns::BTN_MINUS;
        if (bm & 0x02) r.buttons |= ns::BTN_PLUS;
        if (bm & 0x04) r.buttons |= ns::BTN_RSTICK;
        if (bm & 0x08) r.buttons |= ns::BTN_LSTICK;
        if (bm & 0x10) r.buttons |= ns::BTN_HOME;
        if (bm & 0x20) r.buttons |= ns::BTN_CAPTURE;
        if (bl & 0x40) r.buttons |= ns::BTN_L;
        if (bl & 0x80) r.buttons |= ns::BTN_ZL;

        bool down = bl & 0x01, up = bl & 0x02, right = bl & 0x04, left = bl & 0x08;
        if (up && right) r.hat = ns::HAT_NE; else if (up && left) r.hat = ns::HAT_NW;
        else if (down && right) r.hat = ns::HAT_SE; else if (down && left) r.hat = ns::HAT_SW;
        else if (up) r.hat = ns::HAT_N; else if (down) r.hat = ns::HAT_S;
        else if (left) r.hat = ns::HAT_W; else if (right) r.hat = ns::HAT_E;

        uint16_t lx = (uint16_t)b[6] | (((uint16_t)b[7] & 0x0F) << 8);
        uint16_t ly = (((uint16_t)b[7] >> 4) & 0x0F) | ((uint16_t)b[8] << 4);
        uint16_t rx = (uint16_t)b[9] | (((uint16_t)b[10] & 0x0F) << 8);
        uint16_t ry = (((uint16_t)b[10] >> 4) & 0x0F) | ((uint16_t)b[11] << 4);
        r.lx = raw12_to_axis8(lx);
        r.ly = invert_axis8_centered(raw12_to_axis8(ly));
        r.rx = raw12_to_axis8(rx);
        r.ry = invert_axis8_centered(raw12_to_axis8(ry));

        m.ax = read_le16(b + 13); m.ay = read_le16(b + 15); m.az = read_le16(b + 17);
        m.gx = read_le16(b + 19); m.gy = read_le16(b + 21); m.gz = read_le16(b + 23);
        has_motion = true;
        return true;
    }

    void read_loop(Device* d) {
        std::vector<uint8_t> buf(std::max<USHORT>(d->info.input_len, 64));
        while (running.load()) {
            DWORD got = 0;
            if (!ReadFile(d->handle, buf.data(), (DWORD)buf.size(), &got, nullptr) || got == 0) {
                Sleep(5);
                continue;
            }

            ns::HIDReport input;
            ns::MotionReport motion;
            bool has_motion = false;
            bool ok = false;
            if (is_ds4(d->info.vid, d->info.pid))
                ok = parse_ds4(buf.data(), got, input, motion, has_motion);
            else if (is_dualsense(d->info.vid, d->info.pid))
                ok = parse_dualsense(buf.data(), got, input, motion, has_motion);
            else if (is_switch_pro(d->info.vid, d->info.pid))
                ok = parse_switch_pro(buf.data(), got, input, motion, has_motion);

            if (!ok) continue;
            std::lock_guard<std::mutex> lk(mtx);
            states[d->slot].connected = true;
            states[d->slot].input = input;
            states[d->slot].motion = motion;
            states[d->slot].has_motion = has_motion;
        }
    }
};

class RumbleManager {
public:
    void apply_packet(const ns::RumblePacket& rp,
                      const int xinput_for_slot[4],
                      const int raw_for_slot[4],
                      RawHidManager& raw_hid) {
        if (rp.subpad >= 4) return;
        const int slot = rp.subpad;
        uint8_t low = rp.low_freq;
        uint8_t high = rp.high_freq;
        bool neutral = (low == 0 && high == 0) || rp.duration_10ms == 0;
        uint64_t now = ns::now_us();
        uint64_t dur_us = neutral ? 0ULL : std::max<uint64_t>(250000ULL, (uint64_t)rp.duration_10ms * 10000ULL);

        if (!neutral && states[slot].low == low && states[slot].high == high &&
            now - states[slot].last_set_us < 100000ULL) {
            states[slot].until_us = now + dur_us;
            return;
        }

        states[slot].low = low;
        states[slot].high = high;
        states[slot].until_us = neutral ? 0 : now + dur_us;
        states[slot].last_set_us = now;
        set_output(slot, neutral ? 0 : low, neutral ? 0 : high,
                   xinput_for_slot[slot], raw_for_slot[slot], raw_hid);
    }

    void update_timeouts(const int xinput_for_slot[4], const int raw_for_slot[4], RawHidManager& raw_hid) {
        uint64_t now = ns::now_us();
        for (int i = 0; i < 4; ++i) {
            if (states[i].until_us != 0 && now > states[i].until_us) {
                states[i].until_us = 0;
                states[i].low = states[i].high = 0;
                set_output(i, 0, 0, xinput_for_slot[i], raw_for_slot[i], raw_hid);
            }
        }
    }

private:
    struct SlotState {
        uint8_t low = 0, high = 0;
        uint64_t until_us = 0;
        uint64_t last_set_us = 0;
        int last_xinput = -1;
        int last_raw = -1;
    } states[4];

    static WORD motor_word(uint8_t v) {
        return (WORD)((uint32_t)v * 65535u / 255u);
    }

    void stop_previous_if_moved(int slot, int xinput_idx, int raw_idx, RawHidManager& raw_hid) {
        if (states[slot].last_xinput != -1 && states[slot].last_xinput != xinput_idx) {
            XINPUT_VIBRATION z{};
            XInputSetState((DWORD)states[slot].last_xinput, &z);
        }
        if (states[slot].last_raw != -1 && states[slot].last_raw != raw_idx)
            raw_hid.set_rumble(states[slot].last_raw, 0, 0);
    }

    void set_output(int slot, uint8_t low, uint8_t high, int xinput_idx, int raw_idx, RawHidManager& raw_hid) {
        stop_previous_if_moved(slot, xinput_idx, raw_idx, raw_hid);
        if (xinput_idx >= 0 && xinput_idx < 4) {
            XINPUT_VIBRATION vib{};
            vib.wLeftMotorSpeed = motor_word(low);   // low/large motor
            vib.wRightMotorSpeed = motor_word(high); // high/small motor
            XInputSetState((DWORD)xinput_idx, &vib);
        }
        if (raw_idx >= 0)
            raw_hid.set_rumble(raw_idx, low, high);
        states[slot].last_xinput = xinput_idx;
        states[slot].last_raw = raw_idx;
    }
};

static void pump_udp_rumble(SOCKET sock,
                            RumbleManager& rumble,
                            const int xinput_for_slot[4],
                            const int raw_for_slot[4],
                            RawHidManager& raw_hid) {
    uint8_t buf[64];
    sockaddr_in from{};
    int from_len = sizeof(from);
    for (;;) {
        int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK && e != WSAEINTR)
                std::cerr << "UDP receive error: " << e << "\n";
            break;
        }
        if (n == (int)sizeof(ns::RumblePacket)) {
            ns::RumblePacket rp{};
            memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::RUMBLE_MAGIC)
                rumble.apply_packet(rp, xinput_for_slot, raw_for_slot, raw_hid);
        }
    }
}

// Applies deadzone to an analog stick axis
uint8_t apply_deadzone(SHORT val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    else                 scaled = 128 - ((abs(val) - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

// Maps XInput layout to Switch Pro Controller layout
ns::HIDReport map_xinput_to_switch(const XINPUT_GAMEPAD& pad) {
    ns::HIDReport r; r.reset();
    if (pad.wButtons & XINPUT_GAMEPAD_A) r.buttons |= ns::BTN_B; 
    if (pad.wButtons & XINPUT_GAMEPAD_B) r.buttons |= ns::BTN_A;
    if (pad.wButtons & XINPUT_GAMEPAD_X) r.buttons |= ns::BTN_Y;
    if (pad.wButtons & XINPUT_GAMEPAD_Y) r.buttons |= ns::BTN_X;
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  r.buttons |= ns::BTN_L;
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) r.buttons |= ns::BTN_R;
    if (pad.bLeftTrigger > 128)  r.buttons |= ns::BTN_ZL;
    if (pad.bRightTrigger > 128) r.buttons |= ns::BTN_ZR;
    if (pad.wButtons & XINPUT_GAMEPAD_BACK)  r.buttons |= ns::BTN_MINUS;
    if (pad.wButtons & XINPUT_GAMEPAD_START) r.buttons |= ns::BTN_PLUS;
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB)  r.buttons |= ns::BTN_LSTICK;
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) r.buttons |= ns::BTN_RSTICK;

    // Emulate HOME and CAPTURE buttons using button combos
    if ((pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) && (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)) {
        r.buttons |= ns::BTN_HOME; r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
    }
    if ((pad.wButtons & XINPUT_GAMEPAD_BACK) && (pad.wButtons & XINPUT_GAMEPAD_START)) {
        r.buttons |= ns::BTN_CAPTURE; r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);
    }

    bool up = (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP), down = (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
    bool left = (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT), right = (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);

    if (up && right) r.hat = ns::HAT_NE; else if (up && left) r.hat = ns::HAT_NW;
    else if (down && right) r.hat = ns::HAT_SE; else if (down && left) r.hat = ns::HAT_SW;
    else if (up) r.hat = ns::HAT_N; else if (down) r.hat = ns::HAT_S;
    else if (left) r.hat = ns::HAT_W; else if (right) r.hat = ns::HAT_E;

    r.lx = apply_deadzone(pad.sThumbLX, false); r.ly = apply_deadzone(pad.sThumbLY, true);
    r.rx = apply_deadzone(pad.sThumbRX, false); r.ry = apply_deadzone(pad.sThumbRY, true);
    return r;
}


// ── XInput Throttling (Prevents USB driver crash on Windows) ──
static uint64_t g_last_check_us[4] = {0, 0, 0, 0};
static bool g_is_connected[4] = {false, false, false, false};
static int keyboard_mode = 0; // 0=off, 1=single, 2=override

// Checks for controller status efficiently and maps its inputs
void fetch_pad_throttled(DWORD index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    uint64_t now = ns::now_us();
    
    // Only poll disconnected controllers once per second to save CPU and USB bandwidth
    if (!g_is_connected[index] && (now - g_last_check_us[index] < 1'000'000)) {
        conn = false; return; 
    }

    XINPUT_STATE state; ZeroMemory(&state, sizeof(XINPUT_STATE));
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
        if (g_is_connected[index])
            std::cout << "Controller in slot P" << (index + 1) << " disconnected.\n";
        g_is_connected[index] = false;
        g_last_check_us[index] = now;
        conn = false; return;
    }
    
    if (!g_is_connected[index]) {
        int slot = index + 1;
        if (keyboard_mode == 1 && index == 0) {
            if (!g_is_connected[1]) slot = 2;
            else if (!g_is_connected[2]) slot = 3;
            else slot = 4;
        }
        std::cout << "Mapped 'Xbox controller' to local slot P" << slot << "\n";
    }
    g_is_connected[index] = true; conn = true;
    rep = map_xinput_to_switch(state.Gamepad);
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

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]] [--legacy] [--no-raw] [--macro file.json [--upload-macro file.json]]\n";
        std::cerr << "  -k          Enable keyboard mode (default: single)\n";
        std::cerr << "  --legacy    Send old input-only UDP packets; disables UDP rumble/gyro\n";
        std::cerr << "  --no-raw    Disable raw HID DS4/DualSense/Switch-Pro support\n";
        std::cerr << "  --macro     Connect, play a P1 macro JSON/string, then exit\n";
        timeEndPeriod(1); return 1;
    }

    std::string host;
    int port = ns::DEFAULT_PORT;
    bool legacy_udp = false;
    bool raw_hid_enabled = true;
    bool macro_mode = false;
    std::string macro_path;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--legacy") == 0) {
            legacy_udp = true;
         } else if (strcmp(argv[i], "--no-raw") == 0) {
            raw_hid_enabled = false;
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
                    timeEndPeriod(1); return 1;
                }
                host.resize(colon);
            }
        }
    }

    if (host.empty()) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]] [--legacy] [--no-raw] [--macro file.json [--upload-macro file.json]]\n";
        timeEndPeriod(1); return 1;
    }

    KeyBindings kb;
    if (keyboard_mode) {
        kb.load_or_create();
        kb.mode = keyboard_mode;
        std::cout << "Keyboard mode enabled (" << (keyboard_mode == 1 ? "single" : "override") << ") - ";
        std::cout << (keyboard_mode == 1 ? "replaces" : "augments") << " Player 1\n";
    }

    RawHidManager raw_hid;
    if (raw_hid_enabled) {
        raw_hid.start();
    } else {
        std::cout << "Raw HID support disabled; using XInput only.\n";
    }
    if (legacy_udp) {
        std::cout << "Legacy UDP mode: input only. UDP rumble and gyro are disabled.\n";
    } else {
        std::cout << "Extended UDP mode: rumble replies + gyro/motion enabled.\n";
    }

    uint8_t hmac_key[32]; derive_key(ns::DEFAULT_SECRET, hmac_key);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa); 
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);
    
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; 

    char port_buf[8]; snprintf(port_buf, sizeof(port_buf), "%d", port);
    if (getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0 || res == nullptr) {
        std::cerr << "ERROR: Unable to resolve IP: " << host << "\n";
        timeEndPeriod(1); WSACleanup(); return 1;
    }
    
    sockaddr_in dest{}; memcpy(&dest, res->ai_addr, sizeof(dest));
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
    uint32_t seq = 0;
    RumbleManager rumble;
    std::vector<MacroStep> macro_steps;
    uint64_t macro_start_us = 0;
    bool macro_stop_after_send = false;
    if (macro_mode) {
        macro_steps = macro_load_file(macro_path);
        if (macro_steps.empty()) {
            std::cerr << "Macro file has no usable commands: " << macro_path << "\n";
            closesocket(sock); WSACleanup(); timeEndPeriod(1); return 1;
        }
        macro_start_us = ns::now_us();
        std::cout << "Macro mode: executing " << macro_steps.size() << " steps on P1, then exiting.\n";
    }

    while (true) {
        ns::HIDReport logical_reports[4];
        ns::MotionReport logical_motion[4];
        bool present[4] = {false, false, false, false};
        bool has_motion[4] = {false, false, false, false};
        int xinput_for_slot[4] = {-1, -1, -1, -1};
        int raw_for_slot[4] = {-1, -1, -1, -1};
        for (int i = 0; i < 4; ++i) {
            logical_reports[i].reset();
            logical_motion[i].reset();
        }

        int active_count = 0;
        static bool no_controllers_printed = false;

        // Raw HID first: lets DS4/DualSense/Switch Pro work without DS4Windows.
        if (raw_hid_enabled) {
            auto raw = raw_hid.snapshot();
            for (int i = 0; i < 4; ++i) {
                if (!raw[i].connected) continue;
                logical_reports[i] = raw[i].input;
                present[i] = true;
                raw_for_slot[i] = i;
                if (raw[i].has_motion) {
                    logical_motion[i] = raw[i].motion;
                    has_motion[i] = true;
                }
                active_count++;
            }
        }

        // XInput fallback / extra controllers. If a raw controller already claimed
        // a logical slot, put XInput into the next free slot to avoid overwriting it.
        for (DWORD i = 0; i < 4; ++i) {
            ns::HIDReport temp_rep;
            bool is_conn = false;
            fetch_pad_throttled(i, temp_rep, is_conn);
            if (!is_conn) continue;

            int target = (int)i;
            if (present[target]) {
                target = -1;
                for (int s = 0; s < 4; ++s) {
                    if (!present[s]) { target = s; break; }
                }
            }
            if (target < 0) continue;

            logical_reports[target] = temp_rep;
            present[target] = true;
            xinput_for_slot[target] = (int)i;
            active_count++;
        }

        // Keyboard overrides Player 1, preserving the previous P1 controller in
        // another slot when possible, matching the older client behavior.
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
                    xinput_for_slot[target] = xinput_for_slot[0];
                    raw_for_slot[target] = raw_for_slot[0];
                    active_count++;
                }
            }
            logical_reports[0].reset();
            logical_motion[0].reset();
            kb.apply(logical_reports[0]);
            present[0] = true;
            has_motion[0] = false;
            xinput_for_slot[0] = -1;
            raw_for_slot[0] = -1;
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
            for (int i = 0; i < 4; ++i) { logical_reports[i].reset(); logical_motion[i].reset(); present[i] = false; has_motion[i] = false; xinput_for_slot[i] = raw_for_slot[i] = -1; }
            logical_reports[0] = macro_rep;
            present[0] = true;
            has_motion[0] = false;
            active_count = 1;
            if (!active_macro && elapsed_ms > macro_total_ms(macro_steps) + 120) macro_stop_after_send = true;
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
            pkt.version = ns::PROTO_VERSION;
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

            pump_udp_rumble(sock, rumble, xinput_for_slot, raw_for_slot, raw_hid);
            rumble.update_timeouts(xinput_for_slot, raw_for_slot, raw_hid);
        }

        // Sleep to throttle transmission (~250Hz when active, 2Hz when idle)
        if (macro_stop_after_send) break;

        if (active_count > 0) {
            no_controllers_printed = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        } else {
            if (!no_controllers_printed) {
                std::cout << "No controllers detected - waiting for connections...\n";
                no_controllers_printed = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    std::cout << "\nShutting down...\n";
    closesocket(sock); WSACleanup(); 
    timeEndPeriod(1); return 0;
}