// Minimize Windows.h includes to reduce compilation time and avoid namespace pollution
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
#include "sha256.h"            

namespace ns {

static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;  
static constexpr uint8_t  PROTO_VERSION = 2;             
static constexpr uint16_t DEFAULT_PORT  = 7331;          
static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";

// ── 1. HORI Controller Structs ──
enum Button : uint16_t {
    BTN_Y=1<<0, BTN_B=1<<1, BTN_A=1<<2, BTN_X=1<<3,      
    BTN_L=1<<4, BTN_R=1<<5, BTN_ZL=1<<6, BTN_ZR=1<<7,    
    BTN_MINUS=1<<8, BTN_PLUS=1<<9, BTN_LSTICK=1<<10, BTN_RSTICK=1<<11,  
    BTN_HOME=1<<12, BTN_CAPTURE=1<<13,                    
};

enum Hat : uint8_t {
    HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3,
    HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8,
};

#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0;
    uint8_t  hat     = HAT_NEUTRAL;
    uint8_t  lx      = 128;
    uint8_t  ly      = 128;
    uint8_t  rx      = 128;
    uint8_t  ry      = 128;
    uint8_t  vendor  = 0;
    
    void reset() noexcept { buttons = 0; hat = HAT_NEUTRAL; lx = ly = rx = ry = 128; vendor = 0; }
};

// ── 2. GameCube Structs ──
struct GCController {
    uint8_t status = 0x00, btn1 = 0, btn2 = 0;
    uint8_t stick_x = 128, stick_y = 128, cstick_x = 128, cstick_y = 128;
    uint8_t l_analog = 0, r_analog = 0;
};

struct GCHubReport {
    uint8_t      id = 0x21;
    GCController p1, p2, p3, p4;

    void reset() noexcept { 
        id = 0x21;
        p1 = GCController{}; p2 = GCController{}; p3 = GCController{}; p4 = GCController{};
    }
};

// ── 3. Wire Packet ──
enum Flags : uint8_t { FLAG_NONE=0x00, FLAG_RESET=0x01, FLAG_AUTOFIRE=0x02 };
static constexpr size_t HMAC_TAG_SIZE = 16;

struct Packet {
    uint32_t  magic;
    uint8_t   version;
    uint8_t   flags;
    uint16_t  autofire_mask;
    uint32_t  seq;
    uint64_t  ts_us;
    
    union {
        HIDReport   hori;
        GCHubReport gc;
    } payload;
    
    uint8_t   hmac[HMAC_TAG_SIZE];
};
#pragma pack(pop)

static constexpr size_t PACKET_SIZE      = sizeof(Packet);
static constexpr size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;

inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace ns

uint8_t apply_deadzone(SHORT val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) {
        scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    } else {
        scaled = 128 - ((abs(val) - deadzone) * 128) / (32768 - deadzone);
    }
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

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

// ── XInput Throttling (PREVENTS WINDOWS CRASHES) ──
static uint64_t g_last_check_us[4] = {0, 0, 0, 0};
static bool g_is_connected[4] = {false, false, false, false};

