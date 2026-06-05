#include "../../include/protocol.hpp"
#include "../../include/sha256.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <algorithm>
#include <vector>
#include <cstdint>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <ctype.h>

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

using namespace ns;
using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

// ── Global flags ──────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static bool g_verbose = false;

// HMAC authentication (key derived from DEFAULT_SECRET at startup)
static uint8_t  g_hmac_key[32];

// Per-IP rate limiting (token bucket, 32-entry hash table)
static constexpr uint64_t RATE_WINDOW_US = 1'000'000;  // 1 second
static constexpr uint32_t RATE_MAX_PKT   = 2000;       // max packets/sec per IP
static constexpr int      RATE_TABLE     = 32;

struct RateSlot {
    uint32_t ip;           // IP in network byte order, 0 = empty
    uint32_t count;
    uint64_t window_start; // us
};
static RateSlot g_rate_table[RATE_TABLE];

// ── Multi-Client Session State ───────────────────────────────────────────────
static constexpr int MAX_CLIENTS = 4; // Hard limit matching the 4 physical ports

struct ClientSession {
    bool        active = false;
    sockaddr_in addr{};
    uint64_t    last_rx_us = 0;
    uint32_t    expected_seq = 0;
    bool        first_pkt = true;
    MultiReport report{}; // The inputs coming from this specific PC
};

static std::mutex    g_mtx;
static ClientSession g_clients[MAX_CLIENTS];

// Diagnostics
static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};

// ── Signal ────────────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }


// ── Smart Multiplexer HID Writer Thread ───────────────────────────────────────
static void writer_thread(int hz) {
    const auto tick = us(1'000'000 / hz);
    int fds[4] = {-1, -1, -1, -1};
    std::string devs[4] = {"/dev/hidg0", "/dev/hidg1", "/dev/hidg2", "/dev/hidg3"};
    bool was_connected = false;

    // Tracks which physical Switch port is claimed by which (Client, SubController)
    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[4];

    auto is_neutral = [](const HIDReport& r) {
        return r.buttons == 0 && r.hat == 8 && r.lx == 128 && r.ly == 128 && r.rx == 128 && r.ry == 128;
    };

    while (g_running.load(std::memory_order_relaxed)) {
        bool all_open = true;
        for(int i=0; i<4; ++i) {
            if (fds[i] < 0) {
                fds[i] = open(devs[i].c_str(), O_WRONLY);
                if (fds[i] < 0) all_open = false;
            }
        }

        if (!all_open) {
            std::this_thread::sleep_for(ms(500));
            continue;
        }
        
        if (g_verbose || !was_connected)
            std::puts("4x /dev/hidg* opened");
        was_connected = true;

        auto next = Clock::now() + tick;
        MultiReport prev{}; prev.p1.buttons = 0xFFFF; // Force first write
        bool error_shown = false;

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now(); next = std::max(next + tick, now + tick);

            MultiReport r;
            r.reset(); // Base neutral state

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                uint64_t now_stamp = now_us();

                // 1. Clear timed-out clients (Watchdog)
                for (int c = 0; c < MAX_CLIENTS; ++c) {
                    if (g_clients[c].active && (now_stamp - g_clients[c].last_rx_us > WATCHDOG_MS * 1000ULL)) {
                        g_clients[c].active = false;
                        if (g_verbose) std::printf("PC %d timed out and was disconnected.\n", c+1);
                    }
                }

                // 2. Free hardware slots mapped to inactive clients
                for (int h = 0; h < 4; ++h) {
                    if (hw_slots[h].client_idx != -1) {
                        if (!g_clients[hw_slots[h].client_idx].active) {
                            hw_slots[h].client_idx = -1;
                            hw_slots[h].sub_idx = -1;
                        }
                    }
                }

                // 3. Auto-assign unmapped active inputs to free hardware slots
                for (int c = 0; c < MAX_CLIENTS; ++c) {
                    if (!g_clients[c].active) continue;
                    
                    HIDReport* subs[4] = { &g_clients[c].report.p1, &g_clients[c].report.p2, 
                                           &g_clients[c].report.p3, &g_clients[c].report.p4 };
                    
                    for (int s = 0; s < 4; ++s) {
                        bool mapped = false;
                        for (int h = 0; h < 4; ++h) {
                            if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                                mapped = true; break;
                            }
                        }
                        
                        // If player pressed a button and doesn't have a physical port yet
                        if (!mapped && !is_neutral(*subs[s])) {
                            for (int h = 0; h < 4; ++h) {
                                if (hw_slots[h].client_idx == -1) {
                                    hw_slots[h].client_idx = c;
                                    hw_slots[h].sub_idx = s;
                                    if (g_verbose) 
                                        std::printf("Map -> PC %d (Pad %d) took Switch Port %d\n", c+1, s+1, h+1);
                                    break;
                                }
                            }
                        }
                    }
                }

                // 4. Construct the final mixed 4-player report
                HIDReport* out_subs[4] = { &r.p1, &r.p2, &r.p3, &r.p4 };
                for (int h = 0; h < 4; ++h) {
                    if (hw_slots[h].client_idx != -1) {
                        int c = hw_slots[h].client_idx;
                        int s = hw_slots[h].sub_idx;
                        HIDReport* src_subs[4] = { &g_clients[c].report.p1, &g_clients[c].report.p2, 
                                                   &g_clients[c].report.p3, &g_clients[c].report.p4 };
                        *out_subs[h] = *src_subs[s];
                    }
                }
            }

            // 5. Send to physical USB gadget drivers efficiently
            bool ok = true;
            if (r.p1 != prev.p1) { if(write(fds[0], &r.p1, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p2 != prev.p2) { if(write(fds[1], &r.p2, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p3 != prev.p3) { if(write(fds[2], &r.p3, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p4 != prev.p4) { if(write(fds[3], &r.p4, 8) < 0 && errno != EAGAIN) ok = false; }

            if (!ok) {
                if (!error_shown) { std::puts("Switch disconnected — waiting for reconnect..."); error_shown = true; }
                for(int i=0; i<4; ++i) { close(fds[i]); fds[i] = -1; }
                std::this_thread::sleep_for(ms(1000)); break;
            }
            prev = r;
            ++g_hid_writes;
        }
    }
    
    // Shutdown securely by neutralizing all ports
    MultiReport neutral{}; neutral.reset();
    for(int i=0; i<4; ++i) { 
        if (fds[i] >= 0) { ssize_t _u = write(fds[i], &neutral.p1, 8); (void)_u; close(fds[i]); }
    }
}


// ── Stats thread ──────────────────────────────────────────────────────────────
static void stats_thread() {
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(ms(5000));
        if (!g_verbose) continue;
        std::printf("pkts_rx=%-8llu  hid_writes=%-8llu\n",
            (unsigned long long)g_pkts_rx.load(),
            (unsigned long long)g_hid_writes.load());
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
static bool rate_allow(uint32_t ip) {
    uint64_t now = now_us();
    uint32_t idx = ip % RATE_TABLE;
    RateSlot &s = g_rate_table[idx];
    if (s.ip != ip) {
        s.ip = ip; s.count = 1; s.window_start = now; return true;
    }
    if (now - s.window_start > RATE_WINDOW_US) {
        s.count = 1; s.window_start = now; return true;
    }
    s.count++;
    return s.count <= RATE_MAX_PKT;
}


#ifdef USE_UPNP
// ── UPnP port forwarding ──
static bool g_upnp_active = false;
static UPNPUrls g_upnp_urls{};
static IGDdatas g_upnp_data{};
static char g_upnp_lan_addr[64]{};

static bool upnp_add_mapping(uint16_t port) {
    if (g_upnp_active) return false; 

    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
    if (!devlist) return false;
    
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
    freeUPNPDevlist(devlist);
    
    if (igd != 1 && igd != 2) return false;
    
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    
    int r = UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype,
                                port_str, port_str, g_upnp_lan_addr, "ns-backend", "UDP", nullptr, "0");
    if (r != 0) { FreeUPNPUrls(&g_upnp_urls); return false; }
    
    g_upnp_active = true;
    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, external_ip) == 0) {
        std::printf("UPnP: UDP port %u successfully forwarded!\n", port);
        std::printf("UPnP: Tell your clients to connect to -> %s:%u\n", external_ip, port);
    }
    return true;
}

static void upnp_remove_mapping(uint16_t port) {
    if (!g_upnp_active) return;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str, "UDP", nullptr);
    std::puts("UPnP: port mapping removed cleanly");
    FreeUPNPUrls(&g_upnp_urls); g_upnp_active = false;
}
#else
static bool upnp_add_mapping(uint16_t) { return false; }
static void upnp_remove_mapping(uint16_t) {}
#endif


