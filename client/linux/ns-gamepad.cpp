/// ns-gamepad.cpp  —  Linux frontend for the Switch wireless gamepad bridge
///
/// Build:
///   g++ -O3 -std=c++17 ns-gamepad.cpp -o ns-gamepad -lpthread -lSDL2
///
/// Usage:
///   ./ns-gamepad <RASPBERRY_PI_IP[:PORT]> [-k [single|override]]

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
#include <vector>
#include <unordered_map>

#include <SDL2/SDL.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/resource.h>
#include <linux/input.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>

// Undefine colliding macros from <linux/input.h> before importing the protocol
#undef BTN_A
#undef BTN_B
#undef BTN_X
#undef BTN_Y
#undef BTN_L
#undef BTN_R
#undef BTN_ZL
#undef BTN_ZR
#undef BTN_MINUS
#undef BTN_PLUS
#undef BTN_LSTICK
#undef BTN_RSTICK
#undef BTN_HOME
#undef BTN_CAPTURE

#include "../../server/rpi/include/sha256.h"
#include "../../server/rpi/include/protocol.hpp"

// ── Global State ──
static SDL_GameController* g_pads[4] = {nullptr, nullptr, nullptr, nullptr};
static int keyboard_mode = 0; // 0=off, 1=single, 2=override
static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ── Background Keyboard Hardware Polling (/dev/input/) ──
#define BITS_PER_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define test_bit(bit, array) ((array[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

static std::vector<int> g_kb_fds;

void init_global_keyboard() {
    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) == 0) {
            char path[256];
            snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                unsigned long evbit[NLONGS(EV_MAX)];
                ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit);
                if (test_bit(EV_KEY, evbit)) {
                    unsigned long keybit[NLONGS(KEY_MAX)];
                    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
                    if (test_bit(KEY_A, keybit)) { // If it has 'A', it's likely a keyboard
                        g_kb_fds.push_back(fd);
                    } else close(fd);
                } else close(fd);
            }
        }
    }
    closedir(dir);
}

bool is_key_down_global(int linux_key_code) {
    if (linux_key_code == KEY_RESERVED) return false;
    for (int fd : g_kb_fds) {
        unsigned long keystate[NLONGS(KEY_MAX)] = {0};
        if (ioctl(fd, EVIOCGKEY(sizeof(keystate)), keystate) != -1) {
            if (test_bit(linux_key_code, keystate)) return true;
        }
    }
    return false;
}

int name_to_linux_key(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);
    if (name.length() == 1 && name[0] >= 'A' && name[0] <= 'Z') return KEY_A + (name[0] - 'A');
    if (name.length() == 1 && name[0] >= '1' && name[0] <= '9') return KEY_1 + (name[0] - '1');
    if (name == "0") return KEY_0;
    
    if (name == "UP") return KEY_UP;
    if (name == "DOWN") return KEY_DOWN;
    if (name == "LEFT") return KEY_LEFT;
    if (name == "RIGHT") return KEY_RIGHT;
    if (name == "SHIFT_L" || name == "LSHIFT" || name == "LEFT SHIFT") return KEY_LEFTSHIFT;
    if (name == "SHIFT_R" || name == "RSHIFT" || name == "RIGHT SHIFT") return KEY_RIGHTSHIFT;
    if (name == "CONTROL_L" || name == "LCTRL") return KEY_LEFTCTRL;
    if (name == "CONTROL_R" || name == "RCTRL") return KEY_RIGHTCTRL;
    if (name == "ALT_L" || name == "LALT") return KEY_LEFTALT;
    if (name == "ALT_R" || name == "RALT") return KEY_RIGHTALT;
    if (name == "SPACE") return KEY_SPACE;
    if (name == "RETURN" || name == "ENTER") return KEY_ENTER;
    if (name == "TAB") return KEY_TAB;
    if (name == "ESCAPE" || name == "ESC") return KEY_ESC;
    if (name == "BACKSPACE") return KEY_BACKSPACE;
    if (name == "HOME") return KEY_HOME;
    if (name == "PRINT" || name == "SNAPSHOT" || name == "PRINTSCREEN") return KEY_SYSRQ;
    
    return KEY_RESERVED;
}

// ── Gamepad Logic ──
uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    else scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

void scan_for_gamepads() {
    static uint64_t last_scan = 0;
    uint64_t now = ns::now_us();
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
                if (js && SDL_JoystickInstanceID(js) == id) { found = true; break; }
            }
        }
        if (found) continue;

        for (int p = 0; p < 4; ++p) {
            if (!g_pads[p]) {
                SDL_GameController* pad = SDL_GameControllerOpen(i);
                if (!pad) break;
                g_pads[p] = pad;
                const char* name = SDL_GameControllerName(pad);
                int slot = p + 1;
                if (keyboard_mode == 1 && p == 0) {
                    if (!g_pads[1]) slot = 2; else if (!g_pads[2]) slot = 3; else slot = 4;
                }
                std::cout << "Mapped '" << (name ? name : "Unknown") << "' to local slot P" << slot << "\n";
                break;
            }
        }
    }
    if (num == 0 && keyboard_mode == 0) {
        if (!no_controllers_printed) {
            std::cout << "No controllers detected — waiting for connections...\n";
            no_controllers_printed = true;
        }
    } else no_controllers_printed = false;
}

