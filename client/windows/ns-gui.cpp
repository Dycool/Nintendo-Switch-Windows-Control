#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <mmsystem.h>
#include <windowsx.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <xinput.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
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
#include <unordered_map>
#include <array>
#include <mutex>
#include <cmath>
#include <hidsdi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ── Protocol ──
namespace ns {
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;
static constexpr uint8_t  PROTO_VERSION = 4;
static constexpr uint16_t DEFAULT_PORT  = 7331;
static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
static constexpr size_t HMAC_TAG_SIZE = 16;
static constexpr uint32_t RUMBLE_MAGIC = 0x4E535652u;

enum Button : uint16_t {
    BTN_Y=1<<0, BTN_B=1<<1, BTN_A=1<<2, BTN_X=1<<3,
    BTN_L=1<<4, BTN_R=1<<5, BTN_ZL=1<<6, BTN_ZR=1<<7,
    BTN_MINUS=1<<8, BTN_PLUS=1<<9, BTN_LSTICK=1<<10, BTN_RSTICK=1<<11,
    BTN_HOME=1<<12, BTN_CAPTURE=1<<13,
};
enum Hat : uint8_t { HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3, HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8 };
enum Flags : uint8_t { FLAG_NONE=0x00, FLAG_RESET=0x01, FLAG_AUTOFIRE=0x02 };
#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0; uint8_t hat = HAT_NEUTRAL;
    uint8_t lx=128, ly=128, rx=128, ry=128, vendor=0;
    void reset() noexcept { *this = HIDReport{}; }
};
struct MultiReport {
    HIDReport p1, p2, p3, p4;
    void reset() noexcept { p1.reset(); p2.reset(); p3.reset(); p4.reset(); }
};
struct MotionReport {
    int16_t ax=0, ay=0, az=0, gx=0, gy=0, gz=0;
    void reset() noexcept { *this = MotionReport{}; }
};
struct ExtendedHIDReport {
    HIDReport input{};
    uint8_t flags = 0;          // bit 0 = pad present, even when neutral
    MotionReport motion{};
    uint8_t has_motion = 0;
    uint8_t reserved[3]{};
    void reset() noexcept { input.reset(); flags = 0; motion.reset(); has_motion = 0; reserved[0] = reserved[1] = reserved[2] = 0; }
};
struct ExtendedMultiReport {
    ExtendedHIDReport p1, p2, p3, p4;
    void reset() noexcept { p1.reset(); p2.reset(); p3.reset(); p4.reset(); }
};
struct RumblePacket {
    uint32_t magic;
    uint8_t subpad;
    uint8_t low_freq;
    uint8_t high_freq;
    uint8_t duration_10ms;
};
struct Packet {
    uint32_t magic; uint8_t version; uint8_t flags; uint16_t autofire_mask;
    uint32_t seq; uint64_t ts_us; MultiReport report; uint8_t hmac[HMAC_TAG_SIZE];
};
#pragma pack(pop)
static constexpr size_t PACKET_SIZE = sizeof(Packet);
static constexpr size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;
static constexpr size_t EXT_REPORT_SIZE = sizeof(ExtendedHIDReport);
static constexpr size_t EXT_MULTI_SIZE = sizeof(ExtendedMultiReport);
static_assert(sizeof(HIDReport) == 7, "HIDReport wire layout changed");
static_assert(sizeof(MotionReport) == 12, "MotionReport wire layout changed");
static_assert(sizeof(ExtendedHIDReport) == 24, "ExtendedHIDReport wire layout changed");
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
}


#include "../../server/rpi/include/sha256.h"

static constexpr uint8_t EXT_PAD_PRESENT = 0x01;

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