// ══════════════════════════════════════════════════════════════════════════════
// ── Embedded Web Server (HTTP + WebSocket proxy, enabled with -w) ────────────
// ══════════════════════════════════════════════════════════════════════════════

// ── Embedded index.html (served at GET /) ─────────────────────────────────────
static const char INDEX_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <title>NS Web Control</title>\n"
    "    <style>\n"
    "        body {\n"
    "            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;\n"
    "            background-color: #FFFFFF;\n"
    "            color: #1A1A1A;\n"
    "            padding: 20px;\n"
    "            max-width: 480px;\n"
    "        }\n"
    "        h2 { color: #CC0000; margin-top: 0; font-size: 20px; }\n"
    "        .row { display: flex; align-items: center; margin-bottom: 12px; }\n"
    "        .row label { width: 140px; text-align: right; margin-right: 10px; font-size: 14px; }\n"
    "        input[type=\"text\"], select {\n"
    "            flex: 1; padding: 4px; font-family: 'Consolas', monospace; border: 1px solid #ccc;\n"
    "        }\n"
    "        button {\n"
    "            padding: 6px 16px; font-family: 'Segoe UI'; font-size: 14px;\n"
    "            background: #f0f0f0; border: 1px solid #ccc; cursor: pointer;\n"
    "            border-radius: 4px;\n"
    "            min-height: 30px;\n"
    "        }\n"
    "        button:hover { background: #e0e0e0; }\n"
    "        .btn-group { display: flex; gap: 10px; margin-left: 150px; margin-top: 15px; }\n"
    "        hr { border: none; border-top: 2px solid #DDDDDD; margin: 20px 0; }\n"
    "        .status { font-size: 13px; margin-bottom: 4px; }\n"
    "        \n"
    "        #modalOverlay {\n"
    "            display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%;\n"
    "            background: rgba(0,0,0,0.5); justify-content: center; align-items: center;\n"
    "        }\n"
    "        #modalContent {\n"
    "            background: white; padding: 20px; border-radius: 8px; width: 440px;\n"
    "            max-height: 80vh; display: flex; flex-direction: column; box-shadow: 0 4px 12px rgba(0,0,0,0.2);\n"
    "        }\n"
    "        #modalContent h3 { margin-top: 0; margin-bottom: 15px; color: #CC0000; flex-shrink: 0; }\n"
    "        \n"
    "        #bindingsScrollArea {\n"
    "            overflow-y: auto; flex-grow: 1; padding-right: 10px;\n"
    "        }\n"
    "        \n"
    "        .bind-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; font-size: 13px; }\n"
    "        .bind-btn { width: 120px; font-family: 'Consolas'; font-size: 12px; transition: background 0.2s; }\n"
    "        .listening { background: #ffeb3b !important; border-color: #fbc02d !important; color: #000; transform: scale(1.02); }\n"
    "        \n"
    "        .modal-footer {\n"
    "            display: flex; justify-content: space-between; margin-top: 15px; padding-top: 15px; border-top: 1px solid #eee; flex-shrink: 0;\n"
    "        }\n"
    "        .footer-group { display: flex; gap: 8px; }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "\n"
    "    <h2>NS Web Control</h2>\n"
    "\n"
    "    <div class=\"row\">\n"
    "        <label>Keyboard Mode:</label>\n"
    "        <select id=\"kbMode\">\n"
    "            <option value=\"0\">OFF</option>\n"
    "            <option value=\"1\">ON (single)</option>\n"
    "            <option value=\"2\">ON (override)</option>\n"
    "        </select>\n"
    "    </div>\n"
    "\n"
    "    <div class=\"btn-group\">\n"
    "        <button id=\"btnConnect\">Connect</button>\n"
    "        <button id=\"btnBindings\">Bindings...</button>\n"
    "    </div>\n"
    "\n"
    "    <hr>\n"
    "\n"
    "    <div class=\"status\" id=\"statusText\">Ready</div>\n"
    "    <div class=\"status\" id=\"p1Text\">P1: Idle</div>\n"
    "    <div class=\"status\" id=\"p2Text\">P2: Not connected</div>\n"
    "    <div class=\"status\" id=\"p3Text\">P3: Not connected</div>\n"
    "    <div class=\"status\" id=\"p4Text\">P4: Not connected</div>\n"
    "\n"
    "    <div id=\"modalOverlay\">\n"
    "        <div id=\"modalContent\">\n"
    "            <h3>Keyboard Bindings</h3>\n"
    "            <div id=\"bindingsScrollArea\">\n"
    "                <div id=\"bindingsList\"></div>\n"
    "            </div>\n"
    "            <div class=\"modal-footer\">\n"
    "                <div class=\"footer-group\">\n"
    "                    <button id=\"btnSaveBindings\" style=\"background: #e3f2fd; border-color: #90caf9;\">Save</button>\n"
    "                    <button id=\"btnCancelBindings\">Cancel</button>\n"
    "                </div>\n"
    "                <div class=\"footer-group\">\n"
    "                    <button id=\"btnSetupBindings\">Setup Wizard</button>\n"
    "                    <button id=\"btnResetBindings\">Reset</button>\n"
    "                </div>\n"
    "            </div>\n"
    "        </div>\n"
    "    </div>\n"
    "\n"
    "<script>\n"
    "const PROTO_MAGIC = 0x4E535743;\n"
    "const PROTO_VERSION = 4;\n"
    "const SECRET = \"nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY\";\n"
    "const PACKET_SIZE = 68;\n"
    "const PACKET_AUTH_SIZE = 52;\n"
    "\n"
    "const BTN_Y = 1<<0, BTN_B = 1<<1, BTN_A = 1<<2, BTN_X = 1<<3;\n"
    "const BTN_L = 1<<4, BTN_R = 1<<5, BTN_ZL = 1<<6, BTN_ZR = 1<<7;\n"
    "const BTN_MINUS = 1<<8, BTN_PLUS = 1<<9, BTN_LSTICK = 1<<10, BTN_RSTICK = 1<<11;\n"
    "const BTN_HOME = 1<<12, BTN_CAPTURE = 1<<13;\n"
    "const HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3, HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8;\n"
    "\n"
    "let ws = null;\n"
    "let isConnected = false;\n"
    "let loopId = null;\n"
    "let seqCounter = 0;\n"
    "const keysDown = new Set();\n"
    "\n"
    "const defaultBindings = {\n"
    "    'BTN_Y': 'KeyZ', 'BTN_B': 'KeyX', 'BTN_A': 'KeyV', 'BTN_X': 'KeyC',\n"
    "    'BTN_L': 'KeyQ', 'BTN_R': 'KeyE', 'BTN_ZL': 'Digit1', 'BTN_ZR': 'Digit2',\n"
    "    'BTN_MINUS': 'Digit3', 'BTN_PLUS': 'Digit4',\n"
    "    'BTN_LSTICK': 'ShiftLeft', 'BTN_RSTICK': 'ShiftRight',\n"
    "    'BTN_HOME': 'Home', 'BTN_CAPTURE': 'PrintScreen',\n"
    "    'DPAD_UP': 'ArrowUp', 'DPAD_DOWN': 'ArrowDown', 'DPAD_LEFT': 'ArrowLeft', 'DPAD_RIGHT': 'ArrowRight',\n"
    "    'LSTICK_UP': 'KeyW', 'LSTICK_DOWN': 'KeyS', 'LSTICK_LEFT': 'KeyA', 'LSTICK_RIGHT': 'KeyD',\n"
    "    'RSTICK_UP': 'KeyI', 'RSTICK_DOWN': 'KeyK', 'RSTICK_LEFT': 'KeyJ', 'RSTICK_RIGHT': 'KeyL'\n"
    "};\n"
    "\n"
    "let currentBindings = { ...defaultBindings };\n"
    "let preEditBindings = {};\n"
    "\n"
    "window.onload = () => {\n"
    "    const savedMode = localStorage.getItem('nswc_mode');\n"
    "    if (savedMode) document.getElementById('kbMode').value = savedMode;\n"
    "    const savedBindings = localStorage.getItem('nswc_bindings');\n"
    "    if (savedBindings) currentBindings = JSON.parse(savedBindings);\n"
    "};\n"
    "\n"
    "document.getElementById('kbMode').onchange = (e) => localStorage.setItem('nswc_mode', e.target.value);\n"
    "\n"
    "window.addEventListener('keydown', (e) => {\n"
    "    if (activeBindKey) { e.preventDefault(); remapKey(e.code); return; }\n"
    "    keysDown.add(e.code);\n"
    "});\n"
    "window.addEventListener('keyup', (e) => keysDown.delete(e.code));\n"
    "\n"
    "function initCrypto() {}\n"
    "\n"
    "function getNeutralState() {\n"
    "    return { buttons: 0, hat: HAT_NEUTRAL, lx: 128, ly: 128, rx: 128, ry: 128 };\n"
    "}\n"
    "\n"
    "function getKeyboardState() {\n"
    "    let buttons = 0, hat = HAT_NEUTRAL, lx = 128, ly = 128, rx = 128, ry = 128;\n"
    "    if (keysDown.has(currentBindings['BTN_Y'])) buttons |= BTN_Y;\n"
    "    if (keysDown.has(currentBindings['BTN_B'])) buttons |= BTN_B;\n"
    "    if (keysDown.has(currentBindings['BTN_A'])) buttons |= BTN_A;\n"
    "    if (keysDown.has(currentBindings['BTN_X'])) buttons |= BTN_X;\n"
    "    if (keysDown.has(currentBindings['BTN_L'])) buttons |= BTN_L;\n"
    "    if (keysDown.has(currentBindings['BTN_R'])) buttons |= BTN_R;\n"
    "    if (keysDown.has(currentBindings['BTN_ZL'])) buttons |= BTN_ZL;\n"
    "    if (keysDown.has(currentBindings['BTN_ZR'])) buttons |= BTN_ZR;\n"
    "    if (keysDown.has(currentBindings['BTN_MINUS'])) buttons |= BTN_MINUS;\n"
    "    if (keysDown.has(currentBindings['BTN_PLUS'])) buttons |= BTN_PLUS;\n"
    "    if (keysDown.has(currentBindings['BTN_LSTICK'])) buttons |= BTN_LSTICK;\n"
    "    if (keysDown.has(currentBindings['BTN_RSTICK'])) buttons |= BTN_RSTICK;\n"
    "    if (keysDown.has(currentBindings['BTN_HOME'])) buttons |= BTN_HOME;\n"
    "    if (keysDown.has(currentBindings['BTN_CAPTURE'])) buttons |= BTN_CAPTURE;\n"
    "    const up = keysDown.has(currentBindings['DPAD_UP']), down = keysDown.has(currentBindings['DPAD_DOWN']);\n"
    "    const left = keysDown.has(currentBindings['DPAD_LEFT']), right = keysDown.has(currentBindings['DPAD_RIGHT']);\n"
    "    if (up && right) hat = HAT_NE; else if (up && left) hat = HAT_NW;\n"
    "    else if (down && right) hat = HAT_SE; else if (down && left) hat = HAT_SW;\n"
    "    else if (up) hat = HAT_N; else if (down) hat = HAT_S;\n"
    "    else if (left) hat = HAT_W; else if (right) hat = HAT_E;\n"
    "    if (keysDown.has(currentBindings['LSTICK_LEFT']) && !keysDown.has(currentBindings['LSTICK_RIGHT'])) lx = 0;\n"
    "    else if (keysDown.has(currentBindings['LSTICK_RIGHT']) && !keysDown.has(currentBindings['LSTICK_LEFT'])) lx = 255;\n"
    "    if (keysDown.has(currentBindings['LSTICK_UP']) && !keysDown.has(currentBindings['LSTICK_DOWN'])) ly = 0;\n"
    "    else if (keysDown.has(currentBindings['LSTICK_DOWN']) && !keysDown.has(currentBindings['LSTICK_UP'])) ly = 255;\n"
    "    if (keysDown.has(currentBindings['RSTICK_LEFT']) && !keysDown.has(currentBindings['RSTICK_RIGHT'])) rx = 0;\n"
    "    else if (keysDown.has(currentBindings['RSTICK_RIGHT']) && !keysDown.has(currentBindings['RSTICK_LEFT'])) rx = 255;\n"
    "    if (keysDown.has(currentBindings['RSTICK_UP']) && !keysDown.has(currentBindings['RSTICK_DOWN'])) ry = 0;\n"
    "    else if (keysDown.has(currentBindings['RSTICK_DOWN']) && !keysDown.has(currentBindings['RSTICK_UP'])) ry = 255;\n"
    "    return { buttons, hat, lx, ly, rx, ry };\n"
    "}\n"
    "\n"
    "function getGamepadState(pad) {\n"
    "    if (!pad) return null;\n"
    "    let buttons = 0, hat = HAT_NEUTRAL, lx = 128, ly = 128, rx = 128, ry = 128;\n"
    "    if (pad.buttons[0]?.pressed) buttons |= BTN_B;\n"
    "    if (pad.buttons[1]?.pressed) buttons |= BTN_A;\n"
    "    if (pad.buttons[2]?.pressed) buttons |= BTN_Y;\n"
    "    if (pad.buttons[3]?.pressed) buttons |= BTN_X;\n"
    "    if (pad.buttons[4]?.pressed) buttons |= BTN_L;\n"
    "    if (pad.buttons[5]?.pressed) buttons |= BTN_R;\n"
    "    if (pad.buttons[6]?.pressed) buttons |= BTN_ZL;\n"
    "    if (pad.buttons[7]?.pressed) buttons |= BTN_ZR;\n"
    "    if (pad.buttons[8]?.pressed) buttons |= BTN_MINUS;\n"
    "    if (pad.buttons[9]?.pressed) buttons |= BTN_PLUS;\n"
    "    if (pad.buttons[10]?.pressed) buttons |= BTN_LSTICK;\n"
    "    if (pad.buttons[11]?.pressed) buttons |= BTN_RSTICK;\n"
    "    if (pad.buttons[16]?.pressed) buttons |= BTN_HOME;\n"
    "    if (pad.buttons[17]?.pressed) buttons |= BTN_CAPTURE;\n"
    "    if ((buttons & BTN_LSTICK) && (buttons & BTN_RSTICK)) buttons |= BTN_HOME;\n"
    "    if ((buttons & BTN_MINUS) && (buttons & BTN_PLUS)) buttons |= BTN_CAPTURE;\n"
    "    const pup = pad.buttons[12]?.pressed, pdown = pad.buttons[13]?.pressed;\n"
    "    const pleft = pad.buttons[14]?.pressed, pright = pad.buttons[15]?.pressed;\n"
    "    if (pup && pright) hat = HAT_NE; else if (pup && pleft) hat = HAT_NW;\n"
    "    else if (pdown && pright) hat = HAT_SE; else if (pdown && pleft) hat = HAT_SW;\n"
    "    else if (pup) hat = HAT_N; else if (pdown) hat = HAT_S;\n"
    "    else if (pleft) hat = HAT_W; else if (pright) hat = HAT_E;\n"
    "    const applyDeadzone = (val) => {\n"
    "        if (Math.abs(val) < 0.15) return 128;\n"
    "        return Math.round(((val + 1) / 2) * 255);\n"
    "    };\n"
    "    if (pad.axes.length >= 4) {\n"
    "        lx = applyDeadzone(pad.axes[0]);\n"
    "        ly = applyDeadzone(pad.axes[1]);\n"
    "        rx = applyDeadzone(pad.axes[2]);\n"
    "        ry = applyDeadzone(pad.axes[3]);\n"
    "    }\n"
    "    return { buttons, hat, lx, ly, rx, ry };\n"
    "}\n"
    "\n"
    "function mergeStates(s1, s2) {\n"
    "    if (!s1) return s2;\n"
    "    if (!s2) return s1;\n"
    "    return {\n"
    "        buttons: s1.buttons | s2.buttons,\n"
    "        hat: s1.hat !== HAT_NEUTRAL ? s1.hat : s2.hat,\n"
    "        lx: s1.lx !== 128 ? s1.lx : s2.lx,\n"
    "        ly: s1.ly !== 128 ? s1.ly : s2.ly,\n"
    "        rx: s1.rx !== 128 ? s1.rx : s2.rx,\n"
    "        ry: s1.ry !== 128 ? s1.ry : s2.ry\n"
    "    };\n"
    "}\n"
    "\n"
    "function buildAndSendPacket() {\n"
    "    if (!ws || ws.readyState !== WebSocket.OPEN) return;\n"
    "    const rawGamepads = navigator.getGamepads ? navigator.getGamepads() : [];\n"
    "    const activePads = [];\n"
    "    for (let i = 0; i < rawGamepads.length; i++) {\n"
    "        if (rawGamepads[i]) activePads.push(rawGamepads[i]);\n"
    "    }\n"
    "    const mode = parseInt(document.getElementById('kbMode').value);\n"
    "    const kbState = getKeyboardState();\n"
    "    let slotStates = [null, null, null, null];\n"
    "    let uiText = [\"\", \"\", \"\", \"\"];\n"
    "    if (mode === 0) {\n"
    "        for (let i = 0; i < 4; i++) {\n"
    "            let gp = getGamepadState(activePads[i]);\n"
    "            slotStates[i] = gp || getNeutralState();\n"
    "            uiText[i] = gp ? \"Connected\" : \"Not connected\";\n"
    "        }\n"
    "    } else if (mode === 1) {\n"
    "        slotStates[0] = kbState;\n"
    "        uiText[0] = `Keyboard (Connected)`;\n"
    "        for (let i = 1; i < 4; i++) {\n"
    "            let gp = getGamepadState(activePads[i - 1]);\n"
    "            slotStates[i] = gp || getNeutralState();\n"
    "            uiText[i] = gp ? \"Connected\" : \"Not connected\";\n"
    "        }\n"
    "    } else if (mode === 2) {\n"
    "        let gp0 = getGamepadState(activePads[0]);\n"
    "        slotStates[0] = mergeStates(kbState, gp0 || getNeutralState());\n"
    "        uiText[0] = `${gp0 ? \"Connected\" : \"Not connected\"} \\\\ Keyboard`;\n"
    "        for (let i = 1; i < 4; i++) {\n"
    "            let gp = getGamepadState(activePads[i]);\n"
    "            slotStates[i] = gp || getNeutralState();\n"
    "            uiText[i] = gp ? \"Connected\" : \"Not connected\";\n"
    "        }\n"
    "    }\n"
    "    const buffer = new ArrayBuffer(PACKET_SIZE);\n"
    "    const view = new DataView(buffer);\n"
    "    view.setUint32(0, PROTO_MAGIC, true);\n"
    "    view.setUint8(4, PROTO_VERSION);\n"
    "    view.setUint8(5, 0);\n"
    "    view.setUint16(6, 0, true);\n"
    "    view.setUint32(8, seqCounter++, true);\n"
    "    view.setBigUint64(12, BigInt(Date.now() * 1000), true);\n"
    "    for(let p = 0; p < 4; p++) {\n"
    "        document.getElementById(`p${p+1}Text`).innerText = `P${p+1}: ${uiText[p]}`;\n"
    "        let finalButtons = slotStates[p].buttons;\n"
    "        if (finalButtons & BTN_CAPTURE) finalButtons &= ~(BTN_PLUS | BTN_MINUS);\n"
    "        if (finalButtons & BTN_HOME) finalButtons &= ~(BTN_LSTICK | BTN_RSTICK);\n"
    "        const offset = 20 + (p * 8);\n"
    "        view.setUint16(offset, finalButtons, true);\n"
    "        view.setUint8(offset + 2, slotStates[p].hat);\n"
    "        view.setUint8(offset + 3, slotStates[p].lx);\n"
    "        view.setUint8(offset + 4, slotStates[p].ly);\n"
    "        view.setUint8(offset + 5, slotStates[p].rx);\n"
    "        view.setUint8(offset + 6, slotStates[p].ry);\n"
    "        view.setUint8(offset + 7, 0);\n"
    "    }\n"
    "    ws.send(buffer);\n"
    "}\n"
    "\n"
    "document.getElementById('btnConnect').onclick = async () => {\n"
    "    if (isConnected) {\n"
    "        clearInterval(loopId); ws.close(); isConnected = false;\n"
    "        document.getElementById('btnConnect').innerText = \"Connect\";\n"
    "        document.getElementById('statusText').innerText = \"Disconnected\";\n"
    "        document.getElementById('kbMode').disabled = false;\n"
    "        return;\n"
    "    }\n"
    "    const wsUrl = window.location.protocol === 'https:' ? `wss://${window.location.host}` : `ws://${window.location.host}`;\n"
    "    ws = new WebSocket(wsUrl);\n"
    "    ws.binaryType = \"arraybuffer\";\n"
    "    ws.onopen = () => {\n"
    "        isConnected = true;\n"
    "        document.getElementById('btnConnect').innerText = \"Disconnect\";\n"
    "        document.getElementById('kbMode').disabled = true;\n"
    "        document.getElementById('statusText').innerText = `Connected to Pi Proxy.`;\n"
    "        loopId = setInterval(buildAndSendPacket, 8);\n"
    "    };\n"
    "    ws.onerror = () => alert(\"Failed to connect to proxy!\");\n"
    "    ws.onclose = () => {\n"
    "        isConnected = false; clearInterval(loopId);\n"
    "        document.getElementById('btnConnect').innerText = \"Connect\";\n"
    "        document.getElementById('statusText').innerText = \"Disconnected\";\n"
    "        document.getElementById('kbMode').disabled = false;\n"
    "    }\n"
    "};\n"
    "\n"
    "function formatKeyName(code) {\n"
    "    if (code === 'Unbound') return '';\n"
    "    if (code.startsWith('Key')) return code.replace('Key', '');\n"
    "    if (code.startsWith('Digit')) return code.replace('Digit', '');\n"
    "    if (code.startsWith('Arrow')) return code.replace('Arrow', '');\n"
    "    if (code === 'ShiftLeft') return 'LShift';\n"
    "    if (code === 'ShiftRight') return 'RShift';\n"
    "    if (code === 'ControlLeft') return 'LCtrl';\n"
    "    if (code === 'ControlRight') return 'RCtrl';\n"
    "    if (code === 'AltLeft') return 'LAlt';\n"
    "    if (code === 'AltRight') return 'RAlt';\n"
    "    return code;\n"
    "}\n"
    "\n"
    "let activeBindKey = null;\n"
    "let isSetupMode = false;\n"
    "let setupQueue = [];\n"
    "\n"
    "function renderBindings() {\n"
    "    const list = document.getElementById('bindingsList');\n"
    "    list.innerHTML = '';\n"
    "    for (const [btn, code] of Object.entries(currentBindings)) {\n"
    "        const row = document.createElement('div');\n"
    "        row.className = 'bind-row';\n"
    "        const label = document.createElement('span');\n"
    "        label.innerText = btn.replace('BTN_', '');\n"
    "        const btnChange = document.createElement('button');\n"
    "        btnChange.className = 'bind-btn';\n"
    "        btnChange.innerText = formatKeyName(code);\n"
    "        btnChange.id = `btn-${btn}`;\n"
    "        btnChange.onclick = () => {\n"
    "            if (isSetupMode) return;\n"
    "            if (activeBindKey) document.getElementById(`btn-${activeBindKey}`).classList.remove('listening');\n"
    "            activeBindKey = btn;\n"
    "            btnChange.innerText = \"---\";\n"
    "            btnChange.classList.add('listening');\n"
    "        };\n"
    "        row.appendChild(label);\n"
    "        row.appendChild(btnChange);\n"
    "        list.appendChild(row);\n"
    "    }\n"
    "}\n"
    "\n"
    "function startNextSetupBind() {\n"
    "    if (setupQueue.length === 0) {\n"
    "        isSetupMode = false;\n"
    "        activeBindKey = null;\n"
    "        renderBindings();\n"
    "        return;\n"
    "    }\n"
    "    activeBindKey = setupQueue.shift();\n"
    "    renderBindings();\n"
    "    const targetBtn = document.getElementById(`btn-${activeBindKey}`);\n"
    "    if (targetBtn) {\n"
    "        targetBtn.innerText = \"---\";\n"
    "        targetBtn.classList.add('listening');\n"
    "        targetBtn.scrollIntoView({ behavior: 'smooth', block: 'center' });\n"
    "    }\n"
    "}\n"
    "\n"
    "function remapKey(code) {\n"
    "    if (!activeBindKey) return;\n"
    "    if (code === 'Escape') {\n"
    "        isSetupMode = false;\n"
    "        activeBindKey = null;\n"
    "        renderBindings();\n"
    "        return;\n"
    "    }\n"
    "    if (isSetupMode) {\n"
    "        for (const existingCode of Object.values(currentBindings)) {\n"
    "            if (existingCode === code) return;\n"
    "        }\n"
    "        currentBindings[activeBindKey] = code;\n"
    "        startNextSetupBind();\n"
    "    } else {\n"
    "        for (const [existingBtn, existingCode] of Object.entries(currentBindings)) {\n"
    "            if (existingCode === code && existingBtn !== activeBindKey) {\n"
    "                currentBindings[existingBtn] = 'Unbound';\n"
    "            }\n"
    "        }\n"
    "        currentBindings[activeBindKey] = code;\n"
    "        activeBindKey = null;\n"
    "        renderBindings();\n"
    "    }\n"
    "}\n"
    "\n"
    "document.getElementById('btnBindings').onclick = () => {\n"
    "    preEditBindings = { ...currentBindings };\n"
    "    isSetupMode = false;\n"
    "    activeBindKey = null;\n"
    "    renderBindings();\n"
    "    document.getElementById('modalOverlay').style.display = 'flex';\n"
    "};\n"
    "\n"
    "document.getElementById('btnSaveBindings').onclick = () => {\n"
    "    localStorage.setItem('nswc_bindings', JSON.stringify(currentBindings));\n"
    "    isSetupMode = false;\n"
    "    activeBindKey = null;\n"
    "    document.getElementById('modalOverlay').style.display = 'none';\n"
    "};\n"
    "\n"
    "document.getElementById('btnCancelBindings').onclick = () => {\n"
    "    currentBindings = { ...preEditBindings };\n"
    "    isSetupMode = false;\n"
    "    activeBindKey = null;\n"
    "    document.getElementById('modalOverlay').style.display = 'none';\n"
    "};\n"
    "\n"
    "document.getElementById('btnResetBindings').onclick = () => {\n"
    "    if (isSetupMode) isSetupMode = false;\n"
    "    currentBindings = { ...defaultBindings };\n"
    "    activeBindKey = null;\n"
    "    renderBindings();\n"
    "};\n"
    "\n"
    "document.getElementById('btnSetupBindings').onclick = () => {\n"
    "    for (let k in currentBindings) {\n"
    "        currentBindings[k] = 'Unbound';\n"
    "    }\n"
    "    setupQueue = Object.keys(currentBindings);\n"
    "    isSetupMode = true;\n"
    "    startNextSetupBind();\n"
    "};\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";


