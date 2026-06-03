/// ns-gamepad.cpp  —  Linux frontend for the Switch wireless gamepad bridge
///
/// Uses the standard Linux Joystick API (/dev/input/jsX).
/// Now features Smart Discovery: Automatically scans js0-js15, interrogates
/// hardware via ioctl, ignores mice/keyboards, and seamlessly hot-plugs real gamepads.
///
/// Build:
///   g++ -O3 -std=c++17 ns-gamepad.cpp -o ns-gamepad -lpthread
///
/// Usage:
///   ./ns-gamepad <RASPBERRY_PI_IP> [-p PORT]

#include <iostream>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cctype>
#include <atomic>
#include <csignal>
#include <string>

// Linux specific headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>      // Required to query hardware specs
#include <linux/joystick.h>
#include <sys/resource.h>
#include "../../server/rpi/include/sha256.h"

// Undefine conflicting button macros from linux/joystick.h to avoid naming conflicts
#undef BTN_A
#undef BTN_B
#undef BTN_X
#undef BTN_Y
#undef BTN_L
#undef BTN_R
#undef BTN_TL
#undef BTN_TR
#undef BTN_SELECT
#undef BTN_START
#undef BTN_MODE
#undef BTN_THUMBL
#undef BTN_THUMBR

// Import external protocol structures (Version 4 with MultiReport)
#include "../../server/rpi/include/protocol.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Shared gamepad state
// ─────────────────────────────────────────────────────────────────────────────

/// Holds the raw state of a Linux joystick
struct GamepadState {
    bool    buttons[16] = {false};
    int16_t axes[8]     = {0};

    void clear_inputs() {
        std::memset(buttons, 0, sizeof(buttons));
        std::memset(axes, 0, sizeof(axes));
    }
};

static GamepadState g_states[4];
static int          g_fds[4] = {-1, -1, -1, -1};
static std::string  g_dev_paths[4] = {"", "", "", ""};
static char         g_hw_names[4][128] = {{0}};

// ─────────────────────────────────────────────────────────────────────────────
//  Signal handling
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }


// ─────────────────────────────────────────────────────────────────────────────
//  Axis conversion
// ─────────────────────────────────────────────────────────────────────────────

/// Convert raw analog stick value to normalized 0-255 range with deadzone applied
uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;

    int scaled;
    if (val >= deadzone) {
        scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    } else {
        scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    }
    
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}


// ─────────────────────────────────────────────────────────────────────────────
//  Input Mapping
// ─────────────────────────────────────────────────────────────────────────────

