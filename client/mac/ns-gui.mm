/// ns-gui.mm  —  macOS Cocoa GUI frontend for the Switch wireless gamepad bridge
///
/// Features Smart Discovery: Automatically detects up to 4 local controllers
/// via Apple's GameController framework and seamlessly packs them into the UDP stream.
///
/// Build:
///   clang++ -std=c++17 -ObjC++ -fobjc-arc \
///           -framework Cocoa -framework GameController \
///           -framework CoreHaptics \
///           ns-gui.mm -o ns-gui
///
/// Usage:
///   ./ns-gui

#ifndef __APPLE__
#  error "ns-gui.mm is macOS-only."
#endif

// On macOS 10.15+, Bluetooth controllers may require the
// "Input Monitoring" permission under System Settings → Privacy & Security.

#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>

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
#include <cmath>
#include <unordered_map>
#include <cerrno>
#include <fcntl.h>
#include <dispatch/dispatch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/resource.h>
#include <CoreGraphics/CoreGraphics.h>
#include "../../server/rpi/include/sha256.h"

// Import external protocol structures (Version 4 with MultiReport)
#include "../../server/rpi/include/protocol.hpp"

// ── Keyboard mode constants ──
enum KeyboardMode {
    KB_OFF = 0,
    KB_SINGLE = 1,
    KB_OVERRIDE = 2
};

static constexpr int MAX_SLOTS = 4;
static constexpr uint8_t EXT_PAD_PRESENT = 0x01;
static bool g_legacyUdp = false; // hidden fallback: NSPC_LEGACY_UDP=1

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

static GCController* g_rumbleControllers[MAX_SLOTS] = {};
static __strong CHHapticEngine* g_hapticEngines[MAX_SLOTS] = {};
static __strong id<CHHapticPatternPlayer> g_hapticPlayers[MAX_SLOTS] = {};


// ─────────────────────────────────────────────────────────────────────────────
//  Shared gamepad state
// ─────────────────────────────────────────────────────────────────────────────

/// Thread-safe container for controller input state.
struct GamepadState {
    std::atomic<bool>  btn_a{false}, btn_b{false}, btn_x{false}, btn_y{false};
    std::atomic<bool>  btn_l{false}, btn_r{false};
    std::atomic<float> zl{0.0f}, zr{0.0f};
    std::atomic<bool>  btn_menu{false}, btn_options{false};
    std::atomic<bool>  btn_lstick{false}, btn_rstick{false};
    std::atomic<bool>  dpad_up{false}, dpad_down{false}, dpad_left{false}, dpad_right{false};
    std::atomic<float> lx{0.0f}, ly{0.0f}, rx{0.0f}, ry{0.0f};

    // GameController motion values. Accel is in g-ish units, gyro is rad/s.
    std::atomic<bool>  has_motion{false};
    std::atomic<float> ax{0.0f}, ay{0.0f}, az{0.0f};
    std::atomic<float> gx{0.0f}, gy{0.0f}, gz{0.0f};

    void clear_inputs() {
        btn_a = false; btn_b = false; btn_x = false; btn_y = false;
        btn_l = false; btn_r = false; zl = 0.0f; zr = 0.0f;
        btn_menu = false; btn_options = false;
        btn_lstick = false; btn_rstick = false;
        dpad_up = false; dpad_down = false; dpad_left = false; dpad_right = false;
        lx = 0.0f; ly = 0.0f; rx = 0.0f; ry = 0.0f;
        has_motion = false;
        ax = 0.0f; ay = 0.0f; az = 0.0f;
        gx = 0.0f; gy = 0.0f; gz = 0.0f;
        // note: does not clear slotActive (managed separately)
    }

