/// gamepad_mac.mm  —  macOS frontend for the Switch wireless gamepad bridge
///
/// Uses Apple's GameController framework for controller input.
/// Natively supports Xbox, PlayStation, MFi, and Switch Pro Controllers
/// (via Bluetooth or USB, depending on macOS version).
/// Networking uses BSD sockets — identical API to the Linux version.
///
/// Build:
///   clang++ -std=c++17 -ObjC++ \
///           -framework GameController -framework Foundation \
///           gamepad_mac.mm -o gamepad_mac
///
/// Usage:
///   ./gamepad_mac <RASPBERRY_PI_IP>
///
/// Note: On macOS 10.15+, Bluetooth controllers may require the
/// "Input Monitoring" permission under System Settings → Privacy & Security.

#ifndef __APPLE__
#  error "gamepad_mac.mm is macOS-only. Use gamepad_linux.cpp or gamepad_win.cpp for other platforms."
#endif

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <iostream>
#include <chrono>
#include <cstdint>
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
#include "sha256.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Protocol namespace  (identical to Windows / Linux versions)
// ─────────────────────────────────────────────────────────────────────────────
namespace ns {

static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;  // 'NSWC' magic number for packet validation
static constexpr uint8_t  PROTO_VERSION = 1;             // Protocol version for compatibility checking
static constexpr uint16_t DEFAULT_PORT  = 7331;          // UDP port for sending gamepad data

/// Nintendo Switch Pro Controller button bitmask layout
enum Button : uint16_t {
    BTN_Y=1<<0, BTN_B=1<<1, BTN_A=1<<2, BTN_X=1<<3,
    BTN_L=1<<4, BTN_R=1<<5, BTN_ZL=1<<6, BTN_ZR=1<<7,
    BTN_MINUS=1<<8, BTN_PLUS=1<<9, BTN_LSTICK=1<<10, BTN_RSTICK=1<<11,
    BTN_HOME=1<<12, BTN_CAPTURE=1<<13,
};

/// D-pad hat switch directions
enum Hat : uint8_t {
    HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3,
    HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8,
};

/// 8-byte packed HID report — layout must match the USB gadget descriptor on the Pi
#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0;
    uint8_t  hat     = HAT_NEUTRAL;
    uint8_t  lx      = 128;  // Left stick X  (0-255, 128 = center)
    uint8_t  ly      = 128;  // Left stick Y  (0-255, 128 = center)
    uint8_t  rx      = 128;  // Right stick X (0-255, 128 = center)
    uint8_t  ry      = 128;  // Right stick Y (0-255, 128 = center)
    uint8_t  vendor  = 0;
    void reset() noexcept { buttons = 0; hat = HAT_NEUTRAL; lx = ly = rx = ry = 128; vendor = 0; }
};

enum Flags : uint8_t {
    FLAG_NONE     = 0x00,
    FLAG_RESET    = 0x01,
    FLAG_AUTOFIRE = 0x02,
};

/// HMAC authentication tag (truncated HMAC-SHA256)
static constexpr size_t HMAC_TAG_SIZE = 16;

/// ~44-byte UDP wire packet sent to the Raspberry Pi backend (with HMAC tag)
struct Packet {
    uint32_t  magic;
    uint8_t   version;
    uint8_t   flags;
    uint16_t  autofire_mask;
    uint32_t  seq;           // Monotonic sequence counter
    uint64_t  ts_us;         // steady_clock timestamp in microseconds
    HIDReport report;
    uint8_t   hmac[HMAC_TAG_SIZE];
};
#pragma pack(pop)

static constexpr size_t PACKET_SIZE      = sizeof(Packet);
static constexpr size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;

/// Monotonic high-precision timestamp in microseconds
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace ns

// ─────────────────────────────────────────────────────────────────────────────
//  Shared gamepad state
// ─────────────────────────────────────────────────────────────────────────────