void read_pad(int index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    SDL_GameController* pad = g_pads[index];
    if (!pad) { conn = false; return; }
    if (!SDL_GameControllerGetAttached(pad)) {
        std::cout << "Controller in slot P" << (index + 1) << " disconnected.\n";
        SDL_GameControllerClose(pad);
        g_pads[index] = nullptr;
        conn = false; return;
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
    
    bool up = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
    bool down = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    bool left = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    bool right = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    
    rep.hat = ns::HAT_NEUTRAL;
    if (up && right) rep.hat = ns::HAT_NE; else if (up && left) rep.hat = ns::HAT_NW;
    else if (down && right) rep.hat = ns::HAT_SE; else if (down && left) rep.hat = ns::HAT_SW;
    else if (up) rep.hat = ns::HAT_N; else if (down) rep.hat = ns::HAT_S;
    else if (left) rep.hat = ns::HAT_W; else if (right) rep.hat = ns::HAT_E;
    
    rep.lx = apply_deadzone(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), false);
    rep.ly = apply_deadzone(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY), false);
    rep.rx = apply_deadzone(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX), false);
    rep.ry = apply_deadzone(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY), false);
}

// ── Keyboard Support ──
struct KeyBindings {
    std::unordered_map<std::string, std::string> map;
    int mode = 0; 
    static std::unordered_map<std::string, std::string> defaults() {
        return {
            {"Y","Z"}, {"B","X"}, {"A","V"}, {"X","C"},
            {"L","Q"}, {"R","E"}, {"ZL","1"}, {"ZR","2"},
            {"MINUS","3"}, {"PLUS","4"},
            {"LSTICK","SHIFT_L"}, {"RSTICK","SHIFT_R"},
            {"HOME","HOME"}, {"CAPTURE","PRINT"},
            {"LSTICK_UP","W"}, {"LSTICK_DOWN","S"},
            {"LSTICK_LEFT","A"}, {"LSTICK_RIGHT","D"},
            {"RSTICK_UP","I"}, {"RSTICK_DOWN","K"},
            {"RSTICK_LEFT","J"}, {"RSTICK_RIGHT","L"},
            {"DPAD_UP","UP"}, {"DPAD_DOWN","DOWN"},
            {"DPAD_LEFT","LEFT"}, {"DPAD_RIGHT","RIGHT"}
        };
    }
    std::string get_bindings_path() const {
        char buf[1024]; ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0'; std::string p(buf); size_t pos = p.find_last_of("/");
            return (pos != std::string::npos ? p.substr(0, pos) : ".") + "/bindings.json";
        }
        return "bindings.json";
    }
    void load_or_create() {
        std::string path = get_bindings_path();
        FILE* f = fopen(path.c_str(), "r");
        if (f) {
            fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
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
                std::string json = "{\n"; size_t i = 0;
                for (auto& [k, v] : map) {
                    json += "    \"" + k + "\": \"" + v + "\"";
                    if (++i < map.size()) json += ",";
                    json += "\n";
                }
                json += "}\n";
                fputs(json.c_str(), f); fclose(f);
            }
        }
    }
    void apply(ns::HIDReport& rep) const {
        auto is_down = [&](const std::string& btn) -> bool {
            auto it = map.find(btn);
            if (it == map.end() || it->second.empty()) return false;
            return is_key_down_global(name_to_linux_key(it->second));
        };

        if (is_down("Y")) rep.buttons |= ns::BTN_Y;
        if (is_down("B")) rep.buttons |= ns::BTN_B;
        if (is_down("A")) rep.buttons |= ns::BTN_A;
        if (is_down("X")) rep.buttons |= ns::BTN_X;
        if (is_down("L")) rep.buttons |= ns::BTN_L;
        if (is_down("R")) rep.buttons |= ns::BTN_R;
        if (is_down("ZL")) rep.buttons |= ns::BTN_ZL;
        if (is_down("ZR")) rep.buttons |= ns::BTN_ZR;
        if (is_down("MINUS")) rep.buttons |= ns::BTN_MINUS;
        if (is_down("PLUS")) rep.buttons |= ns::BTN_PLUS;
        if (is_down("LSTICK")) rep.buttons |= ns::BTN_LSTICK;
        if (is_down("RSTICK")) rep.buttons |= ns::BTN_RSTICK;
        if (is_down("HOME")) rep.buttons |= ns::BTN_HOME;
        if (is_down("CAPTURE")) rep.buttons |= ns::BTN_CAPTURE;

        bool up = is_down("DPAD_UP"), down = is_down("DPAD_DOWN");
        bool left = is_down("DPAD_LEFT"), right = is_down("DPAD_RIGHT");
        if (up && right) rep.hat = ns::HAT_NE; else if (up && left) rep.hat = ns::HAT_NW;
        else if (down && right) rep.hat = ns::HAT_SE; else if (down && left) rep.hat = ns::HAT_SW;
        else if (up) rep.hat = ns::HAT_N; else if (down) rep.hat = ns::HAT_S;
        else if (left) rep.hat = ns::HAT_W; else if (right) rep.hat = ns::HAT_E;

        bool lsu = is_down("LSTICK_UP"), lsd = is_down("LSTICK_DOWN");
        bool lsl = is_down("LSTICK_LEFT"), lsr = is_down("LSTICK_RIGHT");
        if (lsl && !lsr) rep.lx = 0; else if (lsr && !lsl) rep.lx = 255; else if (mode != 2) rep.lx = 128;
        if (lsu && !lsd) rep.ly = 0; else if (lsd && !lsu) rep.ly = 255; else if (mode != 2) rep.ly = 128;

        bool rsu = is_down("RSTICK_UP"), rsd = is_down("RSTICK_DOWN");
        bool rsl = is_down("RSTICK_LEFT"), rsr = is_down("RSTICK_RIGHT");
        if (rsl && !rsr) rep.rx = 0; else if (rsr && !rsl) rep.rx = 255; else if (mode != 2) rep.rx = 128;
        if (rsu && !rsd) rep.ry = 0; else if (rsd && !rsu) rep.ry = 255; else if (mode != 2) rep.ry = 128;
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP[:PORT]> [-k [single|override]]\n";
        return 1;
    }
    std::string host; int port = ns::DEFAULT_PORT;
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
                if (port < 1 || port > 65535) return 1;
                host.resize(colon);
            }
        }
    }
    if (host.empty()) return 1;

    KeyBindings kb;
    if (keyboard_mode) {
        init_global_keyboard();
        if (g_kb_fds.empty()) {
            std::cerr << "\n*** WARNING ***\nCannot read /dev/input/ devices.\n";
            std::cerr << "To use the keyboard in the background, you MUST run:\n";
            std::cerr << "sudo usermod -aG input $USER\n(Then log out and log back in).\n\n";
        }
        kb.load_or_create(); kb.mode = keyboard_mode;
        std::cout << "Keyboard mode enabled (" << (keyboard_mode == 1 ? "single" : "override") << ")\n";
    }

    uint8_t hmac_key[32]; derive_key(ns::DEFAULT_SECRET, hmac_key);
    signal(SIGINT,  on_signal); signal(SIGTERM, on_signal);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%d", port);
    
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) return 1;
    sockaddr_in dest{}; std::memcpy(&dest, res->ai_addr, sizeof(dest)); freeaddrinfo(res);

    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) return 1;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    std::cout << "Started... Press Ctrl+C to stop\n";
    setpriority(PRIO_PROCESS, 0, -20);

    uint32_t seq = 0;
    auto next_tick = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        while (std::chrono::steady_clock::now() < next_tick) std::atomic_thread_fence(std::memory_order_relaxed);
        
        SDL_Event e; while (SDL_PollEvent(&e)) {}
        scan_for_gamepads();

        ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet)); 
        pkt.magic = ns::PROTO_MAGIC; pkt.version = ns::PROTO_VERSION;
        pkt.flags = ns::FLAG_NONE; pkt.seq = seq++; pkt.ts_us = ns::now_us();
        pkt.report.reset(); 

        ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
        int active_count = 0; bool c1 = false, c2 = false, c3 = false, c4 = false;

        for (int i = 0; i < 4; ++i) {
            bool is_conn = false;
            read_pad(i, *out_reports[i], is_conn);
            if (is_conn) {
                active_count++;
                if (i == 0) c1 = true; else if (i == 1) c2 = true; else if (i == 2) c3 = true; else if (i == 3) c4 = true;
            }
        }

        if (kb.mode == 1) {
            if (c1) {
                if (!c2) { *out_reports[1] = *out_reports[0]; c2 = true; active_count++; }
                else if (!c3) { *out_reports[2] = *out_reports[0]; c3 = true; active_count++; }
                else if (!c4) { *out_reports[3] = *out_reports[0]; c4 = true; active_count++; }
            }
            out_reports[0]->reset(); kb.apply(*out_reports[0]);
            active_count = std::max(active_count, 1);
        } else if (kb.mode == 2) {
            kb.apply(*out_reports[0]);
            active_count = std::max(active_count, 1);
        }

        uint8_t full_hmac[32];
        hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
        memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        sendto(sock, (const char*)&pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
        
        if (active_count > 0) next_tick += std::chrono::milliseconds(4);
        else next_tick += std::chrono::milliseconds(500);
    }

    std::cout << "\nShutting down...\n";
    for (int i = 0; i < 4; ++i) if (g_pads[i]) SDL_GameControllerClose(g_pads[i]);
    for (int fd : g_kb_fds) close(fd);
    SDL_Quit(); close(sock);
    return 0;
}