// ── Minimal SHA-1 (for WebSocket handshake) ───────────────────────────────────
struct Sha1Ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
};

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (block[i*4]<<24) | (block[i*4+1]<<16) | (block[i*4+2]<<8) | block[i*4+3];
    for (int i = 16; i < 80; i++) {
        uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (t << 1) | (t >> 31);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)       { f = (b & c) | (~b & d);       k = 0x5A827999; }
        else if (i < 40)  { f = b ^ c ^ d;                k = 0x6ED9EBA1; }
        else if (i < 60)  { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;                k = 0xCA62C1D6; }
        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1_init(Sha1Ctx *ctx) {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE; ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0; ctx->count = 0;
}

static void sha1_update(Sha1Ctx *ctx, const uint8_t *data, size_t len) {
    size_t idx = ctx->count & 63;
    ctx->count += len;
    while (len--) {
        ctx->buffer[idx++] = *data++;
        if (idx == 64) { sha1_transform(ctx->state, ctx->buffer); idx = 0; }
    }
}

static void sha1_final(Sha1Ctx *ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = ctx->count & 63;
    size_t pad = (idx < 56) ? (56 - idx) : (120 - idx);
    uint8_t padding[64];
    memset(padding, 0, pad);
    padding[0] = 0x80;
    sha1_update(ctx, padding, pad);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) len_bytes[7-i] = (bits >> (i*8)) & 0xFF;
    sha1_update(ctx, len_bytes, 8);
    for (int i = 0; i < 5; i++) {
        digest[i*4]   = (ctx->state[i] >> 24) & 0xFF;
        digest[i*4+1] = (ctx->state[i] >> 16) & 0xFF;
        digest[i*4+2] = (ctx->state[i] >> 8) & 0xFF;
        digest[i*4+3] = ctx->state[i] & 0xFF;
    }
}


