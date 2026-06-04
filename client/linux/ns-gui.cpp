/// ns-gui.cpp  —  GTK3 GUI frontend for the Switch wireless gamepad bridge
///
/// Features Smart Discovery: Automatically scans js0-js15, ignores mice,
/// and natively supports up to 4 local controllers packed into a single UDP stream.
///
/// Build:
///   g++ -O3 -std=c++17 ns-gui.cpp -o ns-gui \
///       $(pkg-config --cflags --libs gtk+-3.0) -lpthread
///
/// Usage:
///   ./ns-gui

#include <gtk/gtk.h>
#include <glib.h>

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
#include <cctype>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <cstdio>

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

// ── Config path helpers ──
static std::string get_config_dir() {
    const char* home = getenv("HOME");
    if (!home) return ".";
    return std::string(home) + "/.config/ns-pc-control";
}

static std::string load_saved_config(int* outMode) {
    std::string path = get_config_dir() + "/config";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    char ip[256]{}, mode[16]{};
    if (fgets(ip, sizeof(ip), f)) {
        size_t len = strlen(ip);
        if (len > 0 && ip[len-1] == '\n') ip[len-1] = '\0';
    }
    if (fgets(mode, sizeof(mode), f)) {
        size_t len = strlen(mode);
        if (len > 0 && mode[len-1] == '\n') mode[len-1] = '\0';
        if (outMode) *outMode = atoi(mode);
    }
    fclose(f);
    return ip;
}

static void save_config(const char* ip, int kbMode) {
    std::string dir = get_config_dir();
    if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) return;
    std::string path = dir + "/config";
    FILE* f = fopen(path.c_str(), "w");
    if (f) { fprintf(f, "%s\n%d\n", ip, kbMode); fclose(f); }
}

// ── Global state ──
static GtkWidget* ipEntry = nullptr;
static GtkWidget* connectBtn = nullptr;
static GtkWidget* statusLabel = nullptr;
static GtkWidget* ctrlLabels[4]; // Labels to display P1 to P4 status
static GtkWidget* kbCombo = nullptr;
static GtkWidget* bindingsBtn = nullptr;

static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_senderRunning{false};
static std::thread g_senderThread;
static uint8_t g_hmacKey[32]{};

// ── Keyboard Mode ──
enum { KB_OFF = 0, KB_SINGLE = 1, KB_OVERRIDE = 2 };
static std::atomic<int> g_keyboardMode{KB_OFF};
static std::unordered_map<std::string, std::string> g_keyBindings;

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

static std::string get_bindings_path() {
    return get_config_dir() + "/bindings";
}

static void load_bindings() {
    g_keyBindings = default_key_bindings();
    std::string path = get_bindings_path();
    FILE* f = fopen(path.c_str(), "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char* val = eq + 1;
                size_t len = strlen(val);
                if (len > 0 && val[len-1] == '\n') val[len-1] = '\0';
                g_keyBindings[line] = val;
            }
        }
        fclose(f);
    }
}

static void save_bindings() {
    std::string dir = get_config_dir();
    g_mkdir_with_parents(dir.c_str(), 0755);
    std::string path = get_bindings_path();
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        for (auto& [k, v] : g_keyBindings)
            fprintf(f, "%s=%s\n", k.c_str(), v.c_str());
        fclose(f);
    }
}

// ── Shared Gamepad State (SDL2) ──
static SDL_GameController* g_pads[4] = {nullptr, nullptr, nullptr, nullptr};
static char         g_hw_names[4][128]; // Stored safely for the GTK thread to read
static std::mutex   g_hw_mtx;           // Protects hardware string arrays

// Shared reports — written by fast timer (main thread), read by SenderThread
static ns::HIDReport g_shared_reports[4];
static bool          g_shared_connected[4] = {};
static std::mutex    g_report_mtx;

// ── Axis conversion ──
static uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    else scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

// ── SDL2 Discovery & Input ──

static void scan_for_gamepads() {
    static uint64_t last_scan = 0;
    uint64_t now = ns::now_us();

    SDL_GameControllerUpdate();

    if (now - last_scan < 1'000'000) return;
    last_scan = now;

    int num = SDL_NumJoysticks();
    for (int i = 0; i < num; ++i) {
        if (!SDL_IsGameController(i)) continue;

        // Check by instance ID to avoid double-mapping
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

        // Find a free slot
        for (int p = 0; p < 4; ++p) {
            if (!g_pads[p]) {
                SDL_GameController* pad = SDL_GameControllerOpen(i);
                if (!pad) break;
                {
                    std::lock_guard<std::mutex> lock(g_hw_mtx);
                    g_pads[p] = pad;
                    const char* name = SDL_GameControllerName(pad);
                    strncpy(g_hw_names[p], name ? name : "Unknown", sizeof(g_hw_names[p]) - 1);
                }
                break;
            }
        }
    }
}

