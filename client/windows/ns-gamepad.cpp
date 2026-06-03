#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>           
#include <winsock2.h>          
#include <ws2tcpip.h>          
#include <xinput.h>            

#include <iostream>            
#include <chrono>              
#include <cstdint>             
#include <cstdlib>             
#include <thread>              
#include <algorithm>           
#include <string>
#include "../../server/rpi/include/sha256.h"

// Import external protocol structures
#include "../../server/rpi/include/protocol.hpp"

// Applies deadzone to an analog stick axis
uint8_t apply_deadzone(SHORT val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    else                 scaled = 128 - ((abs(val) - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

// Maps XInput layout to Switch Pro Controller layout
ns::HIDReport map_xinput_to_switch(const XINPUT_GAMEPAD& pad) {
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

    // Emulate HOME and CAPTURE buttons using button combos
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

// ── XInput Throttling (Prevents USB driver crash on Windows) ──
static uint64_t g_last_check_us[4] = {0, 0, 0, 0};
static bool g_is_connected[4] = {false, false, false, false};

// Checks for controller status efficiently and maps its inputs
void fetch_pad_throttled(DWORD index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    uint64_t now = ns::now_us();
    
    // Only poll disconnected controllers once per second to save CPU and USB bandwidth
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

int main(int argc, char** argv) {
    std::string host = ""; uint16_t port = ns::DEFAULT_PORT;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" && i+1 < argc) port = (uint16_t)std::atoi(argv[++i]);
        else if (host.empty()) host = arg;
    }

    if (host.empty()) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP> [-p PORT]\n";
        return 1;
    }

    uint8_t hmac_key[32]; derive_key(ns::DEFAULT_SECRET, hmac_key);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa); 
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; 

    char port_buf[8]; snprintf(port_buf, sizeof(port_buf), "%u", port);
    if (getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0 || res == nullptr) {
        std::cerr << "ERROR: Unable to resolve IP: " << host << "\n";
        WSACleanup(); return 1;
    }
    
    sockaddr_in dest{}; memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    std::cout << "Started as a Multi-Client Node... Connect Xbox controllers and enjoy!\n";
    uint32_t seq = 0;

    while (true) {
        ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet)); 
        pkt.magic   = ns::PROTO_MAGIC;
        pkt.version = ns::PROTO_VERSION;
        pkt.flags   = ns::FLAG_NONE;
        pkt.seq     = seq++;
        pkt.ts_us   = ns::now_us();

        // VERY IMPORTANT: Initialize all 4 slots to hardware-neutral (128 for sticks, 8 for hat)
        // If left at pure 0 from memset, the Pi reads it as analog sticks pushed Top-Left!
        pkt.report.reset();

        ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
        int active_count = 0;
        
        // Scan all 4 XInput slots and assign to fixed physical slot
        for (DWORD i = 0; i < 4; ++i) {
            ns::HIDReport temp_rep;
            bool is_conn = false;
            fetch_pad_throttled(i, temp_rep, is_conn);
            
            if (is_conn) {
                *out_reports[i] = temp_rep; // Slot matches physical controller
                active_count++;
            }
        }

        // Sign packet
        {
            uint8_t full_hmac[32];
            hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        }

        sendto(sock, (const char*)&pkt, (int)ns::PACKET_SIZE, 0, (const sockaddr*)&dest, sizeof(dest));
        
        // Sleep to throttle transmission (~500Hz when active, 2Hz when idle)
        if (active_count > 0) std::this_thread::sleep_for(std::chrono::milliseconds(2)); 
        else std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    closesocket(sock); WSACleanup(); 
    return 0;
}