static int16_t read_le16(const uint8_t* p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void set_pad_present_flag(ns::ExtendedHIDReport& r, bool present) {
    if (present) r.flags |= EXT_PAD_PRESENT;
    else         r.flags &= (uint8_t)~EXT_PAD_PRESENT;
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
        // Safe to call more than once. If startup happened before the pad was
        // plugged in, a later Connect/device-change can rescan silently.
        if (running.load() && !devices.empty()) return;
        running.store(true);
        if (!devices.empty()) return;
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

            dev->thread = std::thread([this, d = dev.get()] { read_loop(d); });
            devices.push_back(std::move(dev));
            slot++;
        }
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
            uint8_t out[32] = {};
            out[0] = 0x05;
            out[1] = 0xFF;
            out[4] = high;
            out[5] = low;
            DWORD written = 0;
            WriteFile(d->handle, out, sizeof(out), &written, nullptr);
            HidD_SetOutputReport(d->handle, out, sizeof(out));
        } else if (is_dualsense(d->info.vid, d->info.pid)) {
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
            if (thread.joinable()) thread.detach();
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
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
        if (len >= 25 && b[0] == 0x01) { o = 0; motion_o = 13; }
        else if (len >= 78 && b[0] == 0x11) { o = 2; motion_o = 15; }
        else return false;

        r.lx = b[o + 1]; r.ly = b[o + 2]; r.rx = b[o + 3]; r.ry = b[o + 4];
        uint8_t btn0 = b[o + 5], btn1 = b[o + 6], btn2 = b[o + 7];
        sony_dpad_to_hat(btn0, r);
        if (btn0 & 0x10) r.buttons |= ns::BTN_Y;
        if (btn0 & 0x20) r.buttons |= ns::BTN_B;
        if (btn0 & 0x40) r.buttons |= ns::BTN_A;
        if (btn0 & 0x80) r.buttons |= ns::BTN_X;
        if (btn1 & 0x01) r.buttons |= ns::BTN_L;
        if (btn1 & 0x02) r.buttons |= ns::BTN_R;
        if ((btn1 & 0x04) || b[o + 8] > 128) r.buttons |= ns::BTN_ZL;
        if ((btn1 & 0x08) || b[o + 9] > 128) r.buttons |= ns::BTN_ZR;
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

    static bool parse_dualsense(const uint8_t* b, DWORD len, ns::HIDReport& r, ns::MotionReport& m, bool& has_motion) {
        r.reset(); m.reset(); has_motion = false;
        int o = -1, motion_o = -1;
        if (len >= 40 && b[0] == 0x01) { o = 0; motion_o = 16; }
        else if (len >= 78 && b[0] == 0x31) { o = 1; motion_o = 17; }
        else return false;

        r.lx = b[o + 1]; r.ly = b[o + 2]; r.rx = b[o + 3]; r.ry = b[o + 4];
        uint8_t l2 = b[o + 5], r2 = b[o + 6];
        uint8_t btn0 = b[o + 8], btn1 = b[o + 9], btn2 = b[o + 10];
        sony_dpad_to_hat(btn0, r);
        if (btn0 & 0x10) r.buttons |= ns::BTN_Y;
        if (btn0 & 0x20) r.buttons |= ns::BTN_B;
        if (btn0 & 0x40) r.buttons |= ns::BTN_A;
        if (btn0 & 0x80) r.buttons |= ns::BTN_X;
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
                DWORD err = GetLastError();
                if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_INVALID_HANDLE || err == ERROR_OPERATION_ABORTED) {
                    std::lock_guard<std::mutex> lk(mtx);
                    states[d->slot].connected = false;
                    break;
                }
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

static RawHidManager g_rawHid;

class RumbleManager {
public:
    void apply_packet(const ns::RumblePacket& rp,
                      const int xinput_for_slot[4],
                      const int raw_for_slot[4]) {
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
                   xinput_for_slot[slot], raw_for_slot[slot]);
    }

    void update_timeouts(const int xinput_for_slot[4], const int raw_for_slot[4]) {
        uint64_t now = ns::now_us();
        for (int i = 0; i < 4; ++i) {
            if (states[i].until_us != 0 && now > states[i].until_us) {
                states[i].until_us = 0;
                states[i].low = states[i].high = 0;
                set_output(i, 0, 0, xinput_for_slot[i], raw_for_slot[i]);
            }
        }
    }

    void stop_all() {
        for (int i = 0; i < 4; ++i) {
            if (states[i].last_xinput >= 0) {
                XINPUT_VIBRATION z{};
                XInputSetState((DWORD)states[i].last_xinput, &z);
            }
            if (states[i].last_raw >= 0)
                g_rawHid.set_rumble(states[i].last_raw, 0, 0);
            states[i] = SlotState{};
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

    void stop_previous_if_moved(int slot, int xinput_idx, int raw_idx) {
        if (states[slot].last_xinput != -1 && states[slot].last_xinput != xinput_idx) {
            XINPUT_VIBRATION z{};
            XInputSetState((DWORD)states[slot].last_xinput, &z);
        }
        if (states[slot].last_raw != -1 && states[slot].last_raw != raw_idx)
            g_rawHid.set_rumble(states[slot].last_raw, 0, 0);
    }

    void set_output(int slot, uint8_t low, uint8_t high, int xinput_idx, int raw_idx) {
        stop_previous_if_moved(slot, xinput_idx, raw_idx);
        if (xinput_idx >= 0 && xinput_idx < 4) {
            XINPUT_VIBRATION vib{};
            vib.wLeftMotorSpeed = motor_word(low);
            vib.wRightMotorSpeed = motor_word(high);
            XInputSetState((DWORD)xinput_idx, &vib);
        }
        if (raw_idx >= 0)
            g_rawHid.set_rumble(raw_idx, low, high);
        states[slot].last_xinput = xinput_idx;
        states[slot].last_raw = raw_idx;
    }
};

static void pump_udp_rumble(SOCKET sock,
                            RumbleManager& rumble,
                            const int xinput_for_slot[4],
                            const int raw_for_slot[4]) {
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
        if (n == (int)sizeof(ns::RumblePacket)) {
            ns::RumblePacket rp{};
            memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::RUMBLE_MAGIC)
                rumble.apply_packet(rp, xinput_for_slot, raw_for_slot);
        }
    }
}