// ── Read Controller State (SDL2) ──
static void read_pad(int index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    SDL_GameController* pad = g_pads[index];
    if (!pad) { conn = false; return; }

    if (!SDL_GameControllerGetAttached(pad)) {
        std::lock_guard<std::mutex> lock(g_hw_mtx);
        SDL_GameControllerClose(pad);
        g_pads[index] = nullptr;
        g_hw_names[index][0] = '\0';
        conn = false;
        return;
    }

    conn = true;

    // Standardised button mapping (Xbox physical -> Switch)
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) rep.buttons |= ns::BTN_B;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) rep.buttons |= ns::BTN_A;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) rep.buttons |= ns::BTN_Y;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) rep.buttons |= ns::BTN_X;

    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) rep.buttons |= ns::BTN_L;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) rep.buttons |= ns::BTN_R;

    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)   > 16000) rep.buttons |= ns::BTN_ZL;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)  > 16000) rep.buttons |= ns::BTN_ZR;

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

// ── Keyboard polling helpers ──
static bool key_is_down_sdl(const std::string& name) {
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    if (!ks) return false;
    struct { const char* n; SDL_Scancode s; } kmap[] = {
        {"A",SDL_SCANCODE_A},{"B",SDL_SCANCODE_B},{"C",SDL_SCANCODE_C},{"D",SDL_SCANCODE_D},
        {"E",SDL_SCANCODE_E},{"F",SDL_SCANCODE_F},{"G",SDL_SCANCODE_G},{"H",SDL_SCANCODE_H},
        {"I",SDL_SCANCODE_I},{"J",SDL_SCANCODE_J},{"K",SDL_SCANCODE_K},{"L",SDL_SCANCODE_L},
        {"M",SDL_SCANCODE_M},{"N",SDL_SCANCODE_N},{"O",SDL_SCANCODE_O},{"P",SDL_SCANCODE_P},
        {"Q",SDL_SCANCODE_Q},{"R",SDL_SCANCODE_R},{"S",SDL_SCANCODE_S},{"T",SDL_SCANCODE_T},
        {"U",SDL_SCANCODE_U},{"V",SDL_SCANCODE_V},{"W",SDL_SCANCODE_W},{"X",SDL_SCANCODE_X},
        {"Y",SDL_SCANCODE_Y},{"Z",SDL_SCANCODE_Z},
        {"0",SDL_SCANCODE_0},{"1",SDL_SCANCODE_1},{"2",SDL_SCANCODE_2},{"3",SDL_SCANCODE_3},
        {"4",SDL_SCANCODE_4},{"5",SDL_SCANCODE_5},{"6",SDL_SCANCODE_6},{"7",SDL_SCANCODE_7},
        {"8",SDL_SCANCODE_8},{"9",SDL_SCANCODE_9},
        {"UP",SDL_SCANCODE_UP},{"DOWN",SDL_SCANCODE_DOWN},{"LEFT",SDL_SCANCODE_LEFT},{"RIGHT",SDL_SCANCODE_RIGHT},
        {"LSHIFT",SDL_SCANCODE_LSHIFT},{"RSHIFT",SDL_SCANCODE_RSHIFT},
        {"LCTRL",SDL_SCANCODE_LCTRL},{"RCTRL",SDL_SCANCODE_RCTRL},
        {"LALT",SDL_SCANCODE_LALT},{"RALT",SDL_SCANCODE_RALT},
        {"SPACE",SDL_SCANCODE_SPACE},{"ENTER",SDL_SCANCODE_RETURN},{"TAB",SDL_SCANCODE_TAB},
        {"ESC",SDL_SCANCODE_ESCAPE},{"BACKSPACE",SDL_SCANCODE_BACKSPACE},
        {"F1",SDL_SCANCODE_F1},{"F2",SDL_SCANCODE_F2},{"F3",SDL_SCANCODE_F3},{"F4",SDL_SCANCODE_F4},
        {"F5",SDL_SCANCODE_F5},{"F6",SDL_SCANCODE_F6},{"F7",SDL_SCANCODE_F7},{"F8",SDL_SCANCODE_F8},
        {"F9",SDL_SCANCODE_F9},{"F10",SDL_SCANCODE_F10},{"F11",SDL_SCANCODE_F11},{"F12",SDL_SCANCODE_F12},
        {"HOME",SDL_SCANCODE_HOME},{"SNAPSHOT",SDL_SCANCODE_PRINTSCREEN},
    };
    for (auto& km : kmap)
        if (name == km.n) return ks[km.s];
    return false;
}