// ── Minimal SHA-256 (for HMAC signing in web proxy) ──────────────────────────
static const uint32_t SHA256_K[64] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x542B0B7,  0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0xFC19DC6,  0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x6CA6351,  0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2
};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_S0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_S1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_s0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_s1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
};

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4]<<24) | ((uint32_t)block[i*4+1]<<16) | ((uint32_t)block[i*4+2]<<8) | block[i*4+3];
    for (int i = 16; i < 64; i++)
        w[i] = SHA256_s1(w[i-2]) + w[i-7] + SHA256_s0(w[i-15]) + w[i-16];
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SHA256_S1(e) + SHA256_CH(e, f, g) + SHA256_K[i] + w[i];
        uint32_t t2 = SHA256_S0(a) + SHA256_MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_init(Sha256Ctx *ctx) {
    ctx->state[0] = 0x6A09E667; ctx->state[1] = 0xBB67AE85;
    ctx->state[2] = 0x3C6EF372; ctx->state[3] = 0xA54FF53A;
    ctx->state[4] = 0x510E527F; ctx->state[5] = 0x9B05688C;
    ctx->state[6] = 0x1F83D9AB; ctx->state[7] = 0x5BE0CD19;
    ctx->count = 0;
}

static void sha256_update(Sha256Ctx *ctx, const uint8_t *data, size_t len) {
    size_t idx = ctx->count & 63;
    ctx->count += len;
    while (len--) {
        ctx->buffer[idx++] = *data++;
        if (idx == 64) { sha256_transform(ctx->state, ctx->buffer); idx = 0; }
    }
}

