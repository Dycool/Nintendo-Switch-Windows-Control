#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>           
#include <mmsystem.h>          
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
#include <vector>
#include <unordered_map>
#include <cstdio>
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
static int keyboard_mode = 0; // 0=off, 1=single, 2=override

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
        if (g_is_connected[index])
            std::cout << "Controller in slot P" << (index + 1) << " disconnected.\n";
        g_is_connected[index] = false;
        g_last_check_us[index] = now;
        conn = false; return;
    }
    
    if (!g_is_connected[index]) {
        int slot = index + 1;
        if (keyboard_mode == 1 && index == 0) {
            if (!g_is_connected[1]) slot = 2;
            else if (!g_is_connected[2]) slot = 3;
            else slot = 4;
        }
        std::cout << "Mapped 'Xbox controller' to local slot P" << slot << "\n";
    }
    g_is_connected[index] = true; conn = true;
    rep = map_xinput_to_switch(state.Gamepad);
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
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string p(path);
        size_t pos = p.find_last_of("\\/");
        return (pos != std::string::npos ? p.substr(0, pos) : ".") + "\\bindings.json";
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

    // Poll keyboard and fill HIDReport for player 1
    void apply(ns::HIDReport& rep) const {
        // Helper: check if a named key is pressed
        auto is_down = [](const std::string& name) -> bool {
            // Letter keys
            if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
                return GetAsyncKeyState(name[0]) & 0x8000;
            if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
                return GetAsyncKeyState(name[0]) & 0x8000;
            // Named keys
            struct KeyMap { const char* n; int vk; };
            static const KeyMap kmap[] = {
                {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
                {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT},
                {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
                {"LALT", VK_LMENU}, {"RALT", VK_RMENU},
                {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"TAB", VK_TAB},
                {"ESC", VK_ESCAPE}, {"BACKSPACE", VK_BACK},
                {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
                {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
                {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
                {"HOME", VK_HOME}, {"SNAPSHOT", VK_SNAPSHOT},
            };
            for (auto& km : kmap)
                if (name == km.n) return GetAsyncKeyState(km.vk) & 0x8000;
            // Number key alternative: '0'..'9'  
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

        // D-Pad
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

int main(int argc, char** argv) {
    timeBeginPeriod(1);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]]\n";
        std::cerr << "  -k  Enable keyboard mode (default: single)\n";
        timeEndPeriod(1); return 1;
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
                    timeEndPeriod(1); return 1;
                }
                host.resize(colon);
            }
        }
    }

    if (host.empty()) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k]\n";
        timeEndPeriod(1); return 1;
    }

    KeyBindings kb;
    if (keyboard_mode) {
        kb.load_or_create();
        kb.mode = keyboard_mode;
        std::cout << "Keyboard mode enabled (" << (keyboard_mode == 1 ? "single" : "override") << ") - ";
        std::cout << (keyboard_mode == 1 ? "replaces" : "augments") << " Player 1\n";
    }

    uint8_t hmac_key[32]; derive_key(ns::DEFAULT_SECRET, hmac_key);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    WSADATA wsa{}; WSAStartup(MAKEWORD(2, 2), &wsa); 
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; 

    char port_buf[8]; snprintf(port_buf, sizeof(port_buf), "%d", port);
    if (getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0 || res == nullptr) {
        std::cerr << "ERROR: Unable to resolve IP: " << host << "\n";
        timeEndPeriod(1); WSACleanup(); return 1;
    }
    
    sockaddr_in dest{}; memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    std::cout << "Started... Press Ctrl+C to stop\n";
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
        
        static bool no_controllers_printed = false;
        
        // Scan all 4 XInput slots and assign to fixed physical slot
        bool c1 = false, c2 = false, c3 = false, c4 = false;
        for (DWORD i = 0; i < 4; ++i) {
            ns::HIDReport temp_rep;
            bool is_conn = false;
            fetch_pad_throttled(i, temp_rep, is_conn);
            
            if (is_conn) {
                *out_reports[i] = temp_rep; // Slot matches physical controller
                active_count++;
                if (i == 0) c1 = true;
                else if (i == 1) c2 = true;
                else if (i == 2) c3 = true;
                else if (i == 3) c4 = true;
            }
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

        // Sign packet
        {
            uint8_t full_hmac[32];
            hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        }

        sendto(sock, (const char*)&pkt, (int)ns::PACKET_SIZE, 0, (const sockaddr*)&dest, sizeof(dest));
        
        // Sleep to throttle transmission (~250Hz when active, 2Hz when idle)
        if (active_count > 0) {
            no_controllers_printed = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        } else {
            if (!no_controllers_printed) {
                std::cout << "No controllers detected - waiting for connections...\n";
                no_controllers_printed = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    std::cout << "\nShutting down...\n";
    closesocket(sock); WSACleanup(); 
    timeEndPeriod(1); return 0;
}