static void apply_keyboard_to_report(ns::HIDReport& rep, bool override_mode) {
    auto get = [](const std::string& btn) -> std::string {
        auto it = g_keyBindings.find(btn);
        return it != g_keyBindings.end() ? it->second : "";
    };
    std::string k;
    k = get("Y");      if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_Y;
    k = get("B");      if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_B;
    k = get("A");      if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_A;
    k = get("X");      if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_X;
    k = get("L");      if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_L;
    k = get("R");      if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_R;
    k = get("ZL");     if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_ZL;
    k = get("ZR");     if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_ZR;
    k = get("MINUS");  if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_MINUS;
    k = get("PLUS");   if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_PLUS;
    k = get("LSTICK"); if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_LSTICK;
    k = get("RSTICK"); if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_RSTICK;
    k = get("HOME");   if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_HOME;
    k = get("CAPTURE"); if (!k.empty() && key_is_down_sdl(k)) rep.buttons |= ns::BTN_CAPTURE;
    bool up=false,down=false,left=false,right=false;
    k = get("DPAD_UP");    if (!k.empty()) up    = key_is_down_sdl(k);
    k = get("DPAD_DOWN");  if (!k.empty()) down  = key_is_down_sdl(k);
    k = get("DPAD_LEFT");  if (!k.empty()) left  = key_is_down_sdl(k);
    k = get("DPAD_RIGHT"); if (!k.empty()) right = key_is_down_sdl(k);
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
    bool lsu_dn = !lsu.empty() && key_is_down_sdl(lsu);
    bool lsd_dn = !lsd.empty() && key_is_down_sdl(lsd);
    bool lsl_dn = !lsl.empty() && key_is_down_sdl(lsl);
    bool lsr_dn = !lsr.empty() && key_is_down_sdl(lsr);
    if (lsl_dn && !lsr_dn) rep.lx = 0;
    else if (lsr_dn && !lsl_dn) rep.lx = 255;
    else if (!override_mode) rep.lx = 128;
    if (lsu_dn && !lsd_dn) rep.ly = 0;
    else if (lsd_dn && !lsu_dn) rep.ly = 255;
    else if (!override_mode) rep.ly = 128;

    auto rsu = get("RSTICK_UP"), rsd = get("RSTICK_DOWN");
    auto rsl = get("RSTICK_LEFT"), rsr = get("RSTICK_RIGHT");
    bool rsu_dn = !rsu.empty() && key_is_down_sdl(rsu);
    bool rsd_dn = !rsd.empty() && key_is_down_sdl(rsd);
    bool rsl_dn = !rsl.empty() && key_is_down_sdl(rsl);
    bool rsr_dn = !rsr.empty() && key_is_down_sdl(rsr);
    if (rsl_dn && !rsr_dn) rep.rx = 0;
    else if (rsr_dn && !rsl_dn) rep.rx = 255;
    else if (!override_mode) rep.rx = 128;
    if (rsu_dn && !rsd_dn) rep.ry = 0;
    else if (rsd_dn && !rsu_dn) rep.ry = 255;
    else if (!override_mode) rep.ry = 128;
}

// ── Network Sender Thread ──
static void SenderThread(std::string host, uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) {
        close(sock); return;
    }

    struct sockaddr_in dest{};
    memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    uint32_t seq = 0;
    auto next_tick = std::chrono::steady_clock::now();

    while (g_senderRunning.load()) {
        std::this_thread::sleep_until(next_tick);

        ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet)); 
        pkt.magic         = ns::PROTO_MAGIC;
        pkt.version       = ns::PROTO_VERSION;
        pkt.flags         = ns::FLAG_NONE;
        pkt.seq           = seq++;
        pkt.ts_us         = ns::now_us();
        pkt.report.reset(); 

        ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
        int active_count = 0;

        // Copy latest reports polled by the fast timer on the main thread
        {
            std::lock_guard<std::mutex> lock(g_report_mtx);
            for (int i = 0; i < 4; ++i) {
                if (g_shared_connected[i]) {
                    *out_reports[i] = g_shared_reports[i];
                    active_count++;
                }
            }
        }

        {
            uint8_t full_hmac[32];
            hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        }

        sendto(sock, (const char*)&pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
        if (active_count > 0) next_tick += std::chrono::milliseconds(4);
        else next_tick += std::chrono::milliseconds(500);
    }

    close(sock);
}