// ── Throttled XInput polling (4-Player) ──
static uint64_t g_last_check_us[4] = {0, 0, 0, 0};
static bool     g_is_connected[4]  = {false, false, false, false};

static uint8_t apply_deadzone(SHORT val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    else                 scaled = 128 - ((abs(val) - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

static ns::HIDReport map_xinput_to_switch(const XINPUT_GAMEPAD& pad) {
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

static void fetch_pad_throttled(DWORD index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    uint64_t now = ns::now_us();
    if (!g_is_connected[index] && (now - g_last_check_us[index] < 1'000'000)) {
        conn = false; return;
    }
    XINPUT_STATE state; ZeroMemory(&state, sizeof(XINPUT_STATE));
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
        g_is_connected[index] = false;
        g_last_check_us[index] = now;
        conn = false; return;
    }
    g_is_connected[index] = true; conn = true;
    rep = map_xinput_to_switch(state.Gamepad);
}

// ── Global UI state ──
static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;
static HWND g_hIpEdit = nullptr;
static HWND g_hConnectBtn = nullptr;
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

// ── Control IDs ──
enum { IDC_IP = 101, IDC_CONNECT, IDC_KEYBOARD_COMBO = 110, IDC_BINDINGS_BTN = 111, IDC_EDITOR_CHANGE = 200, IDC_EDITOR_SETUP = 400, IDC_EDITOR_RESET = 500, IDC_EDITOR_KEY_START = 300 };

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

// ── Sender thread (4-Player) ──
static void SenderThread() {
    g_rawHid.start();

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) return;

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

    g_sock = sock;
    g_senderRunning = true;
    uint32_t seq = 0;
    RumbleManager rumble;

    while (g_senderRunning) {
        ns::HIDReport logicalReports[4];
        ns::MotionReport logicalMotion[4];
        bool present[4] = {false, false, false, false};
        bool hasMotion[4] = {false, false, false, false};
        int xinputForSlot[4] = {-1, -1, -1, -1};
        int rawForSlot[4] = {-1, -1, -1, -1};
        for (int i = 0; i < 4; ++i) {
            logicalReports[i].reset();
            logicalMotion[i].reset();
        }

        int activeCount = 0;

        // Raw HID first: DS4, DualSense, and Switch Pro get motion/gyro without
        // DS4Windows, Steam Input, or any helper app.
        auto raw = g_rawHid.snapshot();
        for (int i = 0; i < 4; ++i) {
            if (!raw[i].connected) continue;
            logicalReports[i] = raw[i].input;
            present[i] = true;
            rawForSlot[i] = i;
            if (raw[i].has_motion) {
                logicalMotion[i] = raw[i].motion;
                hasMotion[i] = true;
            }
            activeCount++;
        }

        // XInput fallback / Xbox controllers. If raw HID already claimed a slot,
        // put XInput in the next free slot instead of overwriting gyro-capable input.
        bool c1=false, c2=false, c3=false, c4=false;
        for (DWORD i = 0; i < 4; ++i) {
            ns::HIDReport temp{};
            bool conn = false;
            fetch_pad_throttled(i, temp, conn);
            if (!conn) continue;

            int target = (int)i;
            if (present[target]) {
                target = -1;
                for (int s = 0; s < 4; ++s) {
                    if (!present[s]) { target = s; break; }
                }
            }
            if (target < 0) continue;

            logicalReports[target] = temp;
            present[target] = true;
            xinputForSlot[target] = (int)i;
            activeCount++;
            if (target == 0) c1 = true;
            else if (target == 1) c2 = true;
            else if (target == 2) c3 = true;
            else if (target == 3) c4 = true;
        }

        // Keyboard modes are kept exactly as before, only now they fill the new
        // extended packet format underneath the unchanged UI.
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
                    xinputForSlot[target] = xinputForSlot[0];
                    rawForSlot[target] = rawForSlot[0];
                    activeCount++;
                }
            }
            logicalReports[0].reset();
            logicalMotion[0].reset();
            apply_keyboard_to_report(logicalReports[0], false);
            present[0] = true;
            hasMotion[0] = false;
            xinputForSlot[0] = -1;
            rawForSlot[0] = -1;
            activeCount = std::max(activeCount, 1);
        } else if (km == KB_OVERRIDE) {
            apply_keyboard_to_report(logicalReports[0], true);
            present[0] = true;
            activeCount = std::max(activeCount, 1);
        }

        ExtendedUdpPacket pkt{};
        memset(&pkt, 0, sizeof(pkt));
        pkt.magic = ns::PROTO_MAGIC;
        pkt.version = ns::PROTO_VERSION;
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

        pump_udp_rumble(sock, rumble, xinputForSlot, rawForSlot);
        rumble.update_timeouts(xinputForSlot, rawForSlot);

        if (activeCount > 0) std::this_thread::sleep_for(std::chrono::milliseconds(4));
        else std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    rumble.stop_all();
    closesocket(sock);
    g_sock = INVALID_SOCKET;
}

