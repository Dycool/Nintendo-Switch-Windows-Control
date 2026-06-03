/// ns-gamepad.cpp  —  Linux frontend for the Switch wireless gamepad bridge
///
/// Uses SDL2 GameController API for cross-platform controller support.
/// Automatically detects Xbox, PlayStation, and generic controllers
/// via USB or Bluetooth — no raw joystick API needed.
///
/// Build:
///   g++ -O3 -std=c++17 ns-gamepad.cpp -o ns-gamepad -lpthread -lSDL2
///
/// Usage:
///   ./ns-gamepad <RASPBERRY_PI_IP[:PORT]>

#include <iostream>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <atomic>
#include <csignal>
#include <string>

#include <SDL2/SDL.h>

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
//  Shared gamepad state (SDL2)
// ─────────────────────────────────────────────────────────────────────────────

static SDL_GameController* g_pads[4] = {nullptr, nullptr, nullptr, nullptr};

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
//  SDL2 Discovery & Input
// ─────────────────────────────────────────────────────────────────────────────

void scan_for_gamepads() {
    static uint64_t last_scan = 0;
    uint64_t now = ns::now_us();

    SDL_GameControllerUpdate();

    if (now - last_scan < 1'000'000) return;
    last_scan = now;

    static bool no_controllers_printed = false;
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num; ++i) {
        if (!SDL_IsGameController(i)) continue;

        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool found = false;
        for (int p = 0; p < 4; ++p) {
            if (g_pads[p]) {
                SDL_Joystick* js = SDL_GameControllerGetJoystick(g_pads[p]);
                if (js && SDL_JoystickInstanceID(js) == id) {
                    found = true;
                    break;
                }
            }
        }
        if (found) continue;

        for (int p = 0; p < 4; ++p) {
            if (!g_pads[p]) {
                SDL_GameController* pad = SDL_GameControllerOpen(i);
                if (!pad) break;
                g_pads[p] = pad;
                const char* name = SDL_GameControllerName(pad);
                std::cout << "Mapped '" << (name ? name : "Unknown") << "' to local slot P" << (p + 1) << "\n";
                break;
            }
        }
    }
    if (num == 0) {
        if (!no_controllers_printed) {
            std::cout << "No controllers detected — waiting for connections...\n";
            no_controllers_printed = true;
        }
    } else {
        no_controllers_printed = false;
    }
}

void read_pad(int index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    SDL_GameController* pad = g_pads[index];
    if (!pad) { conn = false; return; }

    if (!SDL_GameControllerGetAttached(pad)) {
        std::cout << "Controller in slot P" << (index + 1) << " disconnected.\n";
        SDL_GameControllerClose(pad);
        g_pads[index] = nullptr;
        conn = false;
        return;
    }

    conn = true;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) rep.buttons |= ns::BTN_B;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) rep.buttons |= ns::BTN_A;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) rep.buttons |= ns::BTN_Y;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) rep.buttons |= ns::BTN_X;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) rep.buttons |= ns::BTN_L;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) rep.buttons |= ns::BTN_R;

    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16000) rep.buttons |= ns::BTN_ZL;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) rep.buttons |= ns::BTN_ZR;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK))    rep.buttons |= ns::BTN_MINUS;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START))   rep.buttons |= ns::BTN_PLUS;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK))  rep.buttons |= ns::BTN_LSTICK;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) rep.buttons |= ns::BTN_RSTICK;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_GUIDE))   rep.buttons |= ns::BTN_HOME;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
        rep.buttons |= ns::BTN_HOME; rep.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
    }
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK) && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) {
        rep.buttons |= ns::BTN_CAPTURE; rep.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);
    }

    // D-Pad
    bool up    = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
    bool down  = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    bool left  = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    bool right = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    rep.hat = ns::HAT_NEUTRAL;
    if (up && right) rep.hat = ns::HAT_NE; else if (up && left) rep.hat = ns::HAT_NW;
    else if (down && right) rep.hat = ns::HAT_SE; else if (down && left) rep.hat = ns::HAT_SW;
    else if (up) rep.hat = ns::HAT_N; else if (down) rep.hat = ns::HAT_S;
    else if (left) rep.hat = ns::HAT_W; else if (right) rep.hat = ns::HAT_E;

    // Analog sticks — SDL sets UP as negative, Switch expects UP as positive
    int16_t lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    int16_t rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
    int16_t ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);

    rep.lx = apply_deadzone(lx, false);
    rep.ly = apply_deadzone(ly, false);
    rep.rx = apply_deadzone(rx, false);
    rep.ry = apply_deadzone(ry, false);
}


// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]>\n";
        return 1;
    }

    std::string host = argv[1];
    int port = ns::DEFAULT_PORT;
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        port = std::atoi(host.c_str() + colon + 1);
        if (port < 1 || port > 65535) {
            std::cerr << "Invalid port: " << port << " (must be 1–65535)\n";
            return 1;
        }
        host.resize(colon);
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
    
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%d", port);
    
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        std::cerr << "Cannot resolve address: " << host << "\n";
        close(sock); return 1;
    }
    
    sockaddr_in dest{};
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    // Initialise SDL2 GameController subsystem
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        std::cerr << "Failed to initialise SDL2: " << SDL_GetError() << "\n";
        close(sock); return 1;
    }

    std::cout << "Started... Press Ctrl+C to stop\n";

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
    for (int i = 0; i < 4; ++i) {
        if (g_pads[i]) { SDL_GameControllerClose(g_pads[i]); g_pads[i] = nullptr; }
    }
    SDL_Quit();
    close(sock);
    return 0;
}