// ── Bindings editor state ──
static GtkWidget* editorDialog = nullptr;
static std::vector<GtkWidget*> editorKeyLabels;
static std::unordered_map<std::string, std::string> editorBindings;
static int editorListeningIdx = -1;
static bool editorSetupMode = false;
static const std::vector<std::string> editorBindingKeys = {
    "A","B","X","Y","L","R","ZL","ZR",
    "MINUS","PLUS","LSTICK","RSTICK",
    "HOME",
    "LSTICK_UP","LSTICK_DOWN","LSTICK_LEFT","LSTICK_RIGHT",
    "RSTICK_UP","RSTICK_DOWN","RSTICK_LEFT","RSTICK_RIGHT",
    "DPAD_UP","DPAD_DOWN","DPAD_LEFT","DPAD_RIGHT",
    "CAPTURE"
};

extern "C" gboolean on_binding_key_press(GtkWidget*, GdkEventKey* ev, gpointer) {
    if (editorListeningIdx < 0) return FALSE;
    auto vk_to_name = [](guint keyval) -> std::string {
        if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z) return std::string(1, (char)keyval);
        if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) return std::string(1, (char)keyval);
        struct { guint k; const char* n; } map[] = {
            {GDK_KEY_Up,"UP"},{GDK_KEY_Down,"DOWN"},{GDK_KEY_Left,"LEFT"},{GDK_KEY_Right,"RIGHT"},
            {GDK_KEY_Shift_L,"LSHIFT"},{GDK_KEY_Shift_R,"RSHIFT"},
            {GDK_KEY_Control_L,"LCTRL"},{GDK_KEY_Control_R,"RCTRL"},
            {GDK_KEY_Alt_L,"LALT"},{GDK_KEY_Alt_R,"RALT"},
            {GDK_KEY_space,"SPACE"},{GDK_KEY_Return,"ENTER"},{GDK_KEY_Tab,"TAB"},
            {GDK_KEY_Escape,"ESC"},{GDK_KEY_BackSpace,"BACKSPACE"},
            {GDK_KEY_F1,"F1"},{GDK_KEY_F2,"F2"},{GDK_KEY_F3,"F3"},{GDK_KEY_F4,"F4"},
            {GDK_KEY_F5,"F5"},{GDK_KEY_F6,"F6"},{GDK_KEY_F7,"F7"},{GDK_KEY_F8,"F8"},
            {GDK_KEY_F9,"F9"},{GDK_KEY_F10,"F10"},{GDK_KEY_F11,"F11"},{GDK_KEY_F12,"F12"},
            {GDK_KEY_Home,"HOME"},{GDK_KEY_Print,"SNAPSHOT"},
        };
        for (auto& m : map)
            if (keyval == m.k) return m.n;
        return "";
    };
    if (ev->keyval == GDK_KEY_Escape) {
        editorBindings[editorBindingKeys[editorListeningIdx]] = "";
        gtk_label_set_text(GTK_LABEL(editorKeyLabels[editorListeningIdx]), "");
        if (editorSetupMode) {
            editorListeningIdx++;
            if (editorListeningIdx < (int)editorBindingKeys.size()) {
                gtk_label_set_text(GTK_LABEL(editorKeyLabels[editorListeningIdx]), "...");
                return TRUE;
            }
        }
        editorSetupMode = false;
        editorListeningIdx = -1;
        return TRUE;
    }
    std::string name = vk_to_name(ev->keyval);
    if (!name.empty()) {
        // In setup mode, skip already-bound keys
        if (editorSetupMode) {
            bool alreadyBound = false;
            for (auto& [k, v] : editorBindings) {
                if (k != editorBindingKeys[editorListeningIdx] && v == name) { alreadyBound = true; break; }
            }
            if (alreadyBound) return TRUE;
        }
        // Remove this key from any other binding
        for (auto& [k, v] : editorBindings) {
            if (v == name) { v = ""; break; }
        }
        editorBindings[editorBindingKeys[editorListeningIdx]] = name;
        gtk_label_set_text(GTK_LABEL(editorKeyLabels[editorListeningIdx]), name.c_str());
        // Update display for any cleared binding
        for (size_t i = 0; i < editorBindingKeys.size(); i++) {
            if (editorBindings[editorBindingKeys[i]].empty())
                gtk_label_set_text(GTK_LABEL(editorKeyLabels[i]), "");
        }
    }
    if (editorSetupMode) {
        editorListeningIdx++;
        if (editorListeningIdx < (int)editorBindingKeys.size()) {
            gtk_label_set_text(GTK_LABEL(editorKeyLabels[editorListeningIdx]), "...");
            return TRUE;
        }
    }
    editorSetupMode = false;
    editorListeningIdx = -1;
    return TRUE;
}