// ── Update P1-P4 status display ──
static void UpdateControllerStatus() {
    wchar_t buf[64];
    HWND hText[4] = { g_hP1Text, g_hP2Text, g_hP3Text, g_hP4Text };
    int km = g_keyboardMode.load();
    for (DWORD i = 0; i < 4; i++) {
        if (i == 0 && km != KB_OFF) {
            if (km == KB_SINGLE) {
                swprintf(buf, 64, L"P1: Keyboard");
            } else {
                XINPUT_CAPABILITIES caps{};
                bool present = (XInputGetCapabilities(0, 0, &caps) == ERROR_SUCCESS);
                bool conn = g_connected && g_is_connected[0];
                if (conn || present) {
                    swprintf(buf, 64, L"P1: %s / Keyboard", conn ? L"Connected" : L"Idle");
                } else {
                    swprintf(buf, 64, L"P1: Idle / Keyboard");
                }
            }
            SetWindowText(hText[i], buf);
            continue;
        }
        XINPUT_CAPABILITIES caps{};
        bool present = (XInputGetCapabilities(i, 0, &caps) == ERROR_SUCCESS);
        if (g_connected) {
            swprintf(buf, 64, L"P%d: %s", i + 1, g_is_connected[i] ? L"Connected" : L"Idle");
        } else {
            swprintf(buf, 64, L"P%d: %s", i + 1, present ? L"Available" : L"Not connected");
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

    for (int i = 0; i < 4; i++) {
        g_is_connected[i] = false;
        g_last_check_us[i] = 0;
    }

    g_connected = true;

    if (g_senderThread.joinable()) g_senderThread.join();
    g_senderThread = std::thread(SenderThread);

    SetWindowText(g_hConnectBtn, L"Disconnect");
    EnableWindow(g_hIpEdit, FALSE);
    EnableWindow(g_hKeyboardCombo, FALSE);
    EnableWindow(g_hBindingsBtn, FALSE);

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
                g_rawHid.start();
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
    g_hInst = hInst;

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_rawHid.start();

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

    RECT rc{0, 0, 410, 315};
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