static void sha256_final(Sha256Ctx *ctx, uint8_t digest[32]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = ctx->count & 63;
    size_t pad = (idx < 56) ? (56 - idx) : (120 - idx);
    uint8_t padding[64];
    memset(padding, 0, pad);
    padding[0] = 0x80;
    sha256_update(ctx, padding, pad);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) len_bytes[7-i] = (bits >> (i*8)) & 0xFF;
    sha256_update(ctx, len_bytes, 8);
    for (int i = 0; i < 8; i++) {
        digest[i*4]   = (ctx->state[i] >> 24) & 0xFF;
        digest[i*4+1] = (ctx->state[i] >> 16) & 0xFF;
        digest[i*4+2] = (ctx->state[i] >> 8) & 0xFF;
        digest[i*4+3] = ctx->state[i] & 0xFF;
    }
}


// ── HMAC-SHA256 (sign packets forwarded from web clients) ────────────────────
static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *msg, size_t msg_len,
                        uint8_t out_hmac[32]) {
    uint8_t k[64];
    memset(k, 0, 64);
    if (key_len > 64) {
        Sha256Ctx ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, key, key_len);
        sha256_final(&ctx, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, msg, msg_len);
    uint8_t inner[32];
    sha256_final(&ctx, inner);
    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out_hmac);
}