/// Convert Linux joystick state to Switch Pro Controller HID report format
ns::HIDReport map_linux_js_to_switch(const GamepadState& pad, const char* hw_name) {
    ns::HIDReport r;
    r.reset();

    // Map face buttons (indices 0-3 typically correspond to B, A, Y, X)
    if (pad.buttons[0]) r.buttons |= ns::BTN_B; 
    if (pad.buttons[1]) r.buttons |= ns::BTN_A; 
    if (pad.buttons[2]) r.buttons |= ns::BTN_Y; 
    if (pad.buttons[3]) r.buttons |= ns::BTN_X; 

    // Map shoulder buttons (indices 4-5)
    if (pad.buttons[4]) r.buttons |= ns::BTN_L; 
    if (pad.buttons[5]) r.buttons |= ns::BTN_R; 
    
    // Map trigger buttons via analog axes
    if (pad.axes[2] > 0) r.buttons |= ns::BTN_ZL;
    if (pad.axes[5] > 0) r.buttons |= ns::BTN_ZR;

    // Map menu/select buttons (indices 6-7)
    if (pad.buttons[6]) r.buttons |= ns::BTN_MINUS;  
    if (pad.buttons[7]) r.buttons |= ns::BTN_PLUS;   
    
    // Map stick button presses (indices 9-10)
    if (pad.buttons[9])  r.buttons |= ns::BTN_LSTICK; 
    if (pad.buttons[10]) r.buttons |= ns::BTN_RSTICK; 

    // Emulate HOME and CAPTURE using button combos
    if (pad.buttons[9] && pad.buttons[10]) {
        r.buttons |= ns::BTN_HOME;
        r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK); 
    }
    if (pad.buttons[6] && pad.buttons[7]) {
        r.buttons |= ns::BTN_CAPTURE;
        r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS); 
    }

    // Map D-pad via analog axes (axes 6-7 typically)
    bool up    = (pad.axes[7] < -16000);
    bool down  = (pad.axes[7] >  16000);
    bool left  = (pad.axes[6] < -16000);
    bool right = (pad.axes[6] >  16000);

    r.hat = ns::HAT_NEUTRAL;

    if      (up && right)   r.hat = ns::HAT_NE;
    else if (up && left)    r.hat = ns::HAT_NW;
    else if (down && right) r.hat = ns::HAT_SE;
    else if (down && left)  r.hat = ns::HAT_SW;
    else if (up)            r.hat = ns::HAT_N;
    else if (down)          r.hat = ns::HAT_S;
    else if (left)          r.hat = ns::HAT_W;
    else if (right)         r.hat = ns::HAT_E;

    // Detect Bluetooth/wireless axis layout — shifts right stick from 3/4 to 2/3
    std::string name = hw_name;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    bool wireless = (name.find("wireless") != std::string::npos ||
                     name.find("bluetooth") != std::string::npos);
    int rx_axis = wireless ? 2 : 3;
    int ry_axis = wireless ? 3 : 4;

    // Map analog sticks
    r.lx = apply_deadzone(pad.axes[0], false);
    r.ly = apply_deadzone(pad.axes[1], false);
    r.rx = apply_deadzone(pad.axes[rx_axis], false);
    r.ry = apply_deadzone(pad.axes[ry_axis], false);

    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Hardware Discovery (Ignores Mice/Keyboards)
// ─────────────────────────────────────────────────────────────────────────────

/// Interrogates the Linux kernel to check if a device is a legitimate gamepad
bool is_valid_gamepad(int fd, std::string& out_name) {
    char name[128];
    if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) < 0) {
        strncpy(name, "Unknown Device", sizeof(name));
    }
    
    char axes = 0, buttons = 0;
    ioctl(fd, JSIOCGAXES, &axes);
    ioctl(fd, JSIOCGBUTTONS, &buttons);
    
    out_name = name;
    
    // Heuristic: A real modern gamepad has at least 2 axes (sticks/dpad) and 4 buttons.
    // This perfectly filters out 3D mice, laptop accelerometers, and weird keyboards.
    return (axes >= 2 && buttons >= 4);
}

