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

        NSRect frame = NSMakeRect(0, 0, 600, 460);
        NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        [win setTitle:@"Edit Key Bindings"];
        [win setDelegate:self];
        [win center];

        KeyCaptureView* view = [[KeyCaptureView alloc] initWithFrame:frame];
        [view setEditor:self];
        [win setContentView:view];
        int y = (int)frame.size.height - 30;

        int lx = 15, rx = 302;
        int labelW = 100, keyW = 110, btnW = 54, gap = 4;
        int half = (int)binding_keys.size() / 2;
        for (int i = 0; i < half; i++) {
            int li = i, ri = i + half;
            // Left column
            NSTextField* ll = [[NSTextField alloc] initWithFrame:NSMakeRect(lx, y - 22, labelW, 20)];
            [ll setStringValue:[NSString stringWithUTF8String:binding_keys[li].c_str()]];
            [ll setBezeled:NO]; [ll setDrawsBackground:NO]; [ll setEditable:NO]; [ll setSelectable:NO];
            [ll setAlignment:NSTextAlignmentCenter];
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
            [rl setAlignment:NSTextAlignmentCenter];
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
            y -= 26;
        }

        y -= 8;
        int aw = 74;
        int leftBtnX = lx;
        int rightBtnX = rx + labelW + gap + keyW + gap + btnW - aw;
        // Left: Save (above) Cancel (below)
        NSButton* saveBtn = [[NSButton alloc] initWithFrame:NSMakeRect(leftBtnX, y - 4, aw, 28)];
        [saveBtn setTitle:@"Save"]; [saveBtn setBezelStyle:NSBezelStyleRounded];
        [saveBtn setTarget:self]; [saveBtn setAction:@selector(save)];
        [view addSubview:saveBtn];
        NSButton* setupBtn = [[NSButton alloc] initWithFrame:NSMakeRect(rightBtnX, y - 4, aw, 28)];
        [setupBtn setTitle:@"Setup"]; [setupBtn setBezelStyle:NSBezelStyleRounded];
        [setupBtn setTarget:self]; [setupBtn setAction:@selector(setupClicked)];
        [view addSubview:setupBtn];
        y -= 34;
        // Right: Cancel (below left) Reset (below right)
        NSButton* cancelBtn = [[NSButton alloc] initWithFrame:NSMakeRect(leftBtnX, y - 4, aw, 28)];
        [cancelBtn setTitle:@"Cancel"]; [cancelBtn setBezelStyle:NSBezelStyleRounded];
        [cancelBtn setTarget:self]; [cancelBtn setAction:@selector(cancel)];
        [view addSubview:cancelBtn];
        NSButton* resetBtn = [[NSButton alloc] initWithFrame:NSMakeRect(rightBtnX, y - 4, aw, 28)];
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
    
    // Keep receiving gamepad input when the window loses focus
    GCController.shouldMonitorBackgroundEvents = YES;

    // Trigger the Input Monitoring permission prompt if needed
    if (!CGPreflightListenEventAccess()) CGRequestListenEventAccess();

    memset(hmacKey, 0, sizeof(hmacKey));
    keyboardMode = KB_OFF;

    for (int i = 0; i < 4; ++i) {
        controllers[i] = nil;
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
    [bindingsBtn setEnabled:NO];
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

            bool c1 = false, c2 = false, c3 = false, c4 = false;
            for (int i = 0; i < 4; ++i) {
                if (!self->slotActive[i].load(std::memory_order_relaxed)) continue;
                *out_reports[i] = map_gc_to_switch(self->states[i]);
                active_count++;
                if (i == 0) c1 = true;
                else if (i == 1) c2 = true;
                else if (i == 2) c3 = true;
                else if (i == 3) c4 = true;
            }

            // Keyboard overrides Player 1
            int km = self->keyboardMode.load();
            if (km == KB_SINGLE) {
                if (c1) {
                    if (!c2) {
                        *out_reports[1] = *out_reports[0]; c2 = true; active_count++;
                        self->slotActive[1].store(true, std::memory_order_relaxed);
                        self->states[1] = self->states[0];
                        self->hwNames[1] = self->hwNames[0];
                    } else if (!c3) {
                        *out_reports[2] = *out_reports[0]; c3 = true; active_count++;
                        self->slotActive[2].store(true, std::memory_order_relaxed);
                        self->states[2] = self->states[0];
                        self->hwNames[2] = self->hwNames[0];
                    } else if (!c4) {
                        *out_reports[3] = *out_reports[0]; c4 = true; active_count++;
                        self->slotActive[3].store(true, std::memory_order_relaxed);
                        self->states[3] = self->states[0];
                        self->hwNames[3] = self->hwNames[0];
                    }
                }
                out_reports[0]->reset();
                apply_keyboard_to_report_mac(*out_reports[0], self->keyBindings, false);
                active_count = std::max(active_count, 1);
            } else if (km == KB_OVERRIDE) {
                apply_keyboard_to_report_mac(*out_reports[0], self->keyBindings, true);
                active_count = std::max(active_count, 1);
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
        ? std::chrono::milliseconds(4)
            : std::chrono::milliseconds(50); // keep connection alive below watchdog timeout
            std::this_thread::sleep_for(interval);
        }
        close(self->sock);
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
    if (senderThread.joinable()) senderThread.detach();

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
    for (int i = 0; i < 4; ++i) {
        NSString* text;
        NSColor* color;
        if (i == 0 && km != KB_OFF) {
            if (km == KB_SINGLE) {
                text = @"P1: Keyboard";
                color = [NSColor textColor];
            } else {
                bool conn = slotActive[0].load(std::memory_order_relaxed);
                text = [NSString stringWithFormat:@"P1: %@ / Keyboard", conn ? @"Connected" : @"Idle"];
                color = [NSColor textColor];
            }
        } else if (slotActive[i].load(std::memory_order_relaxed)) {
            text = [NSString stringWithFormat:@"P%d: %@", i+1, hwNames[i]];
            color = [NSColor textColor];
        } else {
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