ns::GCController map_xinput_to_gc(DWORD index, bool& connected_out) {
    ns::GCController gc;
    // Valores neutros seguros
    gc.status = 0x00; gc.btn1 = 0; gc.btn2 = 0;
    gc.stick_x = 128; gc.stick_y = 128; gc.cstick_x = 128; gc.cstick_y = 128;
    gc.l_analog = 0;  gc.r_analog = 0;

    uint64_t now = ns::now_us();

    // BLOQUEIO: Se não estava conectado, testa apenas de 1 em 1 segundo!
    // Isto impede que o Driver USB do Windows crash a aplicação devido a Spam.
    if (!g_is_connected[index] && (now - g_last_check_us[index] < 1'000'000)) {
        connected_out = false;
        return gc; 
    }

    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));

    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
        g_is_connected[index] = false;
        g_last_check_us[index] = now;
        connected_out = false;
        return gc;
    }

    g_is_connected[index] = true;
    connected_out = true;
    gc.status = 0x10; // Conectado!

    const XINPUT_GAMEPAD& pad = state.Gamepad;
    if (pad.wButtons & XINPUT_GAMEPAD_A) gc.btn1 |= (1 << 0);
    if (pad.wButtons & XINPUT_GAMEPAD_B) gc.btn1 |= (1 << 1);
    if (pad.wButtons & XINPUT_GAMEPAD_X) gc.btn1 |= (1 << 2);
    if (pad.wButtons & XINPUT_GAMEPAD_Y) gc.btn1 |= (1 << 3);
    if (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)  gc.btn1 |= (1 << 4);
    if (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) gc.btn1 |= (1 << 5);
    if (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)  gc.btn1 |= (1 << 6);
    if (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP)    gc.btn1 |= (1 << 7);

    if (pad.wButtons & XINPUT_GAMEPAD_START) gc.btn2 |= (1 << 0);
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) gc.btn2 |= (1 << 1); 
    if (pad.bRightTrigger > 128) gc.btn2 |= (1 << 2); 
    if (pad.bLeftTrigger > 128)  gc.btn2 |= (1 << 3); 

    gc.stick_x = apply_deadzone(pad.sThumbLX, false);
    gc.stick_y = apply_deadzone(pad.sThumbLY, false); 
    gc.cstick_x = apply_deadzone(pad.sThumbRX, false);
    gc.cstick_y = apply_deadzone(pad.sThumbRY, false);
    gc.l_analog = pad.bLeftTrigger;
    gc.r_analog = pad.bRightTrigger;

    return gc;
}

int main(int argc, char** argv) {
    std::string host = "";
    uint16_t port    = ns::DEFAULT_PORT;
    bool multiplayer = false;
    DWORD ctrl_index = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-m") {
            multiplayer = true;
        } else if (host.empty()) {
            host = arg;
        } else {
            ctrl_index = (DWORD)std::clamp(std::atoi(arg.c_str()), 0, 3);
        }
    }

    if (host.empty()) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP> [-m] [controller_index]\n";
        return 1;
    }

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa); 

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;     
    hints.ai_socktype = SOCK_DGRAM; 

    char port_buf[8]; snprintf(port_buf, sizeof(port_buf), "%u", port);
    
    // Safety check para evitar crashes se o IP for inválido
    if (getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0 || res == nullptr) {
        std::cerr << "ERRO: Nao foi possivel resolver o IP: " << host << "\n";
        WSACleanup();
        return 1;
    }
    
    sockaddr_in dest{};
    memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    std::cout << "Started in " << (multiplayer ? "GameCube (4-Player)" : "HORI (1-Player)") << " Mode...\n";

    uint32_t seq = 0;

    while (true) {
        ns::Packet pkt;
        // IMPORTANTE: Limpa TODO o lixo de memória da struct/union
        // Sem isto o Windows mistura lixo com a password HMAC e a rede falha!
        memset(&pkt, 0, sizeof(ns::Packet)); 

        pkt.magic   = ns::PROTO_MAGIC;
        pkt.version = ns::PROTO_VERSION;
        pkt.flags   = ns::FLAG_NONE;
        pkt.seq     = seq++;
        pkt.ts_us   = ns::now_us();

        bool any_connected = false;

        if (multiplayer) {
            pkt.payload.gc.id = 0x21;
            bool c1, c2, c3, c4;
            
            pkt.payload.gc.p1 = map_xinput_to_gc(0, c1);
            pkt.payload.gc.p2 = map_xinput_to_gc(1, c2);
            pkt.payload.gc.p3 = map_xinput_to_gc(2, c3);
            pkt.payload.gc.p4 = map_xinput_to_gc(3, c4);
            
            any_connected = (c1 || c2 || c3 || c4);
            
            if (!any_connected) pkt.payload.gc.reset();
        } else {
            XINPUT_STATE state;
            ZeroMemory(&state, sizeof(XINPUT_STATE));
            
            if (XInputGetState(ctrl_index, &state) == ERROR_SUCCESS) {
                pkt.payload.hori = map_xinput_to_switch(state.Gamepad);
                any_connected = true;
            } else {
                pkt.payload.hori.reset();
            }
        }

        {
            uint8_t full_hmac[32];
            hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        }

        sendto(sock, (const char*)&pkt, (int)ns::PACKET_SIZE, 0, (const sockaddr*)&dest, sizeof(dest));
        
        if (any_connected) std::this_thread::sleep_for(std::chrono::milliseconds(2)); 
        else std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    closesocket(sock);
    WSACleanup(); 
    return 0;
}