/// Periodically scans /dev/input/jsX and assigns valid gamepads to free P1-P4 slots
void scan_for_gamepads() {
    static uint64_t last_scan = 0;
    uint64_t now = ns::now_us();
    
    // Only scan once per second to save CPU
    if (now - last_scan < 1'000'000) return;
    last_scan = now;

    for (int i = 0; i < 16; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/js%d", i);
        
        // Skip if this specific device is already mapped to one of our 4 players
        bool already_mapped = false;
        for (int p = 0; p < 4; ++p) {
            if (g_dev_paths[p] == path) { already_mapped = true; break; }
        }
        if (already_mapped) continue;

        // Attempt to open the device
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        std::string hw_name;
        if (is_valid_gamepad(fd, hw_name)) {
            // It's a real gamepad! Find a free player slot (0 to 3)
            bool assigned = false;
            for (int p = 0; p < 4; ++p) {
                if (g_fds[p] < 0) {
                    g_fds[p] = fd;
                    g_dev_paths[p] = path;
                    strncpy(g_hw_names[p], hw_name.c_str(), sizeof(g_hw_names[p]) - 1);
                    g_states[p].clear_inputs();
                    std::cout << "🎮 [P" << (p + 1) << "] Connected: " << hw_name << " (" << path << ")\n";
                    assigned = true;
                    break;
                }
            }
            if (!assigned) {
                // We already have 4 controllers connected, drop the extra one
                close(fd);
            }
        } else {
            // It's a mouse or garbage device, close immediately
            close(fd);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Device Polling
// ─────────────────────────────────────────────────────────────────────────────

/// Drains events from a connected gamepad and builds the HID report
void read_pad(int index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    
    if (g_fds[index] < 0) {
        conn = false;
        return;
    }

    struct js_event event;
    while (true) {
        ssize_t bytes = read(g_fds[index], &event, sizeof(event));
        
        if (bytes > 0) {
            uint8_t type = event.type & ~JS_EVENT_INIT;
            if (type == JS_EVENT_BUTTON && event.number < 16) {
                g_states[index].buttons[event.number] = event.value;
            } else if (type == JS_EVENT_AXIS && event.number < 8) {
                g_states[index].axes[event.number] = event.value;
            }
        } else {
            // Error handling: if not EAGAIN, the device was physically unplugged
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cout << "❌ [P" << (index + 1) << "] Disconnected: " << g_dev_paths[index] << "\n";
                close(g_fds[index]);
                g_fds[index] = -1;
                g_dev_paths[index] = "";
                g_hw_names[index][0] = '\0';
                conn = false;
                return;
            }
            break; // No more events in buffer, break the read loop
        }
    }

    conn = true;
    rep = map_linux_js_to_switch(g_states[index], g_hw_names[index]);
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

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create UDP socket.\n";
        return 1;
    }

    // Resolve address
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);
    
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        std::cerr << "Cannot resolve address: " << host << "\n";
        close(sock); return 1;
    }
    
    sockaddr_in dest{};
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    std::cout << "Started as a Multi-Client Node... Connect gamepads and enjoy!\n";

    // Elevate process priority for low-latency input reading
    setpriority(PRIO_PROCESS, 0, -20);

    uint32_t seq = 0;
    auto next_tick = std::chrono::steady_clock::now();

    // ── Main Loop (Input Polling & UDP Networking) ────────────────────────────
    while (g_running.load(std::memory_order_relaxed)) {
        
        // Busy-wait for 2ms precision timing
        while (std::chrono::steady_clock::now() < next_tick) {
            std::atomic_thread_fence(std::memory_order_relaxed);
        }

        // 1. Scan for newly plugged controllers
        scan_for_gamepads();

        // 2. Prepare network packet
        ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet)); 
        pkt.magic         = ns::PROTO_MAGIC;
        pkt.version       = ns::PROTO_VERSION;
        pkt.flags         = ns::FLAG_NONE;
        pkt.seq           = seq++;
        pkt.ts_us         = ns::now_us();
        
        pkt.report.reset(); // Hardware-neutral init

        ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
        int active_count = 0;

        // 3. Read active controllers, preserving fixed slot assignment
        for (int i = 0; i < 4; ++i) {
            bool is_conn = false;
            read_pad(i, *out_reports[i], is_conn);
            if (is_conn) {
                active_count++;
            }
        }

        // 4. Sign packet
        {
            uint8_t full_hmac[32];
            hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        }

        // 5. Transmit to Server
        sendto(sock, (const char*)&pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
        
        // Dynamic throttling: 500Hz when active, 2Hz when idle (saves network/CPU)
        if (active_count > 0) next_tick += std::chrono::milliseconds(2);
        else next_tick += std::chrono::milliseconds(500);
    }

    // ── Graceful shutdown ──────────────────────────────────────────────────────
    std::cout << "\nShutting down...\n";
    for(int i=0; i<4; ++i) {
        if(g_fds[i] >= 0) close(g_fds[i]);
    }
    close(sock);
    return 0;
}