    GamepadState& operator=(const GamepadState& other) {
        if (this != &other) {
            btn_a.store(other.btn_a.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_b.store(other.btn_b.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_x.store(other.btn_x.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_y.store(other.btn_y.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_l.store(other.btn_l.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_r.store(other.btn_r.load(std::memory_order_relaxed), std::memory_order_relaxed);
            zl.store(other.zl.load(std::memory_order_relaxed), std::memory_order_relaxed);
            zr.store(other.zr.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_menu.store(other.btn_menu.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_options.store(other.btn_options.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_lstick.store(other.btn_lstick.load(std::memory_order_relaxed), std::memory_order_relaxed);
            btn_rstick.store(other.btn_rstick.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dpad_up.store(other.dpad_up.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dpad_down.store(other.dpad_down.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dpad_left.store(other.dpad_left.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dpad_right.store(other.dpad_right.load(std::memory_order_relaxed), std::memory_order_relaxed);
            lx.store(other.lx.load(std::memory_order_relaxed), std::memory_order_relaxed);
            ly.store(other.ly.load(std::memory_order_relaxed), std::memory_order_relaxed);
            rx.store(other.rx.load(std::memory_order_relaxed), std::memory_order_relaxed);
            ry.store(other.ry.load(std::memory_order_relaxed), std::memory_order_relaxed);
            has_motion.store(other.has_motion.load(std::memory_order_relaxed), std::memory_order_relaxed);
            ax.store(other.ax.load(std::memory_order_relaxed), std::memory_order_relaxed);
            ay.store(other.ay.load(std::memory_order_relaxed), std::memory_order_relaxed);
            az.store(other.az.load(std::memory_order_relaxed), std::memory_order_relaxed);
            gx.store(other.gx.load(std::memory_order_relaxed), std::memory_order_relaxed);
            gy.store(other.gy.load(std::memory_order_relaxed), std::memory_order_relaxed);
            gz.store(other.gz.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Axis conversion
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t float_to_byte(float val, bool invert = false, float dz = 0.15f) {
    if (std::abs(val) < dz) return 128;
    int scaled;
    float range = 1.0f - dz;
    if (val > 0.0f) scaled = 128 + (int)(((val - dz) / range) * 127.0f);
    else scaled = 128 - (int)(((-val - dz) / range) * 128.0f);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? 255 - scaled : scaled);
}

static int16_t clamp_i16_from_float(float v) {
    if (v < -32768.0f) return -32768;
    if (v >  32767.0f) return  32767;
    return (int16_t)std::lrintf(v);
}

static ns::MotionReport map_gc_motion_to_switch(const GamepadState& st) {
    ns::MotionReport m;
    m.reset();

    // Match the backend's virtual IMU scale roughly: accel around 0x1000 per g,
    // gyro kept conservative so real pads do not saturate the Switch sample.
    m.ax = clamp_i16_from_float(st.ax.load(std::memory_order_relaxed) * 4096.0f);
    m.ay = clamp_i16_from_float(st.ay.load(std::memory_order_relaxed) * 4096.0f);
    m.az = clamp_i16_from_float(st.az.load(std::memory_order_relaxed) * 4096.0f);
    m.gx = clamp_i16_from_float(st.gx.load(std::memory_order_relaxed) * 1000.0f);
    m.gy = clamp_i16_from_float(st.gy.load(std::memory_order_relaxed) * 1000.0f);
    m.gz = clamp_i16_from_float(st.gz.load(std::memory_order_relaxed) * 1000.0f);
    return m;
}

static void set_pad_present_flag(ns::ExtendedHIDReport& r, bool present) {
    // Backend/web protocol uses byte 7 of ExtendedHIDReport as the pad-present flag.
    // This lets neutral-but-connected UDP pads claim a Switch slot and receive rumble.
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
//  GameController integration
// ─────────────────────────────────────────────────────────────────────────────

static void attach_handlers(GCController* ctrl, GCExtendedGamepad* gp, GamepadState* st) {
    st->clear_inputs();

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

    if (@available(macOS 10.15, *)) {
        GCMotion* motion = ctrl.motion;
        if (motion) {
            motion.valueChangedHandler = ^(GCMotion* m) {
                GCAcceleration gravity = m.gravity;
                GCAcceleration user    = m.userAcceleration;
                GCRotationRate rate     = m.rotationRate;

                st->ax.store((float)(gravity.x + user.x), std::memory_order_relaxed);
                st->ay.store((float)(gravity.y + user.y), std::memory_order_relaxed);
                st->az.store((float)(gravity.z + user.z), std::memory_order_relaxed);
                st->gx.store((float)rate.x, std::memory_order_relaxed);
                st->gy.store((float)rate.y, std::memory_order_relaxed);
                st->gz.store((float)rate.z, std::memory_order_relaxed);
                st->has_motion.store(true, std::memory_order_relaxed);
            };
            motion.sensorsActive = YES;
        } else {
            st->has_motion.store(false, std::memory_order_relaxed);
        }
    } else {
        st->has_motion.store(false, std::memory_order_relaxed);
    }
}

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
    r.ly = float_to_byte(st.ly.load(std::memory_order_relaxed), true);   // inverted
    r.rx = float_to_byte(st.rx.load(std::memory_order_relaxed), false);
    r.ry = float_to_byte(st.ry.load(std::memory_order_relaxed), true);   // inverted

    return r;
}


// ─────────────────────────────────────────────────────────────────────────────
//  UDP rumble -> GameController haptics
// ─────────────────────────────────────────────────────────────────────────────
static void stop_haptics_for_controller_on_main(int ctrl_idx, bool release_engine) {
    if (ctrl_idx < 0 || ctrl_idx >= MAX_SLOTS) return;
    if (@available(macOS 11.0, *)) {
        NSError* err = nil;
        if (g_hapticPlayers[ctrl_idx]) {
            [g_hapticPlayers[ctrl_idx] stopAtTime:0 error:&err];
            g_hapticPlayers[ctrl_idx] = nil;
        }
        if (release_engine && g_hapticEngines[ctrl_idx]) {
            [g_hapticEngines[ctrl_idx] stopWithCompletionHandler:nil];
            g_hapticEngines[ctrl_idx] = nil;
        }
    }
}

static void set_controller_rumble_async(int ctrl_idx, uint8_t low, uint8_t high, uint64_t duration_us) {
    if (ctrl_idx < 0 || ctrl_idx >= MAX_SLOTS) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (@available(macOS 11.0, *)) {
            GCController* ctrl = g_rumbleControllers[ctrl_idx];
            if (!ctrl || !ctrl.haptics) return;

            const bool neutral = ((low == 0 && high == 0) || duration_us == 0);
            if (neutral) {
                stop_haptics_for_controller_on_main(ctrl_idx, false);
                return;
            }

            CHHapticEngine* engine = g_hapticEngines[ctrl_idx];
            if (!engine) {
                engine = [ctrl.haptics createEngineWithLocality:GCHapticsLocalityDefault];
                if (!engine) return;
                NSError* startErr = nil;
                if (![engine startAndReturnError:&startErr]) {
                    std::cerr << "GameController haptics failed to start for P" << (ctrl_idx + 1)
                              << ": " << (startErr ? startErr.localizedDescription.UTF8String : "unknown") << "\n";
                    return;
                }
                g_hapticEngines[ctrl_idx] = engine;
            }

            stop_haptics_for_controller_on_main(ctrl_idx, false);

            const float intensity = std::max((float)low, (float)high) / 255.0f;
            const float sharpness = (low || high) ? ((float)high / (float)std::max<int>(1, low + high)) : 0.0f;
            const NSTimeInterval duration = std::max<NSTimeInterval>(0.25, (NSTimeInterval)duration_us / 1000000.0);

            NSError* err = nil;
            CHHapticEventParameter* pIntensity = [[CHHapticEventParameter alloc]
                initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity];
            CHHapticEventParameter* pSharpness = [[CHHapticEventParameter alloc]
                initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness];
            CHHapticEvent* event = [[CHHapticEvent alloc]
                initWithEventType:CHHapticEventTypeHapticContinuous
                parameters:@[pIntensity, pSharpness]
                relativeTime:0
                duration:duration];
            CHHapticPattern* pattern = [[CHHapticPattern alloc]
                initWithEvents:@[event] parameters:@[] error:&err];
            if (!pattern || err) {
                std::cerr << "GameController haptic pattern failed for P" << (ctrl_idx + 1)
                          << ": " << (err ? err.localizedDescription.UTF8String : "unknown") << "\n";
                return;
            }

            id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&err];
            if (!player || err) {
                std::cerr << "GameController haptic player failed for P" << (ctrl_idx + 1)
                          << ": " << (err ? err.localizedDescription.UTF8String : "unknown") << "\n";
                return;
            }
            if (![player startAtTime:0 error:&err]) {
                std::cerr << "GameController haptic start failed for P" << (ctrl_idx + 1)
                          << ": " << (err ? err.localizedDescription.UTF8String : "unknown") << "\n";
                return;
            }
            g_hapticPlayers[ctrl_idx] = player;
        }
    });
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

    void stop_all() {
        for (int i = 0; i < 4; ++i) {
            if (states[i].last_controller >= 0)
                set_controller_rumble_async(states[i].last_controller, 0, 0, 0);
            states[i] = SlotState{};
        }
    }

private:
    struct SlotState {
        uint8_t low = 0, high = 0;
        uint64_t until_us = 0;
        uint64_t last_set_us = 0;
        int last_controller = -1;
    } states[4];

    void set_output(int slot, uint8_t low, uint8_t high, int ctrl_idx) {
        if (states[slot].last_controller != -1 && states[slot].last_controller != ctrl_idx)
            set_controller_rumble_async(states[slot].last_controller, 0, 0, 0);
        if (ctrl_idx >= 0)
            set_controller_rumble_async(ctrl_idx, low, high, (low || high) ? 250000ULL : 0ULL);
        states[slot].last_controller = ctrl_idx;
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


// ─────────────────────────────────────────────────────────────────────────────
//  App Delegate (GUI and Core Logic)
// ─────────────────────────────────────────────────────────────────────────────

@class BindingsEditor;

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    @public
    NSTextField* ipField;
    NSButton* connectBtn;
    NSPopUpButton* kbCombo;
    NSButton* bindingsBtn;
    NSTextField* statusField;
    NSTextField* pktLabel;
    NSTextField* ctrlLabels[4];

    GamepadState  states[4];
    GCController* controllers[4];
    NSString* hwNames[4];
    std::atomic<bool> slotActive[4];

    std::thread senderThread;
    std::atomic<bool> connected;
    std::atomic<bool> senderRunning;
    std::atomic<int> keyboardMode;
    std::unordered_map<std::string, std::string> keyBindings;
    int sock;
    uint8_t hmacKey[32];
    std::atomic<uint32_t> packetCount;
    BindingsEditor* _bindingsEditor;
}
- (void)connect;
- (void)disconnect;
- (void)updateUI;
- (void)loadBindings;
- (void)saveBindings;
- (void)kbComboChanged;
- (void)openBindingsEditor;
@end

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

static const std::vector<std::string> binding_keys = {
    "A","B","X","Y","L","R","ZL","ZR",
    "MINUS","PLUS","LSTICK","RSTICK",
    "HOME",
    "LSTICK_UP","LSTICK_DOWN","LSTICK_LEFT","LSTICK_RIGHT",
    "RSTICK_UP","RSTICK_DOWN","RSTICK_LEFT","RSTICK_RIGHT",
    "DPAD_UP","DPAD_DOWN","DPAD_LEFT","DPAD_RIGHT",
    "CAPTURE"
};

// ── Keyboard polling helpers (macOS) ──
static bool mac_key_down(const std::string& name) {
    struct { const char* n; CGKeyCode c; } kmap[] = {
        {"A",0x00},{"B",0x0B},{"C",0x08},{"D",0x02},{"E",0x0E},{"F",0x03},
        {"G",0x05},{"H",0x04},{"I",0x22},{"J",0x26},{"K",0x28},{"L",0x25},
        {"M",0x2E},{"N",0x2D},{"O",0x1F},{"P",0x23},{"Q",0x0C},{"R",0x0F},
        {"S",0x01},{"T",0x11},{"U",0x20},{"V",0x09},{"W",0x0D},{"X",0x07},
        {"Y",0x10},{"Z",0x06},
        {"0",0x1D},{"1",0x12},{"2",0x13},{"3",0x14},{"4",0x15},
        {"5",0x17},{"6",0x16},{"7",0x1A},{"8",0x1C},{"9",0x19},
        {"UP",0x7E},{"DOWN",0x7D},{"LEFT",0x7B},{"RIGHT",0x7C},
        {"LSHIFT",0x38},{"RSHIFT",0x3C},
        {"LCTRL",0x3B},{"RCTRL",0x3E},
        {"LALT",0x3A},{"RALT",0x3D},
        {"SPACE",0x31},{"ENTER",0x24},{"TAB",0x30},
        {"ESC",0x35},{"BACKSPACE",0x33},
        {"F1",0x7A},{"F2",0x78},{"F3",0x63},{"F4",0x76},
        {"F5",0x60},{"F6",0x61},{"F7",0x62},{"F8",0x64},
        {"F9",0x65},{"F10",0x6D},{"F11",0x67},{"F12",0x6F},
        {"HOME",0x73},{"SNAPSHOT",0x69},
    };
    for (auto& km : kmap)
        if (name == km.n) return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, km.c);
    return false;
}

static void apply_keyboard_to_report_mac(ns::HIDReport& rep, const std::unordered_map<std::string, std::string>& bindings, bool override_mode) {
    auto get = [&](const std::string& btn) -> std::string {
        auto it = bindings.find(btn);
        return it != bindings.end() ? it->second : "";
    };
    std::string k;
    k = get("Y");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_Y;
    k = get("B");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_B;
    k = get("A");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_A;
    k = get("X");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_X;
    k = get("L");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_L;
    k = get("R");      if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_R;
    k = get("ZL");     if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_ZL;
    k = get("ZR");     if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_ZR;
    k = get("MINUS");  if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_MINUS;
    k = get("PLUS");   if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_PLUS;
    k = get("LSTICK"); if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_LSTICK;
    k = get("RSTICK"); if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_RSTICK;
    k = get("HOME");   if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_HOME;
    k = get("CAPTURE"); if (!k.empty() && mac_key_down(k)) rep.buttons |= ns::BTN_CAPTURE;
    bool up=false,down=false,left=false,right=false;
    k = get("DPAD_UP");    if (!k.empty()) up    = mac_key_down(k);
    k = get("DPAD_DOWN");  if (!k.empty()) down  = mac_key_down(k);
    k = get("DPAD_LEFT");  if (!k.empty()) left  = mac_key_down(k);
    k = get("DPAD_RIGHT"); if (!k.empty()) right = mac_key_down(k);
    if (up && right) rep.hat = ns::HAT_NE;
    else if (up && left) rep.hat = ns::HAT_NW;
    else if (down && right) rep.hat = ns::HAT_SE;
    else if (down && left) rep.hat = ns::HAT_SW;
    else if (up) rep.hat = ns::HAT_N;
    else if (down) rep.hat = ns::HAT_S;
    else if (left) rep.hat = ns::HAT_W;
    else if (right) rep.hat = ns::HAT_E;

    auto lsu = get("LSTICK_UP"), lsd = get("LSTICK_DOWN");
    auto lsl = get("LSTICK_LEFT"), lsr = get("LSTICK_RIGHT");
    bool lsu_dn = !lsu.empty() && mac_key_down(lsu);
    bool lsd_dn = !lsd.empty() && mac_key_down(lsd);
    bool lsl_dn = !lsl.empty() && mac_key_down(lsl);
    bool lsr_dn = !lsr.empty() && mac_key_down(lsr);
    if (lsl_dn && !lsr_dn) rep.lx = 0;
    else if (lsr_dn && !lsl_dn) rep.lx = 255;
    else if (!override_mode) rep.lx = 128;
    if (lsu_dn && !lsd_dn) rep.ly = 0;
    else if (lsd_dn && !lsu_dn) rep.ly = 255;
    else if (!override_mode) rep.ly = 128;

    auto rsu = get("RSTICK_UP"), rsd = get("RSTICK_DOWN");
    auto rsl = get("RSTICK_LEFT"), rsr = get("RSTICK_RIGHT");
    bool rsu_dn = !rsu.empty() && mac_key_down(rsu);
    bool rsd_dn = !rsd.empty() && mac_key_down(rsd);
    bool rsl_dn = !rsl.empty() && mac_key_down(rsl);
    bool rsr_dn = !rsr.empty() && mac_key_down(rsr);
    if (rsl_dn && !rsr_dn) rep.rx = 0;
    else if (rsr_dn && !rsl_dn) rep.rx = 255;
    else if (!override_mode) rep.rx = 128;
    if (rsu_dn && !rsd_dn) rep.ry = 0;
    else if (rsd_dn && !rsu_dn) rep.ry = 255;
    else if (!override_mode) rep.ry = 128;
}

// ── Bindings Editor Window Controller ──
@interface BindingsEditor : NSWindowController <NSWindowDelegate> {
    @public
    std::unordered_map<std::string, std::string> editBindings;
    std::vector<NSTextField*> keyLabels;
    int listeningIdx;
    BOOL setupMode;
    AppDelegate* parent;
}
- (instancetype)initWithBindings:(const std::unordered_map<std::string, std::string>&)bindings parent:(AppDelegate*)p;
- (void)cancel;
- (void)save;
- (void)changeClicked:(NSButton*)sender;
- (void)resetClicked;
- (void)setupClicked;
@end

// ── Custom NSView to capture key events for bindings editor ──
@class BindingsEditor;
@interface KeyCaptureView : NSView
@property (assign) BindingsEditor* editor;
@end

@implementation BindingsEditor

- (instancetype)initWithBindings:(const std::unordered_map<std::string, std::string>&)bindings parent:(AppDelegate*)p {
    self = [super init];
    if (self) {
        editBindings = bindings;
        parent = p;
        listeningIdx = -1;
        setupMode = NO;

        // 1. Expand window slightly to accommodate larger buttons and spacing
        NSRect frame = NSMakeRect(0, 0, 680, 480);
        NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        [win setTitle:@"Edit Key Bindings"];
        [win setDelegate:self];
        [win center];

        KeyCaptureView* view = [[KeyCaptureView alloc] initWithFrame:frame];
        [view setEditor:self];
        [win setContentView:view];
        
        // Start rendering lower from the top to give breathing room
        int y = (int)frame.size.height - 40;

        // 2. Adjust columns and expand btnW to 75 so "Change" fits
        int lx = 20;
        int rx = 350; // Shifted further right to make room for the wider left column
        int labelW = 115, keyW = 110, btnW = 75, gap = 5;
        int half = (int)binding_keys.size() / 2;
        
        for (int i = 0; i < half; i++) {
            int li = i, ri = i + half;
            
            // Left column
            NSTextField* ll = [[NSTextField alloc] initWithFrame:NSMakeRect(lx, y - 22, labelW, 20)];
            [ll setStringValue:[NSString stringWithUTF8String:binding_keys[li].c_str()]];
            [ll setBezeled:NO]; [ll setDrawsBackground:NO]; [ll setEditable:NO]; [ll setSelectable:NO];
            [ll setAlignment:NSTextAlignmentRight]; // Right-aligning labels looks much cleaner
            [view addSubview:ll];
            
            NSTextField* lv = [[NSTextField alloc] initWithFrame:NSMakeRect(lx + labelW + gap, y - 22, keyW, 20)];
            [lv setStringValue:[NSString stringWithUTF8String:editBindings[binding_keys[li]].c_str()]];
            [lv setBezeled:YES]; [lv setDrawsBackground:YES]; [lv setEditable:NO]; [lv setSelectable:NO];
            [lv setAlignment:NSTextAlignmentCenter];
            [view addSubview:lv];
            keyLabels.push_back(lv);
            
            NSButton* lc = [[NSButton alloc] initWithFrame:NSMakeRect(lx + labelW + gap + keyW + gap, y - 24, btnW, 24)];
            [lc setTitle:@"Change"]; [lc setBezelStyle:NSBezelStyleRounded];
            [lc setTag:(NSInteger)li]; [lc setTarget:self]; [lc setAction:@selector(changeClicked:)];
            [view addSubview:lc];
            
            // Right column
            NSTextField* rl = [[NSTextField alloc] initWithFrame:NSMakeRect(rx, y - 22, labelW, 20)];
            [rl setStringValue:[NSString stringWithUTF8String:binding_keys[ri].c_str()]];
            [rl setBezeled:NO]; [rl setDrawsBackground:NO]; [rl setEditable:NO]; [rl setSelectable:NO];
            [rl setAlignment:NSTextAlignmentRight];
            [view addSubview:rl];
            
            NSTextField* rv = [[NSTextField alloc] initWithFrame:NSMakeRect(rx + labelW + gap, y - 22, keyW, 20)];
            [rv setStringValue:[NSString stringWithUTF8String:editBindings[binding_keys[ri]].c_str()]];
            [rv setBezeled:YES]; [rv setDrawsBackground:YES]; [rv setEditable:NO]; [rv setSelectable:NO];
            [rv setAlignment:NSTextAlignmentCenter];
            [view addSubview:rv];
            keyLabels.push_back(rv);
            
            NSButton* rc = [[NSButton alloc] initWithFrame:NSMakeRect(rx + labelW + gap + keyW + gap, y - 24, btnW, 24)];
            [rc setTitle:@"Change"]; [rc setBezelStyle:NSBezelStyleRounded];
            [rc setTag:(NSInteger)ri]; [rc setTarget:self]; [rc setAction:@selector(changeClicked:)];
            [view addSubview:rc];
            
            y -= 28; // Increased from 26 for slightly better row breathing room
        }

        // 3. Move action buttons to a single clean row at the very bottom
        y -= 10;
        int aw = 85; 
        int ah = 28;
        int by = y - ah; 

        // Left Actions
        NSButton* cancelBtn = [[NSButton alloc] initWithFrame:NSMakeRect(lx, by, aw, ah)];
        [cancelBtn setTitle:@"Cancel"]; [cancelBtn setBezelStyle:NSBezelStyleRounded];
        [cancelBtn setTarget:self]; [cancelBtn setAction:@selector(cancel)];
        [view addSubview:cancelBtn];

        NSButton* saveBtn = [[NSButton alloc] initWithFrame:NSMakeRect(lx + aw + 10, by, aw, ah)];
        [saveBtn setTitle:@"Save"]; [saveBtn setBezelStyle:NSBezelStyleRounded];
        [saveBtn setTarget:self]; [saveBtn setAction:@selector(save)];
        [saveBtn setKeyEquivalent:@"\r"]; // Pressing Enter will trigger Save!
        [view addSubview:saveBtn];

        // Right Actions (anchored to the right edge of the right column)
        int rightEdge = rx + labelW + gap + keyW + gap + btnW; 
        
        NSButton* setupBtn = [[NSButton alloc] initWithFrame:NSMakeRect(rightEdge - aw, by, aw, ah)];
        [setupBtn setTitle:@"Setup"]; [setupBtn setBezelStyle:NSBezelStyleRounded];
        [setupBtn setTarget:self]; [setupBtn setAction:@selector(setupClicked)];
        [view addSubview:setupBtn];

        NSButton* resetBtn = [[NSButton alloc] initWithFrame:NSMakeRect(rightEdge - aw - 10 - aw, by, aw, ah)];
        [resetBtn setTitle:@"Reset"]; [resetBtn setBezelStyle:NSBezelStyleRounded];
        [resetBtn setTarget:self]; [resetBtn setAction:@selector(resetClicked)];
        [view addSubview:resetBtn];

        self.window = win;
    }
    return self;
}

- (void)changeClicked:(NSButton*)sender {
    setupMode = NO;
    int idx = (int)[sender tag];
    listeningIdx = idx;
    [keyLabels[idx] setStringValue:@"..."];
}

- (void)resetClicked {
    auto defs = default_key_bindings();
    for (size_t i = 0; i < binding_keys.size(); i++) {
        editBindings[binding_keys[i]] = defs[binding_keys[i]];
        [keyLabels[i] setStringValue:[NSString stringWithUTF8String:editBindings[binding_keys[i]].c_str()]];
    }
}

- (void)setupClicked {
    setupMode = YES;
    for (size_t i = 0; i < binding_keys.size(); i++) {
        editBindings[binding_keys[i]] = "";
        [keyLabels[i] setStringValue:i == 0 ? @"..." : @""];
    }
    listeningIdx = 0;
}

- (void)cancel {
    setupMode = NO;
    [self.window close];
}

- (void)save {
    parent->keyBindings = editBindings;
    [parent saveBindings];
    [self.window close];
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    return YES;
}

@end

@implementation KeyCaptureView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event {
    if (!self.editor) return;
    int lidx = self.editor->listeningIdx;
    if (lidx < 0) return;
    unsigned short kc = [event keyCode];
    if (kc == 0x35) { // ESC
        self.editor->editBindings[binding_keys[lidx]] = "";
        [self.editor->keyLabels[lidx] setStringValue:@""];
        if (self.editor->setupMode) {
            self.editor->listeningIdx++;
            if (self.editor->listeningIdx < (int)binding_keys.size()) {
                [self.editor->keyLabels[self.editor->listeningIdx] setStringValue:@"..."];
                return;
            }
        }
        self.editor->setupMode = NO;
        self.editor->listeningIdx = -1;
        return;
    }
    auto kc_to_name = [](unsigned short kc) -> std::string {
        struct { unsigned short k; const char* n; } map[] = {
            {0x00,"A"},{0x0B,"B"},{0x08,"C"},{0x02,"D"},{0x0E,"E"},{0x03,"F"},
            {0x05,"G"},{0x04,"H"},{0x22,"I"},{0x26,"J"},{0x28,"K"},{0x25,"L"},
            {0x2E,"M"},{0x2D,"N"},{0x1F,"O"},{0x23,"P"},{0x0C,"Q"},{0x0F,"R"},
            {0x01,"S"},{0x11,"T"},{0x20,"U"},{0x09,"V"},{0x0D,"W"},{0x07,"X"},
            {0x10,"Y"},{0x06,"Z"},
            {0x1D,"0"},{0x12,"1"},{0x13,"2"},{0x14,"3"},{0x15,"4"},
            {0x17,"5"},{0x16,"6"},{0x1A,"7"},{0x1C,"8"},{0x19,"9"},
            {0x7E,"UP"},{0x7D,"DOWN"},{0x7B,"LEFT"},{0x7C,"RIGHT"},
            {0x38,"LSHIFT"},{0x3C,"RSHIFT"},
            {0x3B,"LCTRL"},{0x3E,"RCTRL"},
            {0x3A,"LALT"},{0x3D,"RALT"},
            {0x31,"SPACE"},{0x24,"ENTER"},{0x30,"TAB"},{0x35,"ESC"},{0x33,"BACKSPACE"},
            {0x7A,"F1"},{0x78,"F2"},{0x63,"F3"},{0x76,"F4"},
            {0x60,"F5"},{0x61,"F6"},{0x62,"F7"},{0x64,"F8"},
            {0x65,"F9"},{0x6D,"F10"},{0x67,"F11"},{0x6F,"F12"},
            {0x73,"HOME"},{0x69,"SNAPSHOT"},
        };
        for (auto& m : map)
            if (kc == m.k) return m.n;
        return "";
    };
    std::string name = kc_to_name(kc);
    if (!name.empty()) {
        // In setup mode, skip already-bound keys
        if (self.editor->setupMode) {
            bool alreadyBound = false;
            for (auto& [k, v] : self.editor->editBindings) {
                if (k != binding_keys[lidx] && v == name) { alreadyBound = true; break; }
            }
            if (alreadyBound) return;
        }
        // Remove this key from any other binding
        for (auto& [k, v] : self.editor->editBindings) {
            if (v == name) { v = ""; break; }
        }
        self.editor->editBindings[binding_keys[lidx]] = name;
        [self.editor->keyLabels[lidx] setStringValue:[NSString stringWithUTF8String:name.c_str()]];
        // Update display for any cleared binding
        for (size_t i = 0; i < binding_keys.size(); i++) {
            if (self.editor->editBindings[binding_keys[i]].empty())
                [self.editor->keyLabels[i] setStringValue:@""];
        }
    }
    if (self.editor->setupMode) {
        self.editor->listeningIdx++;
        if (self.editor->listeningIdx < (int)binding_keys.size()) {
            [self.editor->keyLabels[self.editor->listeningIdx] setStringValue:@"..."];
            return;
        }
    }
    self.editor->setupMode = NO;
    self.editor->listeningIdx = -1;
}
@end

@implementation AppDelegate

- (void)loadBindings {
    keyBindings = default_key_bindings();
    NSUserDefaults* defs = [NSUserDefaults standardUserDefaults];
    for (auto& [k, v] : keyBindings) {
        NSString* val = [defs stringForKey:[NSString stringWithUTF8String:("kb_" + k).c_str()]];
        if (val) keyBindings[k] = std::string([val UTF8String]);
    }
    NSNumber* savedMode = [defs objectForKey:@"keyboardMode"];
    if (savedMode) keyboardMode = [savedMode intValue];
}

- (void)saveBindings {
    NSUserDefaults* defs = [NSUserDefaults standardUserDefaults];
    for (auto& [k, v] : keyBindings)
        [defs setObject:[NSString stringWithUTF8String:v.c_str()] forKey:[NSString stringWithUTF8String:("kb_" + k).c_str()]];
    [defs setInteger:keyboardMode.load() forKey:@"keyboardMode"];
}

- (void)kbComboChanged {
    keyboardMode = (int)[kbCombo indexOfSelectedItem];
    if (keyboardMode.load() < 0) keyboardMode = 0;
    [bindingsBtn setEnabled:keyboardMode.load() != KB_OFF];
    [self saveBindings];
}

- (void)openBindingsEditor {
    _bindingsEditor = [[BindingsEditor alloc] initWithBindings:keyBindings parent:self];
    [_bindingsEditor showWindow:self];
}

- (void)applicationDidFinishLaunching:(NSNotification*)aNotification {
    // Elevate priority for lower input latency
    setpriority(PRIO_PROCESS, 0, -20);
    
    // Hidden fallback for old backends. UI stays unchanged; extended UDP is default.
    const char* legacyEnv = getenv("NSPC_LEGACY_UDP");
    g_legacyUdp = legacyEnv && legacyEnv[0] && strcmp(legacyEnv, "0") != 0;

    // Keep receiving gamepad input when the window loses focus
    GCController.shouldMonitorBackgroundEvents = YES;

    // Trigger the Input Monitoring permission prompt if needed
    if (!CGPreflightListenEventAccess()) CGRequestListenEventAccess();

    memset(hmacKey, 0, sizeof(hmacKey));
    keyboardMode = KB_OFF;

    for (int i = 0; i < 4; ++i) {
        controllers[i] = nil;
        g_rumbleControllers[i] = nil;
        g_hapticEngines[i] = nil;
        g_hapticPlayers[i] = nil;
        hwNames[i] = @"";
        slotActive[i] = false;
    }

    [self loadBindings];

    auto assign_controller = ^(GCController* ctrl) {
        if (!ctrl.extendedGamepad) return;
        // Prevent double-assignment (notification may fire for already-connected controllers)
        for (int i = 0; i < 4; ++i) {
            if (self->controllers[i] == ctrl) return;
        }
        for (int i = 0; i < 4; ++i) {
            if (self->controllers[i] == nil) {
                self->controllers[i] = ctrl;
                g_rumbleControllers[i] = ctrl;
                self->slotActive[i].store(true, std::memory_order_relaxed); // FIX 1
                self->hwNames[i] = ctrl.vendorName ?: @"Unknown Controller";
                attach_handlers(ctrl, ctrl.extendedGamepad, &self->states[i]);
                break;
            }
        }
    };

    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note) {
            GCController* ctrl = (GCController*)note.object;
            assign_controller(ctrl);
    }];

    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note) {
            GCController* ctrl = (GCController*)note.object;
            for (int i = 0; i < 4; ++i) {
                if (self->controllers[i] == ctrl) {
                    if (@available(macOS 10.15, *)) {
                        if (ctrl.motion) ctrl.motion.sensorsActive = NO;
                    }
                    stop_haptics_for_controller_on_main(i, true);
                    self->controllers[i] = nil;
                    g_rumbleControllers[i] = nil;
                    self->slotActive[i].store(false, std::memory_order_relaxed); // FIX 1
                    self->states[i].clear_inputs();
                    self->hwNames[i] = @"";
                    break;
                }
            }
    }];

    // Bind already connected controllers
    for (GCController* ctrl in [GCController controllers]) {
        assign_controller(ctrl);
    }

    // ── Build Window ──
    NSRect frame = NSMakeRect(0, 0, 420, 320);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    [window setTitle:@"NS PC Control (Mac)"];
    [window setDelegate:self];
    [window center];

    NSView* view = [window contentView];

    NSTextField* ipLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 280, 110, 20)];
    [ipLabel setStringValue:@"Raspberry Pi IP:"];
    [ipLabel setBezeled:NO]; [ipLabel setDrawsBackground:NO]; [ipLabel setEditable:NO]; [ipLabel setSelectable:NO];
    [view addSubview:ipLabel];

    ipField = [[NSTextField alloc] initWithFrame:NSMakeRect(130, 278, 270, 22)];
    NSString* saved = [[NSUserDefaults standardUserDefaults] stringForKey:@"lastIP"];
    [ipField setStringValue:saved ?: @"192.168.1.100"];
    [view addSubview:ipField];

    // Keyboard Mode row
    NSTextField* kbLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 252, 100, 18)];
    [kbLabel setStringValue:@"Keyboard Mode:"];
    [kbLabel setBezeled:NO]; [kbLabel setDrawsBackground:NO]; [kbLabel setEditable:NO]; [kbLabel setSelectable:NO];
    [kbLabel setAlignment:NSTextAlignmentRight];
    [view addSubview:kbLabel];

    kbCombo = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(125, 248, 175, 24)];
    [kbCombo addItemWithTitle:@"OFF"];
    [kbCombo addItemWithTitle:@"ON (single)"];
    [kbCombo addItemWithTitle:@"ON (override)"];
    [kbCombo selectItemAtIndex:keyboardMode.load()];
    [kbCombo setTarget:self];
    [kbCombo setAction:@selector(kbComboChanged)];
    [view addSubview:kbCombo];

    bindingsBtn = [[NSButton alloc] initWithFrame:NSMakeRect(310, 248, 80, 24)];
    [bindingsBtn setTitle:@"Bindings..."];
    [bindingsBtn setBezelStyle:NSBezelStyleRounded];
    [bindingsBtn setTarget:self];
    [bindingsBtn setAction:@selector(openBindingsEditor)];
    [bindingsBtn setEnabled:(keyboardMode.load() != KB_OFF)];
    [view addSubview:bindingsBtn];

    connectBtn = [[NSButton alloc] initWithFrame:NSMakeRect(125, 215, 120, 32)];
    [connectBtn setTitle:@"Connect"];
    [connectBtn setBezelStyle:NSBezelStyleRounded];
    [connectBtn setTarget:self];
    [connectBtn setAction:@selector(connectClicked)];
    [view addSubview:connectBtn];

    NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect(15, 195, 390, 1)];
    [sep setBoxType:NSBoxSeparator];
    [view addSubview:sep];

    statusField = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 165, 390, 17)];
    [statusField setStringValue:@"Ready"];
    [statusField setBezeled:NO]; [statusField setDrawsBackground:NO]; [statusField setEditable:NO]; [statusField setSelectable:NO];
    [statusField setTextColor:[NSColor grayColor]];
    [view addSubview:statusField];