extern "C" void on_binding_change_clicked(GtkWidget*, gpointer data) {
    int idx = (int)(intptr_t)data;
    editorSetupMode = false;
    editorListeningIdx = idx;
    gtk_label_set_text(GTK_LABEL(editorKeyLabels[idx]), "...");
}

extern "C" void on_binding_reset_clicked(GtkWidget*, gpointer) {
    auto defs = default_key_bindings();
    for (size_t i = 0; i < editorBindingKeys.size(); i++) {
        editorBindings[editorBindingKeys[i]] = defs[editorBindingKeys[i]];
        gtk_label_set_text(GTK_LABEL(editorKeyLabels[i]), editorBindings[editorBindingKeys[i]].c_str());
    }
}

extern "C" void on_binding_setup_clicked(GtkWidget*, gpointer) {
    editorSetupMode = true;
    for (size_t i = 0; i < editorBindingKeys.size(); i++) {
        editorBindings[editorBindingKeys[i]] = "";
        gtk_label_set_text(GTK_LABEL(editorKeyLabels[i]), i == 0 ? "..." : "");
    }
    editorListeningIdx = 0;
}

extern "C" void on_bindings_cancel(GtkWidget*, gpointer) {
    editorSetupMode = false;
    if (editorDialog) gtk_widget_destroy(editorDialog);
    editorDialog = nullptr;
}

extern "C" void on_bindings_save(GtkWidget*, gpointer) {
    g_keyBindings = editorBindings;
    save_bindings();
    if (editorDialog) gtk_widget_destroy(editorDialog);
    editorDialog = nullptr;
}

static void open_bindings_editor() {
    if (editorDialog) return;
    editorBindings = g_keyBindings;
    editorKeyLabels.clear();
    editorSetupMode = false;

    editorDialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(editorDialog), "Edit Key Bindings");
    gtk_window_set_default_size(GTK_WINDOW(editorDialog), 620, 460);
    gtk_window_set_transient_for(GTK_WINDOW(editorDialog), GTK_WINDOW(gtk_widget_get_toplevel(ipEntry)));
    gtk_window_set_modal(GTK_WINDOW(editorDialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(editorDialog), TRUE);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(editorDialog));
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
    gtk_container_add(GTK_CONTAINER(content), grid);

    int half = (int)editorBindingKeys.size() / 2;
    for (int i = 0; i < half; i++) {
        int li = i, ri = i + half;
        // Left: label, key, change at cols 0,1,2
        GtkWidget* ll = gtk_label_new(editorBindingKeys[li].c_str());
        gtk_widget_set_halign(ll, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(grid), ll, 0, i, 1, 1);
        GtkWidget* lk = gtk_label_new(editorBindings[editorBindingKeys[li]].c_str());
        gtk_widget_set_halign(lk, GTK_ALIGN_CENTER);
        gtk_widget_set_size_request(lk, 110, -1);
        gtk_grid_attach(GTK_GRID(grid), lk, 1, i, 1, 1);
        editorKeyLabels.push_back(lk);
        GtkWidget* lc = gtk_button_new_with_label("Change");
        g_signal_connect(lc, "clicked", G_CALLBACK(on_binding_change_clicked), (gpointer)(intptr_t)li);
        gtk_grid_attach(GTK_GRID(grid), lc, 2, i, 1, 1);
        // Right: label, key, change at cols 3,4,5
        GtkWidget* rl = gtk_label_new(editorBindingKeys[ri].c_str());
        gtk_widget_set_halign(rl, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(grid), rl, 3, i, 1, 1);
        GtkWidget* rk = gtk_label_new(editorBindings[editorBindingKeys[ri]].c_str());
        gtk_widget_set_halign(rk, GTK_ALIGN_CENTER);
        gtk_widget_set_size_request(rk, 90, -1);
        gtk_grid_attach(GTK_GRID(grid), rk, 4, i, 1, 1);
        editorKeyLabels.push_back(rk);
        GtkWidget* rc = gtk_button_new_with_label("Change");
        g_signal_connect(rc, "clicked", G_CALLBACK(on_binding_change_clicked), (gpointer)(intptr_t)ri);
        gtk_grid_attach(GTK_GRID(grid), rc, 5, i, 1, 1);
    }

    // Key press handling
    g_signal_connect(editorDialog, "key-press-event", G_CALLBACK(on_binding_key_press), nullptr);

    // Buttons - 2x2: Save/Cancel left, Setup/Reset right
    int btnRow = 13;
    GtkWidget* saveBtn = gtk_button_new_with_label("Save");
    g_signal_connect(saveBtn, "clicked", G_CALLBACK(on_bindings_save), nullptr);
    gtk_grid_attach(GTK_GRID(grid), saveBtn, 0, btnRow, 1, 1);
    gtk_widget_set_halign(saveBtn, GTK_ALIGN_START);
    GtkWidget* setupBtn = gtk_button_new_with_label("Setup");
    g_signal_connect(setupBtn, "clicked", G_CALLBACK(on_binding_setup_clicked), nullptr);
    gtk_grid_attach(GTK_GRID(grid), setupBtn, 5, btnRow, 1, 1);
    gtk_widget_set_halign(setupBtn, GTK_ALIGN_END);
    GtkWidget* cancelBtn = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancelBtn, "clicked", G_CALLBACK(on_bindings_cancel), nullptr);
    gtk_grid_attach(GTK_GRID(grid), cancelBtn, 0, btnRow + 1, 1, 1);
    gtk_widget_set_halign(cancelBtn, GTK_ALIGN_START);
    GtkWidget* resetBtn = gtk_button_new_with_label("Reset");
    g_signal_connect(resetBtn, "clicked", G_CALLBACK(on_binding_reset_clicked), nullptr);
    gtk_grid_attach(GTK_GRID(grid), resetBtn, 5, btnRow + 1, 1, 1);
    gtk_widget_set_halign(resetBtn, GTK_ALIGN_END);

    gtk_widget_show_all(editorDialog);
}