/// Thread-safe container for controller input state.
/// GCController value-change handlers (main thread) write here;
/// the UDP sender thread reads with relaxed atomics.
struct GamepadState {
    // Face buttons — GCController uses Xbox naming; remapped to Switch in map_gc_to_switch()
    std::atomic<bool>  btn_a{false}, btn_b{false}, btn_x{false}, btn_y{false};

    // Shoulder bumpers
    std::atomic<bool>  btn_l{false}, btn_r{false};

    // Analog triggers (0.0 = released, 1.0 = fully pressed)
    std::atomic<float> zl{0.0f}, zr{0.0f};

    // Menu buttons:
    //   buttonMenu    → always present (Options on PS / Start on Xbox / + on Switch) → Switch Plus
    //   buttonOptions → optional      (Share on PS  / Back on Xbox  / − on Switch) → Switch Minus
    std::atomic<bool>  btn_menu{false}, btn_options{false};

    // Thumbstick clicks (absent on some controllers — guarded with nil check in attach_handlers)
    std::atomic<bool>  btn_lstick{false}, btn_rstick{false};

    // D-pad cardinal directions
    std::atomic<bool>  dpad_up{false}, dpad_down{false}, dpad_left{false}, dpad_right{false};

    // Analog sticks — GCController convention: +X = right, +Y = up, range -1.0 .. 1.0
    std::atomic<float> lx{0.0f}, ly{0.0f}, rx{0.0f}, ry{0.0f};
};

// ─────────────────────────────────────────────────────────────────────────────
//  Signal handling
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ─────────────────────────────────────────────────────────────────────────────
//  Axis conversion
// ─────────────────────────────────────────────────────────────────────────────