// ── Base64 encoding ──────────────────────────────────────────────────────────
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, size_t len, char *out) {
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i+1 < len) v |= (uint32_t)in[i+1] << 8;
        if (i+2 < len) v |= in[i+2];
        *out++ = B64[(v >> 18) & 0x3F];
        *out++ = B64[(v >> 12) & 0x3F];
        *out++ = (i+1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        *out++ = (i+2 < len) ? B64[v & 0x3F] : '=';
    }
    *out = '\0';
}


// ── Read exactly N bytes from fd (returns false on error/close) ───────────────
static bool read_exact(int fd, uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t r = read(fd, buf, n);
        if (r <= 0) return false;
        buf += r;
        n -= r;
    }
    return true;
}


// ── Handle a single WebSocket client: read frames, forward to UDP ────────────
static void handle_ws_client(int fd, uint16_t udp_port) {
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { close(fd); return; }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(udp_port);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1

    uint8_t buf[4096];

    while (g_running.load(std::memory_order_relaxed)) {
        uint8_t hdr[2];
        if (!read_exact(fd, hdr, 2)) break;

        int opcode = hdr[0] & 0x0F;
        bool masked = hdr[1] & 0x80;
        uint64_t len = hdr[1] & 0x7F;

        if (len == 126) {
            uint8_t ext[2];
            if (!read_exact(fd, ext, 2)) break;
            len = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!read_exact(fd, ext, 8)) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
        }

        uint8_t mask[4] = {0};
        if (masked && !read_exact(fd, mask, 4)) break;

        if (len > sizeof(buf)) break;
        if (!read_exact(fd, buf, len)) break;

        if (masked)
            for (uint64_t i = 0; i < len; i++) buf[i] ^= mask[i & 3];

        if (opcode == 8) break; // close
        if (opcode == 9) {     // ping → pong
            uint8_t pong[2] = {0x8A, 0x00};
            ssize_t _u = write(fd, pong, 2); (void)_u;
            continue;
        }
        if (opcode == 2 && len >= PACKET_SIZE) {
            // Sign the packet with HMAC before forwarding to UDP backend
            uint8_t hmac[32];
            hmac_sha256(g_hmac_key, 32, buf, PACKET_AUTH_SIZE, hmac);
            memcpy(buf + PACKET_AUTH_SIZE, hmac, HMAC_TAG_SIZE);
            sendto(udp_fd, buf, len, 0, (sockaddr*)&dst, sizeof(dst));
        }
    }

    close(udp_fd);
    close(fd);
}