// ── GTK Callbacks ──
extern "C" void on_keyboard_combo_changed(GtkComboBox* combo, gpointer) {
    int sel = gtk_combo_box_get_active(combo);
    if (sel < 0) sel = 0;
    g_keyboardMode = sel;
    gtk_widget_set_sensitive(bindingsBtn, sel != KB_OFF);
    save_config(gtk_entry_get_text(GTK_ENTRY(ipEntry)), sel);
}

extern "C" void on_bindings_clicked(GtkWidget*, gpointer) {
    open_bindings_editor();
}

extern "C" void on_connect_clicked(GtkWidget*, gpointer) {
    if (g_connected) {
        g_connected = false;
        g_senderRunning = false;
        if (g_senderThread.joinable()) g_senderThread.join();
        
        // Clean up gamepads on the main thread
        for (int i = 0; i < 4; ++i) {
            if (g_pads[i]) { SDL_GameControllerClose(g_pads[i]); g_pads[i] = nullptr; g_hw_names[i][0] = '\0'; }
        }
        for (int i = 0; i < 4; ++i) g_shared_connected[i] = false;

        gtk_button_set_label(GTK_BUTTON(connectBtn), "Connect");
        gtk_widget_set_sensitive(ipEntry, TRUE);
        gtk_widget_set_sensitive(kbCombo, TRUE);
        if (g_keyboardMode.load() != KB_OFF) gtk_widget_set_sensitive(bindingsBtn, TRUE);
        gtk_label_set_text(GTK_LABEL(statusLabel), "Disconnected");
        
        for (int i = 0; i < 4; ++i) {
            char buf[64]; snprintf(buf, sizeof(buf), "P%d: Waiting...", i + 1);
            gtk_label_set_text(GTK_LABEL(ctrlLabels[i]), buf);
        }
        return;
    }

    const char* ipStr = gtk_entry_get_text(GTK_ENTRY(ipEntry));
    if (strlen(ipStr) == 0) return;

    char ipBuf[64]; strncpy(ipBuf, ipStr, sizeof(ipBuf) - 1); ipBuf[sizeof(ipBuf) - 1] = '\0';
    int port = ns::DEFAULT_PORT;
    char* colon = strchr(ipBuf, ':');
    if (colon) { *colon = '\0'; port = atoi(colon + 1); if (port <= 0 || port > 65535) port = ns::DEFAULT_PORT; }

    save_config(ipStr, g_keyboardMode.load());
    derive_key(ns::DEFAULT_SECRET, g_hmacKey);
    g_connected = true;

    for (int i=0; i<4; ++i) { g_hw_names[i][0] = '\0'; g_pads[i] = nullptr; }

    g_senderRunning = true;
    g_senderThread = std::thread(SenderThread, std::string(ipBuf), (uint16_t)port);

    gtk_button_set_label(GTK_BUTTON(connectBtn), "Disconnect");
    gtk_widget_set_sensitive(ipEntry, FALSE);
    gtk_widget_set_sensitive(kbCombo, FALSE);
    gtk_widget_set_sensitive(bindingsBtn, FALSE);

    char status[128]; snprintf(status, sizeof(status), "Connected to %s:%d", ipBuf, port);
    gtk_label_set_text(GTK_LABEL(statusLabel), status);
}

