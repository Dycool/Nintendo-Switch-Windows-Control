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
///           ns-gamepad.mm -o ns-gamepad
///
/// Usage:
///   ./ns-gamepad <RASPBERRY_PI_IP[:PORT]>

#ifndef __APPLE__
#  error "ns-gamepad.mm is macOS-only."
#endif

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

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
};

static constexpr int MAX_SLOTS = 4;
static GamepadState  g_states[MAX_SLOTS];
static GCController* g_controllers[MAX_SLOTS] = {};

// FIX 1: Separate atomic flags so the sender thread can safely check slot
// occupancy without touching the ObjC pointer (which is not atomic-safe with ARC).
// Only the main thread reads/writes g_controllers[]; the sender thread only
// reads g_slot_active[].
static std::atomic<bool> g_slot_active[MAX_SLOTS] = {};
static int keyboard_mode = 0; // 0=off, 1=single, 2=override

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

// ─────────────────────────────────────────────────────────────────────────────
//  GameController integration
// ─────────────────────────────────────────────────────────────────────────────

/// Register value-change handlers for every input on a GCExtendedGamepad.
/// Handlers run on the main thread and atomically update the given GamepadState.
static void attach_handlers(GCExtendedGamepad* gp, GamepadState* st) {
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

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-k") == 0) {
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
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]]\n";
        std::cerr << "  -k  Enable keyboard mode (default: single)\n";
        return 1;
    }

    KeyBindings kb;
    if (keyboard_mode) {
        kb.load_or_create();
        kb.mode = keyboard_mode;
        std::cout << "Keyboard mode enabled (" << (keyboard_mode == 1 ? "single" : "override") << ") - ";
        std::cout << (keyboard_mode == 1 ? "replaces" : "augments") << " Player 1\n";
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

    std::cout << "Started... Press Ctrl+C to stop\n";

    setpriority(PRIO_PROCESS, 0, -20);

    // ── Dedicated UDP sender thread ───────────────────────────────────────────
    std::thread sender([&]() {
        uint32_t seq = 0;

        while (g_running.load(std::memory_order_relaxed)) {
            ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet));
            pkt.magic   = ns::PROTO_MAGIC;
            pkt.version = ns::PROTO_VERSION;
            pkt.flags   = ns::FLAG_NONE;
            pkt.seq     = seq++;
            pkt.ts_us   = ns::now_us();

            pkt.report.reset();

            ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
            int active_count = 0;

            bool c1 = false, c2 = false, c3 = false, c4 = false;
            for (int i = 0; i < MAX_SLOTS; ++i) {
                if (!g_slot_active[i].load(std::memory_order_relaxed)) continue;
                *out_reports[i] = map_gc_to_switch(g_states[i]);
                active_count++;
                if (i == 0) c1 = true;
                else if (i == 1) c2 = true;
                else if (i == 2) c3 = true;
                else if (i == 3) c4 = true;
            }

            // Keyboard overrides Player 1
            if (kb.mode == 1) {
                if (c1) {
                    if (!c2) { *out_reports[1] = *out_reports[0]; c2 = true; active_count++; }
                    else if (!c3) { *out_reports[2] = *out_reports[0]; c3 = true; active_count++; }
                    else if (!c4) { *out_reports[3] = *out_reports[0]; c4 = true; active_count++; }
                }
                out_reports[0]->reset();
                kb.apply(*out_reports[0]);
                active_count = std::max(active_count, 1);
            } else if (kb.mode == 2) {
                kb.apply(*out_reports[0]);
                active_count = std::max(active_count, 1);
            }

            {
                uint8_t full_hmac[32];
                hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
            }

            sendto(sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));

            // FIX 3: Sleep instead of busy-waiting so we don't burn a full CPU core.
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
                g_slot_active[i].store(true, std::memory_order_relaxed);
                NSString* name = ctrl.vendorName ?: @"Unknown Controller";
                int display_slot = i + 1;
                if (keyboard_mode == 1 && i == 0) {
                    int free_idx = 1;
                    while (free_idx < MAX_SLOTS && g_controllers[free_idx]) free_idx++;
                    display_slot = free_idx + 1;
                }
                std::cout << "Mapped '" << name.UTF8String << "' to local slot P" << display_slot << "\n";
                attach_handlers(ctrl.extendedGamepad, &g_states[i]);
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
                g_slot_active[i].store(true, std::memory_order_relaxed);
                NSString* name = ctrl.vendorName ?: @"Unknown Controller";
                int display_slot = i + 1;
                if (keyboard_mode == 1 && i == 0) {
                    int free_idx = 1;
                    while (free_idx < MAX_SLOTS && g_controllers[free_idx]) free_idx++;
                    display_slot = free_idx + 1;
                }
                std::cout << "Mapped '" << name.UTF8String << "' to local slot P" << display_slot << "\n";
                attach_handlers(ctrl.extendedGamepad, &g_states[i]);
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
    close(sock);
    close(lock_fd);
    unlink("/tmp/ns-gamepad.lock");
    return 0;
}