// ── Perform WebSocket upgrade handshake ──────────────────────────────────────
static bool ws_upgrade(int fd, const char *key_line) {
    // Extract key value after "Sec-WebSocket-Key: "
    const char *key_start = strstr(key_line, "Sec-WebSocket-Key:");
    if (!key_start) return false;
    key_start += 18;
    while (*key_start == ' ') key_start++;

    // Find end of line
    const char *key_end = strchr(key_start, '\r');
    if (!key_end) key_end = strchr(key_start, '\n');
    if (!key_end) return false;

    // Copy key
    char key[256];
    size_t klen = key_end - key_start;
    if (klen >= sizeof(key)) return false;
    memcpy(key, key_start, klen);
    key[klen] = '\0';

    // Compute accept = base64(sha1(key + magic GUID))
    const char *magic = "258EAFA5-E914-47DA-95CA-5AB9DC4-36-36-4B";
    uint8_t sha_input[256];
    size_t slen = snprintf((char*)sha_input, sizeof(sha_input), "%s%s", key, magic);

    Sha1Ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, sha_input, slen);
    uint8_t digest[20];
    sha1_final(&ctx, digest);

    char b64out[64];
    base64_encode(digest, 20, b64out);

    char resp[512];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", b64out);

    return write(fd, resp, n) == n;
}


// ── Serve index.html via HTTP ────────────────────────────────────────────────
static void serve_http(int fd, const char *request) {
    (void)request;
    char hdr[512];
    int nh = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n", strlen(INDEX_HTML));
    ssize_t _u1 = write(fd, hdr, nh); (void)_u1;
    ssize_t _u2 = write(fd, INDEX_HTML, strlen(INDEX_HTML)); (void)_u2;
    close(fd);
}


// ── HTTP 404 ─────────────────────────────────────────────────────────────────
static void serve_404(int fd) {
    const char *body = "Not Found";
    char hdr[256];
    int nh = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", strlen(body));
    ssize_t _u1 = write(fd, hdr, nh); (void)_u1;
    ssize_t _u2 = write(fd, body, strlen(body)); (void)_u2;
    close(fd);
}