extern "C" gboolean on_timer(gpointer) {
    int km = g_keyboardMode.load();
    if (g_connected) {
        std::lock_guard<std::mutex> lock(g_hw_mtx);
        for (int i = 0; i < 4; ++i) {
            char lbl[128];
            if (i == 0 && km != KB_OFF) {
                if (km == KB_SINGLE) {
                    snprintf(lbl, sizeof(lbl), "P1: Keyboard");
                } else {
                    bool conn = g_hw_names[0][0] != '\0';
                    snprintf(lbl, sizeof(lbl), "P1: %s / Keyboard", conn ? (std::string("Connected - ") + g_hw_names[0]).c_str() : "Idle");
                }
            } else if (g_hw_names[i][0] != '\0') {
                snprintf(lbl, sizeof(lbl), "P%d: %s", i + 1, g_hw_names[i]);
            } else {
                snprintf(lbl, sizeof(lbl), "P%d: Waiting...", i + 1);
            }
            gtk_label_set_text(GTK_LABEL(ctrlLabels[i]), lbl);
        }
    } else {
        std::lock_guard<std::mutex> lock(g_hw_mtx);
        for (int i = 0; i < 4; ++i) {
            char lbl[128];
            if (i == 0 && km != KB_OFF) {
                if (km == KB_SINGLE) {
                    snprintf(lbl, sizeof(lbl), "P1: Keyboard");
                } else {
                    bool conn = g_hw_names[0][0] != '\0';
                    snprintf(lbl, sizeof(lbl), "P1: %s / Keyboard", conn ? (std::string("Connected - ") + g_hw_names[0]).c_str() : "Idle");
                }
            } else if (g_hw_names[i][0] != '\0') {
                snprintf(lbl, sizeof(lbl), "P%d: %s", i + 1, g_hw_names[i]);
            } else {
                snprintf(lbl, sizeof(lbl), "P%d: Waiting...", i + 1);
            }
            gtk_label_set_text(GTK_LABEL(ctrlLabels[i]), lbl);
        }
    }
    return G_SOURCE_CONTINUE;
}

// ── Fast Timer: SDL Polling on Main Thread (250Hz) ──
extern "C" gboolean on_fast_timer(gpointer) {
    scan_for_gamepads();

    if (!g_connected) return G_SOURCE_CONTINUE;

    ns::HIDReport reports[4];
    bool connected[4] = {};
    int active_count = 0;

    for (int i = 0; i < 4; ++i) {
        bool conn = false;
        read_pad(i, reports[i], conn);
        connected[i] = conn;
        if (conn) active_count++;
    }

    int km = g_keyboardMode.load();
    if (km == KB_SINGLE) {
        bool c1 = connected[0], c2 = connected[1], c3 = connected[2], c4 = connected[3];
        if (c1) {
            if (!c2) {
                reports[1] = reports[0]; connected[1] = true; active_count++;
                std::lock_guard<std::mutex> lock(g_hw_mtx);
                strncpy(g_hw_names[1], g_hw_names[0], sizeof(g_hw_names[1]) - 1);
            } else if (!c3) {
                reports[2] = reports[0]; connected[2] = true; active_count++;
                std::lock_guard<std::mutex> lock(g_hw_mtx);
                strncpy(g_hw_names[2], g_hw_names[0], sizeof(g_hw_names[2]) - 1);
            } else if (!c4) {
                reports[3] = reports[0]; connected[3] = true; active_count++;
                std::lock_guard<std::mutex> lock(g_hw_mtx);
                strncpy(g_hw_names[3], g_hw_names[0], sizeof(g_hw_names[3]) - 1);
            }
        }
        reports[0].reset();
        apply_keyboard_to_report(reports[0], false);
        active_count = std::max(active_count, 1);
    } else if (km == KB_OVERRIDE) {
        apply_keyboard_to_report(reports[0], true);
        active_count = std::max(active_count, 1);
    }

    {
        std::lock_guard<std::mutex> lock(g_report_mtx);
        for (int i = 0; i < 4; ++i) {
            g_shared_reports[i] = reports[i];
            g_shared_connected[i] = connected[i];
        }
    }

    return G_SOURCE_CONTINUE;
}