    // P1 to P4 Labels
    for (int i = 0; i < 4; ++i) {
        ctrlLabels[i] = [[NSTextField alloc] initWithFrame:NSMakeRect(25, 140 - (i * 25), 380, 17)];
        [ctrlLabels[i] setStringValue:[NSString stringWithFormat:@"P%d: Waiting...", i+1]];
        [ctrlLabels[i] setBezeled:NO]; [ctrlLabels[i] setDrawsBackground:NO]; [ctrlLabels[i] setEditable:NO]; [ctrlLabels[i] setSelectable:NO];
        [view addSubview:ctrlLabels[i]];
    }

    pktLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 15, 390, 17)];
    [pktLabel setStringValue:@"Packets sent: 0"];
    [pktLabel setBezeled:NO]; [pktLabel setDrawsBackground:NO]; [pktLabel setEditable:NO]; [pktLabel setSelectable:NO];
    [view addSubview:pktLabel];

    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    [NSTimer scheduledTimerWithTimeInterval:0.1 target:self selector:@selector(updateUI) userInfo:nil repeats:YES];
}

- (void)connectClicked {
    if (connected) [self disconnect];
    else [self connect];
}

- (void)connect {
    NSString* ipStr = [ipField stringValue];
    if ([ipStr length] == 0) return;

    // Parse ip:port safely
    NSArray<NSString*> *parts = [ipStr componentsSeparatedByString:@":"];
    NSString *host = parts.firstObject;
    int port = ns::DEFAULT_PORT;
    if (parts.count > 1) {
        port = [parts.lastObject intValue];
        if (port <= 0 || port > 65535) port = ns::DEFAULT_PORT;
    }
    std::string stdIp([host UTF8String]);

    [[NSUserDefaults standardUserDefaults] setObject:ipStr forKey:@"lastIP"];
    derive_key(ns::DEFAULT_SECRET, hmacKey);

    packetCount = 0;
    connected = true;
    senderRunning = true;

    senderThread = std::thread([self, stdIp, port] {
        self->sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (self->sock < 0) return;

        // Bind to a fixed local port so the backend identifies reconnects as the same PC.
        int opt = 1;
        setsockopt(self->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(self->sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        struct sockaddr_in local_bind{};
        local_bind.sin_family = AF_INET;
        local_bind.sin_addr.s_addr = INADDR_ANY;
        local_bind.sin_port = htons(42069);
        ::bind(self->sock, (struct sockaddr*)&local_bind, sizeof(local_bind));

        int flags = fcntl(self->sock, F_GETFL, 0);
        if (flags >= 0) fcntl(self->sock, F_SETFL, flags | O_NONBLOCK);

        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char portStr[8]; snprintf(portStr, sizeof(portStr), "%u", port);
        
        if (getaddrinfo(stdIp.c_str(), portStr, &hints, &res) != 0 || !res) {
            close(self->sock); self->sock = -1; return;
        }
        
        sockaddr_in dest{};
        memcpy(&dest, res->ai_addr, sizeof(dest));
        freeaddrinfo(res);

        uint32_t seqCounter = 0;
        bool first_packet = true;
        RumbleManager rumble;

        while (self->senderRunning.load(std::memory_order_relaxed)) {
            ns::HIDReport logical_reports[4];
            ns::MotionReport logical_motion[4];
            bool present[4] = {false, false, false, false};
            bool has_motion[4] = {false, false, false, false};
            int controller_for_slot[4] = {-1, -1, -1, -1};
            for (int i = 0; i < 4; ++i) {
                logical_reports[i].reset();
                logical_motion[i].reset();
            }

            int active_count = 0;
            bool c1 = false, c2 = false, c3 = false, c4 = false;
            for (int i = 0; i < 4; ++i) {
                if (!self->slotActive[i].load(std::memory_order_relaxed)) continue;
                logical_reports[i] = map_gc_to_switch(self->states[i]);
                present[i] = true;
                controller_for_slot[i] = i;
                if (self->states[i].has_motion.load(std::memory_order_relaxed)) {
                    logical_motion[i] = map_gc_motion_to_switch(self->states[i]);
                    has_motion[i] = true;
                }
                active_count++;
                if (i == 0) c1 = true;
                else if (i == 1) c2 = true;
                else if (i == 2) c3 = true;
                else if (i == 3) c4 = true;
            }

            // Keyboard overrides Player 1
            int km = self->keyboardMode.load();
            if (km == KB_SINGLE) {
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
                        controller_for_slot[target] = controller_for_slot[0];
                        active_count++;
                    }
                }
                logical_reports[0].reset();
                logical_motion[0].reset();
                apply_keyboard_to_report_mac(logical_reports[0], self->keyBindings, false);
                present[0] = true;
                has_motion[0] = false;
                controller_for_slot[0] = -1;
                active_count = std::max(active_count, 1);
            } else if (km == KB_OVERRIDE) {
                apply_keyboard_to_report_mac(logical_reports[0], self->keyBindings, true);
                present[0] = true;
                active_count = std::max(active_count, 1);
            }

            if (g_legacyUdp) {
                ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet));
                pkt.magic         = ns::PROTO_MAGIC;
                pkt.version       = ns::PROTO_VERSION;
                pkt.flags         = first_packet ? ns::FLAG_RESET : ns::FLAG_NONE;
                first_packet      = false;
                pkt.seq           = seqCounter++;
                pkt.ts_us         = ns::now_us();
                pkt.report.reset();

                ns::HIDReport* pads[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
                for (int i = 0; i < 4; ++i) *pads[i] = logical_reports[i];

                uint8_t full_hmac[32];
                hmac_sha256(self->hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
                sendto(self->sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
            } else {
                ExtendedUdpPacket pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.magic        = ns::PROTO_MAGIC;
                pkt.version      = ns::PROTO_VERSION;
                pkt.flags        = first_packet ? ns::FLAG_RESET : ns::FLAG_NONE;
                first_packet     = false;
                pkt.seq          = seqCounter++;
                pkt.timestamp_us = ns::now_us();
                pkt.report.reset();

                ns::ExtendedHIDReport* pads[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
                for (int i = 0; i < 4; ++i)
                    fill_extended_pad(*pads[i], logical_reports[i], present[i], has_motion[i] ? &logical_motion[i] : nullptr);

                uint8_t full_hmac[32];
                hmac_sha256(self->hmacKey, 32, (const uint8_t*)&pkt, EXT_UDP_PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
                sendto(self->sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&dest, sizeof(dest));

                pump_udp_rumble(self->sock, rumble, controller_for_slot);
                rumble.update_timeouts(controller_for_slot);
            }

            self->packetCount++;

            auto interval = (active_count > 0)
                ? std::chrono::milliseconds(4)
                : std::chrono::milliseconds(50); // keep connection alive below watchdog timeout
            std::this_thread::sleep_for(interval);
        }

        rumble.stop_all();
        close(self->sock);
        self->sock = -1;
    });

    [connectBtn setTitle:@"Disconnect"];
    [ipField setEnabled:NO];
    [kbCombo setEnabled:NO];
    [bindingsBtn setEnabled:NO];
    [statusField setStringValue:[NSString stringWithFormat:@"Connected to %@:%d", host, port]];
    [statusField setTextColor:[NSColor systemGreenColor]];
}

- (void)disconnect {
    connected = false;
    senderRunning = false;
    if (senderThread.joinable()) senderThread.join();

    for (int i = 0; i < 4; ++i)
        stop_haptics_for_controller_on_main(i, false);

    [connectBtn setTitle:@"Connect"];
    [ipField setEnabled:YES];
    [kbCombo setEnabled:YES];
    [bindingsBtn setEnabled:keyboardMode.load() != KB_OFF];
    [statusField setStringValue:@"Disconnected"];
    [statusField setTextColor:[NSColor grayColor]];
}

- (void)updateUI {
    if (connected) {
        [pktLabel setStringValue:[NSString stringWithFormat:@"Packets sent: %u", packetCount.load()]];
    }
    
    int km = keyboardMode.load();
    
    // Figure out where P1's physical controller is being shifted
    int shifted_p1_target = -1;
    if (km == KB_SINGLE && slotActive[0].load(std::memory_order_relaxed)) {
        if (!slotActive[1].load(std::memory_order_relaxed)) shifted_p1_target = 1;
        else if (!slotActive[2].load(std::memory_order_relaxed)) shifted_p1_target = 2;
        else if (!slotActive[3].load(std::memory_order_relaxed)) shifted_p1_target = 3;
    }

    for (int i = 0; i < 4; ++i) {
        NSString* text;
        NSColor* color;
        
        if (i == 0 && km != KB_OFF) {
            text = (km == KB_SINGLE) ? @"P1: Keyboard (Single)" : @"P1: Keyboard (Override)";
            color = [NSColor textColor];
        } 
        else if (i == shifted_p1_target) {
            // Visually insert the shifted controller
            text = [NSString stringWithFormat:@"P%d: %@ (Shifted)", i+1, hwNames[0]];
            color = [NSColor textColor];
        }
        else if (slotActive[i].load(std::memory_order_relaxed)) {
            text = [NSString stringWithFormat:@"P%d: %@", i+1, hwNames[i]];
            color = [NSColor textColor];
        } 
        else {
            text = [NSString stringWithFormat:@"P%d: Waiting...", i+1];
            color = [NSColor disabledControlTextColor];
        }
        
        [ctrlLabels[i] setStringValue:text];
        [ctrlLabels[i] setTextColor:color];
    }
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    [self disconnect];
    [NSApp terminate:self];
    return YES;
}

@end

// ── Entry point ──
int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        AppDelegate* delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}