/// Convert a GCController float axis (-1.0..1.0) to an HID unsigned byte (0-255) with deadzone.
/// @param val     Axis value from GCController (-1.0 = min, 0.0 = center, 1.0 = max)
/// @param invert  Flip the output direction.
///                Needed for Y axes: GCController +Y is up, but Switch HID byte 0 means up.
/// @param dz      Deadzone fraction — values within ±dz of center are treated as neutral (default 15%)
/// @return        Normalized HID byte (0-255, 128 = neutral)
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
/// Handlers run on the main thread and atomically update the shared GamepadState.
static void attach_handlers(GCExtendedGamepad* gp, GamepadState* st) {
    // ── Face buttons ──────────────────────────────────────────────────────────
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

    // ── Shoulder bumpers ──────────────────────────────────────────────────────
    gp.leftShoulder.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->btn_l.store((bool)p, std::memory_order_relaxed);
    };
    gp.rightShoulder.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->btn_r.store((bool)p, std::memory_order_relaxed);
    };

    // ── Analog triggers ───────────────────────────────────────────────────────
    gp.leftTrigger.valueChangedHandler = ^(GCControllerButtonInput*, float v, BOOL) {
        st->zl.store(v, std::memory_order_relaxed);
    };
    gp.rightTrigger.valueChangedHandler = ^(GCControllerButtonInput*, float v, BOOL) {
        st->zr.store(v, std::memory_order_relaxed);
    };

    // ── Menu buttons ──────────────────────────────────────────────────────────
    gp.buttonMenu.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
        st->btn_menu.store((bool)p, std::memory_order_relaxed);
    };
    if (gp.buttonOptions) {  // Optional — may be absent on some controllers
        gp.buttonOptions.valueChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) {
            st->btn_options.store((bool)p, std::memory_order_relaxed);
        };
    }

    // ── Thumbstick clicks (optional on many controllers) ──────────────────────
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

    // ── D-pad ─────────────────────────────────────────────────────────────────
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

    // ── Analog sticks ─────────────────────────────────────────────────────────
    gp.leftThumbstick.xAxis.valueChangedHandler = ^(GCControllerAxisInput*, float v) {
        st->lx.store(v, std::memory_order_relaxed);
    };
    gp.leftThumbstick.yAxis.valueChangedHandler = ^(GCControllerAxisInput*, float v) {
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
/// Button cross-mapping mirrors the Windows XInput version.
static ns::HIDReport map_gc_to_switch(const GamepadState& st) {
    ns::HIDReport r;
    r.reset();

    // ── Face buttons ──────────────────────────────────────────────────────────
    // GCController follows Xbox naming (position-based, not label-based):
    //   buttonA (bottom) → Switch B  |  buttonB (right) → Switch A
    //   buttonX (left)   → Switch Y  |  buttonY (top)   → Switch X
    if (st.btn_a.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_B;
    if (st.btn_b.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_A;
    if (st.btn_x.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_Y;
    if (st.btn_y.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_X;

    // ── Shoulder bumpers ──────────────────────────────────────────────────────
    if (st.btn_l.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_L;
    if (st.btn_r.load(std::memory_order_relaxed)) r.buttons |= ns::BTN_R;

    // ── Triggers — 50% press threshold for digital output ─────────────────────
    if (st.zl.load(std::memory_order_relaxed) > 0.5f) r.buttons |= ns::BTN_ZL;
    if (st.zr.load(std::memory_order_relaxed) > 0.5f) r.buttons |= ns::BTN_ZR;

    // ── Menu buttons ──────────────────────────────────────────────────────────
    bool plus  = st.btn_menu.load(std::memory_order_relaxed);
    bool minus = st.btn_options.load(std::memory_order_relaxed);
    if (plus)  r.buttons |= ns::BTN_PLUS;
    if (minus) r.buttons |= ns::BTN_MINUS;

    // ── Thumbstick clicks ─────────────────────────────────────────────────────
    bool ls = st.btn_lstick.load(std::memory_order_relaxed);
    bool rs = st.btn_rstick.load(std::memory_order_relaxed);
    if (ls) r.buttons |= ns::BTN_LSTICK;
    if (rs) r.buttons |= ns::BTN_RSTICK;

    // Special: both sticks simultaneously = HOME (removes individual stick bits)
    if (ls && rs) {
        r.buttons |= ns::BTN_HOME;
        r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
    }

    // Special: Menu + Options simultaneously = CAPTURE (removes individual menu bits)
    if (plus && minus) {
        r.buttons |= ns::BTN_CAPTURE;
        r.buttons &= ~(ns::BTN_PLUS | ns::BTN_MINUS);
    }

    // ── D-pad → hat switch ────────────────────────────────────────────────────
    bool up    = st.dpad_up.load(std::memory_order_relaxed);
    bool down  = st.dpad_down.load(std::memory_order_relaxed);
    bool left  = st.dpad_left.load(std::memory_order_relaxed);
    bool right = st.dpad_right.load(std::memory_order_relaxed);

    if      (up   && right) r.hat = ns::HAT_NE;
    else if (up   && left)  r.hat = ns::HAT_NW;
    else if (down && right) r.hat = ns::HAT_SE;
    else if (down && left)  r.hat = ns::HAT_SW;
    else if (up)            r.hat = ns::HAT_N;
    else if (down)          r.hat = ns::HAT_S;
    else if (left)          r.hat = ns::HAT_W;
    else if (right)         r.hat = ns::HAT_E;

    // ── Analog sticks ─────────────────────────────────────────────────────────
    // GCController: +Y is up; Switch HID byte 0 = up, 255 = down → invert Y
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
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP>\n";
        return 1;
    }
    std::string host = argv[1];

    // Derive HMAC key from compiled-in default secret (always active)
    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    // Install signal handlers for graceful Ctrl+C / SIGTERM shutdown
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    // ── UDP socket ────────────────────────────────────────────────────────────
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create UDP socket.\n";
        return 1;
    }

    // Resolve the Raspberry Pi address (accepts both raw IPs and hostnames)
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", ns::DEFAULT_PORT);
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        std::cerr << "Cannot resolve address: " << host << "\n";
        close(sock);
        return 1;
    }
    sockaddr_in dest{};
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    std::cout << "Streaming to " << host << ":" << ns::DEFAULT_PORT
              << " — press Ctrl+C to stop.\n";

    // Elevate process priority for minimal input latency (requires sudo or appropriate entitlement)
    setpriority(PRIO_PROCESS, 0, -20);

    // ── Shared state + 500 Hz sender thread ───────────────────────────────────
    GamepadState state;
    // Raw pointer used inside Objective-C blocks: blocks capture locals as const copies,
    // which would make &state a 'const GamepadState*' and break attach_handlers.
    // Capturing a pointer by value avoids the const issue and the deleted copy constructor.
    GamepadState* statePtr = &state;
    std::atomic<bool> running{true};

    // Dedicated UDP sender thread — busy-waits for precise 2 ms interval (same as Linux version)
    std::thread sender([&]() {
        uint32_t seq       = 0;
        auto     next_tick = std::chrono::steady_clock::now();

        while (running.load(std::memory_order_relaxed)) {
            while (std::chrono::steady_clock::now() < next_tick)
                std::atomic_thread_fence(std::memory_order_relaxed);

            ns::Packet pkt{};
            pkt.magic         = ns::PROTO_MAGIC;
            pkt.version       = ns::PROTO_VERSION;
            pkt.flags         = ns::FLAG_NONE;
            pkt.autofire_mask = 0;
            pkt.seq           = seq++;
            pkt.ts_us         = ns::now_us();
            pkt.report        = map_gc_to_switch(state);
            {
                uint8_t full_hmac[32];
                hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
            }

            sendto(sock, &pkt, ns::PACKET_SIZE, 0,
                   (struct sockaddr*)&dest, sizeof(dest));

            next_tick += std::chrono::milliseconds(2);
        }
    });

    // ── GameController notification setup (runs on main thread) ───────────────
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

    [nc addObserverForName:GCControllerDidConnectNotification
                   object:nil
                    queue:[NSOperationQueue mainQueue]
               usingBlock:^(NSNotification* note) {
        GCController* ctrl = (GCController*)note.object;
        if (!ctrl.extendedGamepad) {
            std::cout << "Controller connected, but extended gamepad profile unavailable — skipping.\n";
            return;
        }
        NSString* name = ctrl.vendorName ?: @"Unknown Controller";
        std::cout << "Controller connected: " << name.UTF8String << "\n";
        attach_handlers(ctrl.extendedGamepad, statePtr);
    }];

    [nc addObserverForName:GCControllerDidDisconnectNotification
                   object:nil
                    queue:[NSOperationQueue mainQueue]
               usingBlock:^(NSNotification* note) {
        GCController* ctrl = (GCController*)note.object;
        NSString* name = ctrl.vendorName ?: @"Unknown Controller";
        std::cout << "Controller disconnected: " << name.UTF8String << "\n";
        // Sender thread keeps running and will transmit neutral reports until reconnected
    }];

    // Handle controllers that were already connected when the program launched
    for (GCController* ctrl in [GCController controllers]) {
        if (ctrl.extendedGamepad) {
            NSString* name = ctrl.vendorName ?: @"Unknown Controller";
            std::cout << "Found existing controller: " << name.UTF8String << "\n";
            attach_handlers(ctrl.extendedGamepad, statePtr);
            break;  // Use only the first extended-gamepad-capable controller
        }
    }

    if ([[GCController controllers] count] == 0)
        std::cout << "No controller detected — waiting for one to be connected...\n";

    // ── Main NSRunLoop ─────────────────────────────────────────────────────────
    // GCController notifications require an active NSRunLoop on the main thread.
    // We advance it in 100 ms slices so Ctrl+C is handled promptly.
    while (g_running.load(std::memory_order_relaxed))
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];

    // ── Graceful shutdown ──────────────────────────────────────────────────────
    std::cout << "\nShutting down...\n";
    running.store(false, std::memory_order_relaxed);
    if (sender.joinable()) sender.join();
    close(sock);
    return 0;
}
