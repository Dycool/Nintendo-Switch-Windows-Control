/// ns-gui.mm  —  macOS Cocoa GUI frontend for the Switch wireless gamepad bridge
///
/// Features Smart Discovery: Automatically detects up to 4 local controllers
/// via Apple's GameController framework and seamlessly packs them into the UDP stream.
///
/// Build:
///   clang++ -std=c++17 -ObjC++ -fobjc-arc \
///           -framework Cocoa -framework GameController \
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

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <cmath>
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
struct GamepadState {
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
        // note: does not clear slotActive (managed separately)
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

// ─────────────────────────────────────────────────────────────────────────────
//  GameController integration
// ─────────────────────────────────────────────────────────────────────────────

static void attach_handlers(GCExtendedGamepad* gp, GamepadState* st) {
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
//  App Delegate (GUI and Core Logic)
// ─────────────────────────────────────────────────────────────────────────────

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSTextField* ipField;
    NSButton* connectBtn;
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
    int sock;
    uint8_t hmacKey[32];
    std::atomic<uint32_t> packetCount;
}
- (void)connect;
- (void)disconnect;
- (void)updateUI;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)aNotification {
    // Elevate priority for lower input latency
    setpriority(PRIO_PROCESS, 0, -20);
    
    // Keep receiving gamepad input when the window loses focus
    GCController.shouldMonitorBackgroundEvents = YES;

    // Trigger the Input Monitoring permission prompt if needed
    if (!CGPreflightListenEventAccess()) CGRequestListenEventAccess();

    memset(hmacKey, 0, sizeof(hmacKey));

    for (int i = 0; i < 4; ++i) {
        controllers[i] = nil;
        hwNames[i] = @"";
        slotActive[i] = false;
    }

    auto assign_controller = ^(GCController* ctrl) {
        if (!ctrl.extendedGamepad) return;
        // Prevent double-assignment (notification may fire for already-connected controllers)
        for (int i = 0; i < 4; ++i) {
            if (self->controllers[i] == ctrl) return;
        }
        for (int i = 0; i < 4; ++i) {
            if (self->controllers[i] == nil) {
                self->controllers[i] = ctrl;
                self->slotActive[i].store(true, std::memory_order_relaxed); // FIX 1
                self->hwNames[i] = ctrl.vendorName ?: @"Unknown Controller";
                attach_handlers(ctrl.extendedGamepad, &self->states[i]);
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
                    self->controllers[i] = nil;
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
    NSRect frame = NSMakeRect(0, 0, 420, 290);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    [window setTitle:@"NS PC Control (Mac)"];
    [window setDelegate:self];
    [window center];

    NSView* view = [window contentView];

    NSTextField* ipLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 250, 110, 20)];
    [ipLabel setStringValue:@"Raspberry Pi IP:"];
    [ipLabel setBezeled:NO]; [ipLabel setDrawsBackground:NO]; [ipLabel setEditable:NO]; [ipLabel setSelectable:NO];
    [view addSubview:ipLabel];

    ipField = [[NSTextField alloc] initWithFrame:NSMakeRect(130, 248, 270, 22)];
    NSString* saved = [[NSUserDefaults standardUserDefaults] stringForKey:@"lastIP"];
    [ipField setStringValue:saved ?: @"192.168.1.100"];
    [view addSubview:ipField];

    connectBtn = [[NSButton alloc] initWithFrame:NSMakeRect(125, 205, 120, 32)];
    [connectBtn setTitle:@"Connect"];
    [connectBtn setBezelStyle:NSBezelStyleRounded];
    [connectBtn setTarget:self];
    [connectBtn setAction:@selector(connectClicked)];
    [view addSubview:connectBtn];

    NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect(15, 185, 390, 1)];
    [sep setBoxType:NSBoxSeparator];
    [view addSubview:sep];

    statusField = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 155, 390, 17)];
    [statusField setStringValue:@"Ready"];
    [statusField setBezeled:NO]; [statusField setDrawsBackground:NO]; [statusField setEditable:NO]; [statusField setSelectable:NO];
    [statusField setTextColor:[NSColor grayColor]];
    [view addSubview:statusField];

    // P1 to P4 Labels
    for (int i = 0; i < 4; ++i) {
        ctrlLabels[i] = [[NSTextField alloc] initWithFrame:NSMakeRect(25, 130 - (i * 25), 380, 17)];
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

        // Bind to a fixed local port so the backend identifies reconnects as the same PC
        int opt = 1;
        setsockopt(self->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(self->sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        struct sockaddr_in local_bind{};
        local_bind.sin_family = AF_INET;
        local_bind.sin_addr.s_addr = INADDR_ANY;
        local_bind.sin_port = htons(42069);
        ::bind(self->sock, (struct sockaddr*)&local_bind, sizeof(local_bind));

        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char portStr[8]; snprintf(portStr, sizeof(portStr), "%u", port);
        
        if (getaddrinfo(stdIp.c_str(), portStr, &hints, &res) != 0 || !res) {
            close(self->sock); return;
        }
        
        sockaddr_in dest{};
        memcpy(&dest, res->ai_addr, sizeof(dest));
        freeaddrinfo(res);

        uint32_t seqCounter = 0;
        bool first_packet = true;

        while (self->senderRunning.load(std::memory_order_relaxed)) {
            ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet)); 
            pkt.magic         = ns::PROTO_MAGIC;
            pkt.version       = ns::PROTO_VERSION;
            pkt.flags         = first_packet ? ns::FLAG_RESET : ns::FLAG_NONE;
            first_packet      = false;
            pkt.seq           = seqCounter++;
            pkt.ts_us         = ns::now_us();

            pkt.report.reset(); // Hardware-neutral init

            ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
            int active_count = 0;

            for (int i = 0; i < 4; ++i) {
                // FIX 1: Read the atomic flag instead of the bare ObjC pointer.
                if (!self->slotActive[i].load(std::memory_order_relaxed)) continue;

                // FIX 2: Use slot index i directly so P2's data goes to report.p2,
                // not remapped to p1 just because p1 is empty.
                *out_reports[i] = map_gc_to_switch(self->states[i]);
                active_count++;
            }

            {
                uint8_t full_hmac[32];
                hmac_sha256(self->hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
            }

            sendto(self->sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
            self->packetCount++;

            // FIX 3: Sleep instead of busy-waiting so we don't burn a full CPU core.
            auto interval = (active_count > 0)
        ? std::chrono::milliseconds(2)
            : std::chrono::milliseconds(50); // keep connection alive below watchdog timeout
            std::this_thread::sleep_for(interval);
        }
        close(self->sock);
    });

    [connectBtn setTitle:@"Disconnect"];
    [ipField setEnabled:NO];
    [statusField setStringValue:[NSString stringWithFormat:@"Connected to %@:%d", host, port]];
    [statusField setTextColor:[NSColor systemGreenColor]];
}

- (void)disconnect {
    connected = false;
    senderRunning = false;
    if (senderThread.joinable()) senderThread.detach();

    [connectBtn setTitle:@"Connect"];
    [ipField setEnabled:YES];
    [statusField setStringValue:@"Disconnected"];
    [statusField setTextColor:[NSColor grayColor]];
}

- (void)updateUI {
    if (connected) {
        [pktLabel setStringValue:[NSString stringWithFormat:@"Packets sent: %u", packetCount.load()]];
    }
    
    // Always update gamepad connectivity status (whether connected to RPi or not)
    for (int i = 0; i < 4; ++i) {
        if (slotActive[i].load(std::memory_order_relaxed)) {
            [ctrlLabels[i] setStringValue:[NSString stringWithFormat:@"🎮 P%d: %@", i+1, hwNames[i]]];
            [ctrlLabels[i] setTextColor:[NSColor textColor]];
        } else {
            [ctrlLabels[i] setStringValue:[NSString stringWithFormat:@"P%d: Waiting...", i+1]];
            [ctrlLabels[i] setTextColor:[NSColor disabledControlTextColor]];
        }
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