static std::string get_exe_dir() {
    char buf[1024]; ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0'; std::string exe(buf);
        size_t pos = exe.find_last_of("/");
        if (pos != std::string::npos) return exe.substr(0, pos);
    }
    return ".";
}

extern "C" void on_window_destroy(GtkWidget*, gpointer) {
    if (g_connected) { g_senderRunning = false; if (g_senderThread.joinable()) g_senderThread.join(); }
    gtk_main_quit();
}

// ── Entry point ──
int main(int argc, char* argv[]) {
    // Elevate priority
    setpriority(PRIO_PROCESS, 0, -20);

    // Initialise SDL2 GameController subsystem
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_VIDEO) < 0) {
        std::cerr << "Failed to initialise SDL2: " << SDL_GetError() << "\n";
        return 1;
    }

    gtk_init(&argc, &argv);

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "NS PC Control");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 310);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), nullptr);
    gtk_window_set_icon_from_file(GTK_WINDOW(window), (get_exe_dir() + "/icon.png").c_str(), nullptr);

    GtkWidget* grid = gtk_grid_new();
    gtk_container_add(GTK_CONTAINER(window), grid);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 16);

    // Row 0: IP
    GtkWidget* ipLabel = gtk_label_new("Raspberry Pi IP:");
    gtk_widget_set_halign(ipLabel, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), ipLabel, 0, 0, 1, 1);

    ipEntry = gtk_entry_new();
    {
        int savedMode = 0;
        std::string saved = load_saved_config(&savedMode);
        g_keyboardMode = savedMode;
        gtk_entry_set_text(GTK_ENTRY(ipEntry), saved.empty() ? "192.168.1.100" : saved.c_str());
    }
    gtk_grid_attach(GTK_GRID(grid), ipEntry, 1, 0, 3, 1);

    // Row 1: Keyboard Mode
    GtkWidget* kbLabel = gtk_label_new("Keyboard Mode:");
    gtk_widget_set_halign(kbLabel, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), kbLabel, 0, 1, 1, 1);

    kbCombo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kbCombo), nullptr, "OFF");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kbCombo), nullptr, "ON (single)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(kbCombo), nullptr, "ON (override)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(kbCombo), g_keyboardMode.load());
    g_signal_connect(kbCombo, "changed", G_CALLBACK(on_keyboard_combo_changed), nullptr);
    gtk_grid_attach(GTK_GRID(grid), kbCombo, 1, 1, 2, 1);

    bindingsBtn = gtk_button_new_with_label("Bindings...");
    gtk_widget_set_sensitive(bindingsBtn, FALSE);
    g_signal_connect(bindingsBtn, "clicked", G_CALLBACK(on_bindings_clicked), nullptr);
    gtk_grid_attach(GTK_GRID(grid), bindingsBtn, 3, 1, 1, 1);

    // Row 2: Connect Button
    connectBtn = gtk_button_new_with_label("Connect");
    g_signal_connect(connectBtn, "clicked", G_CALLBACK(on_connect_clicked), nullptr);
    gtk_grid_attach(GTK_GRID(grid), connectBtn, 1, 2, 3, 1);

    // Row 3: Separator
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep, 0, 3, 4, 1);

    // Row 4: Status
    statusLabel = gtk_label_new("Ready");
    gtk_widget_set_halign(statusLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), statusLabel, 0, 4, 4, 1);

    // Rows 5-8: P1 to P4 Slots
    for (int i = 0; i < 4; ++i) {
        char buf[64]; snprintf(buf, sizeof(buf), "P%d: Waiting...", i + 1);
        ctrlLabels[i] = gtk_label_new(buf);
        
        // Add some margin for visual indentation
        gtk_widget_set_margin_start(ctrlLabels[i], 10);
        gtk_widget_set_halign(ctrlLabels[i], GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), ctrlLabels[i], 0, 5 + i, 4, 1);
    }

    // Fast timer for SDL gamepad polling (250Hz, main thread only)
    g_timeout_add(4, on_fast_timer, nullptr);
    // Timer for UI updates (100ms)
    g_timeout_add(100, on_timer, nullptr);

    load_bindings();

    gtk_widget_show_all(window);
    gtk_main();

    SDL_Quit();
    return 0;
}