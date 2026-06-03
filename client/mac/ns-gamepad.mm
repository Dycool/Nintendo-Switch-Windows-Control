/// ns-gamepad.mm  —  macOS frontend for the Switch wireless gamepad bridge
///
/// Uses Apple's GameController framework for controller input.
/// Natively supports Xbox, PlayStation, MFi, and Switch Pro Controllers
/// (via Bluetooth or USB, depending on macOS version).
///
/// Build:
///   clang++ -std=c++17 -ObjC++ \
///           -framework GameController -framework Foundation \
///           ns-gamepad.mm -o ns-gamepad
///
/// Usage:
///   ./ns-gamepad <RASPBERRY_PI_IP> [-p PORT]

#ifndef __APPLE__
#  error "gamepad_mac.mm is macOS-only."
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

// BSD networking headers (macOS is BSD-derived; same API as Linux)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/resource.h>
#include "../../server/rpi/include/sha256.h"

// Import external protocol structures (Version 4 with MultiReport)
#include "../../server/rpi/include/protocol.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Shared gamepad state
// ─────────────────────────────────────────────────────────────────────────────

/// Thread-safe container for controller input state.
/// GCController value-change handlers (main thread) write here;
/// the UDP sender thread reads with relaxed atomics.
struct GamepadState {
    std::atomic<bool>  active{false}; // True if a physical controller is mapped here

    std::atomic<bool>  btn_a{false}, btn_b{false}, btn_x{false}, btn_y{false};
    std::atomic<bool>  btn_l{false}, btn_r{false};
    std::atomic<float> zl{0.0f}, zr{0.0f};
    std::atomic<bool>  btn_menu{false}, btn_options{false};
    std::atomic<bool>  btn_lstick{false}, btn_rstick{false};
    std::atomic<bool>  dpad_up{false}, dpad_down{false}, dpad_left{false}, dpad_right{false};
    std::atomic<float> lx{0.0f}, ly{0.0f}, rx{0.0f}, ry{0.0f};

    void clear_inputs() {
        btn_a = false; btn_b = false; btn_x = false; btn_y = false;
        btn_l = false; btn_r = false; zl = 0.0f; zr = 0.0f;
        btn_menu = false; btn_options = false;
        btn_lstick = false; btn_rstick = false;
        dpad_up = false; dpad_down = false; dpad_left = false; dpad_right = false;
        lx = 0.0f; ly = 0.0f; rx = 0.0f; ry = 0.0f;
    }
};

// Global tracking for up to 4 local controllers
static GamepadState  g_states[4];
static GCController* g_controllers[4] = {nullptr, nullptr, nullptr, nullptr};

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
static void attach_handlers(GCExtendedGamepad* gp, GamepadState* st) {
    st->clear_inputs(); // Wipe any ghost states

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
}

/// Translate GameController state into a Switch Pro Controller HID report.
static ns::HIDReport map_gc_to_switch(const GamepadState& st) {
    ns::HIDReport r;
    r.reset();

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
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    std::string host = ""; 
    uint16_t port = ns::DEFAULT_PORT;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" && i+1 < argc) port = (uint16_t)std::atoi(argv[++i]);
        else if (host.empty()) host = arg;
    }

    if (host.empty()) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP> [-p PORT]\n";
        return 1;
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
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);
    
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        std::cerr << "Cannot resolve address: " << host << "\n";
        close(sock); return 1;
    }
    
    sockaddr_in dest{};
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    std::cout << "Started as a Multi-Client Node... Connect controllers and enjoy!\n";

    // Elevate process priority for minimal input latency
    setpriority(PRIO_PROCESS, 0, -20);

    // ── Dedicated UDP sender thread ───────────────────────────────────────────
    std::thread sender([&]() {
        uint32_t seq = 0;
        auto next_tick = std::chrono::steady_clock::now();

        while (g_running.load(std::memory_order_relaxed)) {
            while (std::chrono::steady_clock::now() < next_tick)
                std::atomic_thread_fence(std::memory_order_relaxed);

            ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet)); 
            pkt.magic         = ns::PROTO_MAGIC;
            pkt.version       = ns::PROTO_VERSION;
            pkt.flags         = ns::FLAG_NONE;
            pkt.seq           = seq++;
            pkt.ts_us         = ns::now_us();
            
            pkt.report.reset(); // Hardware-neutral init

            ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
            int active_count = 0;

            // Pack only active controllers sequentially to the front of the packet
            for (int i = 0; i < 4; ++i) {
                if (g_states[i].active.load(std::memory_order_relaxed)) {
                    *out_reports[active_count] = map_gc_to_switch(g_states[i]);
                    active_count++;
                }
            }

            {
                uint8_t full_hmac[32];
                hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
            }

            sendto(sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));

            // Dynamic throttling
            if (active_count > 0) next_tick += std::chrono::milliseconds(2);
            else next_tick += std::chrono::milliseconds(500);
        }
    });

    // ── GameController notification setup (runs on main thread) ───────────────
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

    auto assign_controller = ^(GCController* ctrl) {
        if (!ctrl.extendedGamepad) return;
        
        // Find a free slot
        for (int i = 0; i < 4; ++i) {
            if (g_controllers[i] == nullptr) {
                g_controllers[i] = ctrl;
                g_states[i].active.store(true, std::memory_order_relaxed);
                NSString* name = ctrl.vendorName ?: @"Unknown Controller";
                std::cout << "Mapped '" << name.UTF8String << "' to local slot P" << (i + 1) << "\n";
                attach_handlers(ctrl.extendedGamepad, &g_states[i]);
                break;
            }
        }
    };

    [nc addObserverForName:GCControllerDidConnectNotification
                    object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
        GCController* ctrl = (GCController*)note.object;
        assign_controller(ctrl);
    }];

    [nc addObserverForName:GCControllerDidDisconnectNotification
                    object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
        GCController* ctrl = (GCController*)note.object;
        for (int i = 0; i < 4; ++i) {
            if (g_controllers[i] == ctrl) {
                g_controllers[i] = nullptr;
                g_states[i].active.store(false, std::memory_order_relaxed);
                g_states[i].clear_inputs(); // Wipe phantom inputs
                std::cout << "Local controller in slot P" << (i + 1) << " disconnected.\n";
                break;
            }
        }
    }];

    // Handle controllers that were already connected when the program launched
    for (GCController* ctrl in [GCController controllers]) {
        assign_controller(ctrl);
    }

    if ([GCController controllers].count == 0) {
        std::cout << "No controllers detected — waiting for connections...\n";
    }

    // ── Main NSRunLoop ─────────────────────────────────────────────────────────
    // GCController notifications require an active NSRunLoop on the main thread.
    // We advance it in 100 ms slices so Ctrl+C is handled promptly.
    while (g_running.load(std::memory_order_relaxed))
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];

    // ── Graceful shutdown ──────────────────────────────────────────────────────
    std::cout << "\nShutting down...\n";
    g_running.store(false, std::memory_order_relaxed);
    if (sender.joinable()) sender.join();
    close(sock);
    return 0;
}