// ── Case-insensitive substring check ─────────────────────────────────────────
static bool has_header(const char *buf, const char *header) {
    // Skip past "GET /... HTTP/1.1\r\n" to find header lines
    const char *p = buf;
    while (*p) {
        // Compare prefix case-insensitively
        const char *h = header;
        const char *b = p;
        while (*h && *b && (tolower((unsigned char)*b) == tolower((unsigned char)*h))) {
            b++; h++;
        }
        if (!*h) return true; // full header name matched
        // Advance to next line
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return false;
}


// ── Read until end of HTTP headers (\r\n\r\n) ───────────────────────────────
// Returns true if headers were fully read, buf will be null-terminated
static bool read_http_headers(int fd, char *buf, size_t size) {
    size_t pos = 0;
    while (pos < size - 1) {
        ssize_t n = read(fd, buf + pos, 1);
        if (n <= 0) return false;
        pos++;
        buf[pos] = '\0';
        // Check for \r\n\r\n (end of headers)
        if (pos >= 4 &&
            buf[pos-1] == '\n' &&
            buf[pos-2] == '\r' &&
            buf[pos-3] == '\n' &&
            buf[pos-4] == '\r')
            return true;
    }
    return false; // buffer full without finding headers
}


// ── Accept and handle an HTTP/WS client connection ───────────────────────────
static void handle_web_client(int client_fd, uint16_t udp_port) {
    char buf[8192];
    if (!read_http_headers(client_fd, buf, sizeof(buf))) {
        close(client_fd);
        return;
    }

    // Check if it's a WebSocket upgrade request (case-insensitive headers)
    if (has_header(buf, "upgrade: websocket") &&
        has_header(buf, "sec-websocket-key:")) {
        if (ws_upgrade(client_fd, buf))
            handle_ws_client(client_fd, udp_port);
        else
            close(client_fd);
        return;
    }

    // Otherwise serve HTTP
    if (strstr(buf, "GET / ") != nullptr || strstr(buf, "GET /index.html ") != nullptr)
        serve_http(client_fd, buf);
    else
        serve_404(client_fd);
}


// ── Web Server Thread ────────────────────────────────────────────────────────
static void web_server_thread(int web_port, uint16_t udp_port) {
    int srv = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv < 0) { perror("web socket"); return; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(web_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("web bind"); close(srv); return; }
    if (listen(srv, 8) < 0) { perror("web listen"); close(srv); return; }

    std::printf("[web] HTTP + WebSocket server listening on port %d\n", web_port);

    std::vector<pollfd> pfds;
    pfds.push_back({srv, POLLIN, 0});

    while (g_running.load(std::memory_order_relaxed)) {
        int rc = poll(pfds.data(), pfds.size(), 200);
        if (rc <= 0) continue;

        if (pfds[0].revents & POLLIN) {
            int client = accept(srv, nullptr, nullptr);
            if (client >= 0)
                std::thread(handle_web_client, client, udp_port).detach();
        }
    }

    close(srv);
    std::printf("[web] server stopped\n");
}


// ══════════════════════════════════════════════════════════════════════════════
// ── UDP receive loop (main thread) ────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
    std::string bind_addr = "0.0.0.0";
    bool        do_upnp   = false;
    int         web_port  = 0; // 0 = disabled

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if      (a == "-p" && i+1 < argc) port      = (uint16_t)std::atoi(argv[++i]);
        else if (a == "-b" && i+1 < argc) bind_addr = argv[++i];
        else if (a == "-v")               g_verbose  = true;
        else if (a == "--upnp")           do_upnp    = true;
        else if (a == "-w") {
            if (i+1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
                web_port = std::atoi(argv[++i]);
            else
                web_port = 8080;
        }
        else if (a == "-h") {
            puts("ns-backend  [-p PORT] [-b ADDR] [--upnp] [-w [WEB_PORT]] [-v]");
            return 0;
        }
    }

    derive_key(DEFAULT_SECRET, g_hmac_key);
    signal(SIGINT,  on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (do_upnp) upnp_add_mapping(port);

    // Start web server if requested
    std::thread web_thread;
    if (web_port > 0)
        web_thread = std::thread(web_server_thread, web_port, port);

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int rbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }
    
    std::printf("UDP %s:%u writer=%d Hz\n",
                bind_addr.c_str(), port, WRITER_HZ);

    std::thread wt(writer_thread, WRITER_HZ);
    std::thread st(stats_thread);

    int ep = epoll_create1(0); epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = sock; epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);

    Packet pkt{};
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 4, 200 /*ms timeout*/);
        if (n <= 0) continue;

        sockaddr_in sender{};
        socklen_t slen = sizeof(sender);
        ssize_t bytes = recvfrom(sock, &pkt, sizeof(pkt), 0, (sockaddr*)&sender, &slen);

        if (bytes != (ssize_t)PACKET_SIZE) continue;

        // ── 1. Per-IP rate limiter ────────────────────────────────────────────────
        uint32_t src_ip = sender.sin_addr.s_addr;
        if (!rate_allow(src_ip)) {
            if (g_verbose) puts("rate limit exceeded, dropped");
            continue;
        }

        // ── 2. Magic + version check ──────────────────────────────────────────────
        if (!packet_ok(pkt)) {
            if (g_verbose) puts("bad magic/version, dropped");
            continue;
        }

        // ── 3. Find Client Session or Pin new IP ──────────────────────────────────
        int client_idx = -1;
        uint64_t now = now_us();
        
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (g_clients[i].active &&
                g_clients[i].addr.sin_addr.s_addr == src_ip &&
                g_clients[i].addr.sin_port == sender.sin_port) {
                client_idx = i;
                break;
            }
        }

        // If not found, assign to a free/timed-out slot
        if (client_idx == -1) {
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (!g_clients[i].active || (now - g_clients[i].last_rx_us > WATCHDOG_MS * 1000ULL)) {
                    client_idx = i;
                    g_clients[i].active = true;
                    g_clients[i].addr = sender;
                    g_clients[i].first_pkt = true;
                    g_clients[i].report.reset();
                    if (g_verbose) std::printf("New PC accepted into Server Slot %d/4\n", i+1);
                    break;
                }
            }
        }

        // If all 4 slots are taken by active PCs, drop the packet
        if (client_idx == -1) {
            if (g_verbose) puts("server is full (4 PCs already active), dropped");
            continue;
        }

        // ── 4. HMAC authentication ────────────────────────────────────────────────
        if (hmac_verify(g_hmac_key, 32, (const uint8_t *)&pkt, PACKET_AUTH_SIZE, pkt.hmac, HMAC_TAG_SIZE) != 0) {
            if (g_verbose) puts("bad HMAC, dropped");
            continue;
        }

        // ── 5. Sequence counter (Anti-Replay) ─────────────────────────────────────
        bool is_reset = (pkt.flags & FLAG_RESET);
        bool sequence_jump = (g_clients[client_idx].expected_seq > pkt.seq) && ((g_clients[client_idx].expected_seq - pkt.seq) > 100);

        if (!g_clients[client_idx].first_pkt && pkt.seq < g_clients[client_idx].expected_seq && !is_reset && !sequence_jump) {
            if (g_verbose)
                std::printf("PC %d out-of-order seq=%u, dropped\n", client_idx+1, pkt.seq);
            continue;
        }
        g_clients[client_idx].first_pkt = false;
        g_clients[client_idx].expected_seq = pkt.seq + 1;

        // ── 6. Apply to shared state ──────────────────────────────────────────────
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (is_reset) {
                g_clients[client_idx].report.reset();
            } else {
                g_clients[client_idx].report = pkt.report;
            }
            g_clients[client_idx].last_rx_us = now_us();
        }
        ++g_pkts_rx;
    }

    puts("[backend] shutting down");
    upnp_remove_mapping(port);
    close(ep); close(sock);
    wt.join(); st.join();
    if (web_thread.joinable()) web_thread.join();
    return 0;
}
