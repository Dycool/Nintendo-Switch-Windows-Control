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


#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

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
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]] [--legacy] [--no-raw] [--macro file.json]\n";
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
        } else if (strcmp(argv[i], "--macro") == 0 && i + 1 < argc) {
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
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]] [--legacy] [--no-raw] [--macro file.json]\n";
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
            ns::Packet pkt; memset(&pkt, 0, sizeof(pkt));
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
            ExtendedUdpPacket pkt; memset(&pkt, 0, sizeof(pkt));
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