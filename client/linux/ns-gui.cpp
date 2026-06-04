/// ns-gui.cpp  —  GTK3 GUI frontend for the Switch wireless gamepad bridge
///
/// Build:
///   g++ -O3 -std=c++17 ns-gui.cpp -o ns-gui \
///       $(pkg-config --cflags --libs gtk+-3.0) -lpthread -lSDL2
///
/// Usage:
///   ./ns-gui

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
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

// ── Background Keyboard Hardware Polling (/dev/input/) ──
#define BITS_PER_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define test_bit(bit, array) ((array[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

static std::vector<int> g_kb_fds;
static bool g_kb_permission_warning_shown = false;

void init_global_keyboard() {
    for (int fd : g_kb_fds) close(fd);
    g_kb_fds.clear();

    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) == 0) {
            char path[256]; snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                unsigned long evbit[NLONGS(EV_MAX)] = {0};
                if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) >= 0) {
                    if (test_bit(EV_KEY, evbit)) {
                        unsigned long keybit[NLONGS(KEY_MAX)] = {0};
                        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
                        if (test_bit(KEY_A, keybit)) g_kb_fds.push_back(fd);
                        else close(fd);
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
    
    // FIX: Exact mapping to Linux EVDEV hardware scan codes
    static const std::unordered_map<std::string, int> key_map = {
        {"A", KEY_A}, {"B", KEY_B}, {"C", KEY_C}, {"D", KEY_D}, {"E", KEY_E},
        {"F", KEY_F}, {"G", KEY_G}, {"H", KEY_H}, {"I", KEY_I}, {"J", KEY_J},
        {"K", KEY_K}, {"L", KEY_L}, {"M", KEY_M}, {"N", KEY_N}, {"O", KEY_O},
        {"P", KEY_P}, {"Q", KEY_Q}, {"R", KEY_R}, {"S", KEY_S}, {"T", KEY_T},
        {"U", KEY_U}, {"V", KEY_V}, {"W", KEY_W}, {"X", KEY_X}, {"Y", KEY_Y},
        {"Z", KEY_Z},
        {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4}, {"5", KEY_5},
        {"6", KEY_6}, {"7", KEY_7}, {"8", KEY_8}, {"9", KEY_9}, {"0", KEY_0},
        {"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT},
        {"SHIFT_L", KEY_LEFTSHIFT}, {"LSHIFT", KEY_LEFTSHIFT}, {"LEFT SHIFT", KEY_LEFTSHIFT},
        {"SHIFT_R", KEY_RIGHTSHIFT}, {"RSHIFT", KEY_RIGHTSHIFT}, {"RIGHT SHIFT", KEY_RIGHTSHIFT},
        {"CONTROL_L", KEY_LEFTCTRL}, {"LCTRL", KEY_LEFTCTRL},
        {"CONTROL_R", KEY_RIGHTCTRL}, {"RCTRL", KEY_RIGHTCTRL},
        {"ALT_L", KEY_LEFTALT}, {"LALT", KEY_LEFTALT},
        {"ALT_R", KEY_RIGHTALT}, {"RALT", KEY_RIGHTALT},
        {"SPACE", KEY_SPACE}, {"RETURN", KEY_ENTER}, {"ENTER", KEY_ENTER},
        {"TAB", KEY_TAB}, {"ESCAPE", KEY_ESC}, {"ESC", KEY_ESC},
        {"BACKSPACE", KEY_BACKSPACE}, {"HOME", KEY_HOME},
        {"PRINT", KEY_SYSRQ}, {"SNAPSHOT", KEY_SYSRQ}, {"PRINTSCREEN", KEY_SYSRQ}
    };

    auto it = key_map.find(name);
    return (it != key_map.end()) ? it->second : KEY_RESERVED;
}

// ── Logic State ──
enum { KB_OFF = 0, KB_SINGLE = 1, KB_OVERRIDE = 2 };
static std::atomic<int> g_keyboardMode{KB_OFF};
static std::unordered_map<std::string, std::string> g_keyBindings;

static std::unordered_map<std::string, std::string> default_key_bindings() {
    return {
        {"Y","Z"}, {"B","X"}, {"A","V"}, {"X","C"},
        {"L","Q"}, {"R","E"}, {"ZL","1"}, {"ZR","2"},
        {"MINUS","3"}, {"PLUS","4"},
        {"LSTICK","Shift_L"}, {"RSTICK","Shift_R"},
        {"HOME","Home"}, {"CAPTURE","Print"},
        {"LSTICK_UP","W"}, {"LSTICK_DOWN","S"},
        {"LSTICK_LEFT","A"}, {"LSTICK_RIGHT","D"},
        {"RSTICK_UP","I"}, {"RSTICK_DOWN","K"},
        {"RSTICK_LEFT","J"}, {"RSTICK_RIGHT","L"},
        {"DPAD_UP","Up"}, {"DPAD_DOWN","Down"},
        {"DPAD_LEFT","Left"}, {"DPAD_RIGHT","Right"}
    };
}

static std::string get_config_dir() {
    const char* home = getenv("HOME");
    if (!home) return ".";
    return std::string(home) + "/.config/ns-pc-control";
}

static void load_saved_config(std::string& ip, int& mode) {
    ip = "192.168.1.100"; mode = 0; g_keyBindings = default_key_bindings();
    std::string path = get_config_dir() + "/config";
    FILE* f = fopen(path.c_str(), "r"); if (!f) return;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "IP") ip = v;
            else if (k == "KB_MODE") mode = std::atoi(v.c_str());
            else if (k.find("BIND_") == 0) g_keyBindings[k.substr(5)] = v;
        }
    }
    fclose(f);
}

static void save_config(const std::string& ip, int mode) {
    std::string dir = get_config_dir();
    if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) return;
    std::string path = dir + "/config";
    FILE* f = fopen(path.c_str(), "w");
    if (f) { 
        fprintf(f, "IP=%s\nKB_MODE=%d\n", ip.c_str(), mode);
        for (auto& [k, v] : g_keyBindings) fprintf(f, "BIND_%s=%s\n", k.c_str(), v.c_str());
        fclose(f); 
    }
}

static GtkWidget* ipEntry = nullptr;
static GtkWidget* connectBtn = nullptr;
static GtkWidget* statusLabel = nullptr;
static GtkWidget* kbCombo = nullptr;
static GtkWidget* bindingsBtn = nullptr;
static GtkWidget* ctrlLabels[4]; 

static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_senderRunning{false};
static std::thread g_senderThread;
static uint8_t g_hmacKey[32]{};

static SDL_GameController* g_pads[4] = {nullptr, nullptr, nullptr, nullptr};
static char         g_hw_names[4][128];
static std::mutex   g_hw_mtx;

static uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled = (val >= deadzone) ? (128 + ((val - deadzone) * 127) / (32767 - deadzone))
                                   : (128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone));
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

static void scan_for_gamepads() {
    static uint64_t last_scan = 0;
    uint64_t now = ns::now_us();
    SDL_GameControllerUpdate();
    if (now - last_scan < 1'000'000) return;
    last_scan = now;

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
                
                // Keep the lock scoped ONLY to the variable assignment!
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

static void read_pad(int index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    SDL_GameController* pad = g_pads[index];
    if (!pad) { conn = false; return; }

    if (!SDL_GameControllerGetAttached(pad)) {
        std::lock_guard<std::mutex> lock(g_hw_mtx);
        SDL_GameControllerClose(pad);
        g_pads[index] = nullptr; g_hw_names[index][0] = '\0';
        conn = false; return;
    }

    conn = true;
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

static void apply_keyboard_to_report(ns::HIDReport& rep, bool override_mode) {
    auto is_down = [&](const std::string& btn) -> bool {
        auto it = g_keyBindings.find(btn);
        if (it == g_keyBindings.end() || it->second.empty()) return false;
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
    if (lsl && !lsr) rep.lx = 0; else if (lsr && !lsl) rep.lx = 255; else if (!override_mode) rep.lx = 128;
    if (lsu && !lsd) rep.ly = 0; else if (lsd && !lsu) rep.ly = 255; else if (!override_mode) rep.ly = 128;

    bool rsu = is_down("RSTICK_UP"), rsd = is_down("RSTICK_DOWN");
    bool rsl = is_down("RSTICK_LEFT"), rsr = is_down("RSTICK_RIGHT");
    if (rsl && !rsr) rep.rx = 0; else if (rsr && !rsl) rep.rx = 255; else if (!override_mode) rep.rx = 128;
    if (rsu && !rsd) rep.ry = 0; else if (rsd && !rsu) rep.ry = 255; else if (!override_mode) rep.ry = 128;
}

static void SenderThread(std::string host, uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0); if (sock < 0) return;
    struct addrinfo hints{}, *res; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0) { close(sock); return; }

    struct sockaddr_in dest{}; memcpy(&dest, res->ai_addr, sizeof(dest)); freeaddrinfo(res);
    uint32_t seq = 0; auto next_tick = std::chrono::steady_clock::now();

    while (g_senderRunning.load()) {
        while (std::chrono::steady_clock::now() < next_tick) std::atomic_thread_fence(std::memory_order_relaxed);
        scan_for_gamepads();

        ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet)); 
        pkt.magic = ns::PROTO_MAGIC; pkt.version = ns::PROTO_VERSION;
        pkt.flags = ns::FLAG_NONE; pkt.seq = seq++; pkt.ts_us = ns::now_us();
        pkt.report.reset(); 

        ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
        int active_count = 0; bool c1=false, c2=false, c3=false, c4=false;
        
        for (int i = 0; i < 4; ++i) {
            bool is_conn = false; read_pad(i, *out_reports[i], is_conn);
            if (is_conn) { active_count++; if (i==0) c1=true; else if (i==1) c2=true; else if (i==2) c3=true; else if (i==3) c4=true; }
        }

        int km = g_keyboardMode.load();
        if (km == KB_SINGLE) {
            if (c1) {
                if (!c2) { pkt.report.p2 = pkt.report.p1; c2 = true; }
                else if (!c3) { pkt.report.p3 = pkt.report.p1; c3 = true; }
                else if (!c4) { pkt.report.p4 = pkt.report.p1; c4 = true; }
            }
            pkt.report.p1.reset(); apply_keyboard_to_report(pkt.report.p1, false);
            active_count = std::max(active_count, 1);
        } else if (km == KB_OVERRIDE) {
            apply_keyboard_to_report(pkt.report.p1, true);
            active_count = std::max(active_count, 1);
        }

        if (active_count == 0) pkt.report.reset();

        uint8_t full_hmac[32]; hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
        memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        sendto(sock, (const char*)&pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
        if (active_count > 0) next_tick += std::chrono::milliseconds(4); else next_tick += std::chrono::milliseconds(500);
    }
    
    // Cleanup on thread exit
    std::lock_guard<std::mutex> lock(g_hw_mtx);
    for (int i = 0; i < 4; ++i) { if (g_pads[i]) { SDL_GameControllerClose(g_pads[i]); g_pads[i] = nullptr; g_hw_names[i][0] = '\0'; } }
    close(sock);
}

// ── GTK Callbacks ──
extern "C" void on_connect_clicked(GtkWidget*, gpointer) {
    if (g_connected) {
        g_connected = false; g_senderRunning = false;
        if (g_senderThread.joinable()) g_senderThread.join();
        gtk_button_set_label(GTK_BUTTON(connectBtn), "Connect");
        gtk_widget_set_sensitive(ipEntry, TRUE); gtk_widget_set_sensitive(kbCombo, TRUE);
        if (g_keyboardMode != KB_OFF) gtk_widget_set_sensitive(bindingsBtn, TRUE);
        gtk_label_set_text(GTK_LABEL(statusLabel), "Disconnected");
        for (int i = 0; i < 4; ++i) gtk_label_set_text(GTK_LABEL(ctrlLabels[i]), "Waiting...");
        return;
    }

    const char* ipStr = gtk_entry_get_text(GTK_ENTRY(ipEntry)); if (strlen(ipStr) == 0) return;
    char ipBuf[64]; strncpy(ipBuf, ipStr, sizeof(ipBuf) - 1); ipBuf[sizeof(ipBuf) - 1] = '\0';
    int port = ns::DEFAULT_PORT; char* colon = strchr(ipBuf, ':');
    if (colon) { *colon = '\0'; port = atoi(colon + 1); if (port <= 0 || port > 65535) port = ns::DEFAULT_PORT; }

    save_config(ipStr, g_keyboardMode.load());
    derive_key(ns::DEFAULT_SECRET, g_hmacKey);
    g_connected = true;

    {
        std::lock_guard<std::mutex> lock(g_hw_mtx);
        for (int i=0; i<4; ++i) { g_hw_names[i][0] = '\0'; g_pads[i] = nullptr; }
    }
    
    // Check permission context immediately upon connection
    init_global_keyboard();
    if (g_keyboardMode.load() != KB_OFF && g_kb_fds.empty() && !g_kb_permission_warning_shown) {
        GtkWidget *dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Cannot access background keyboard!\n\nTo use background input on Linux, run this in your terminal and log out/in:\n\nsudo usermod -aG input $USER");
        gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
        g_kb_permission_warning_shown = true;
    }

    g_senderRunning = true; g_senderThread = std::thread(SenderThread, std::string(ipBuf), (uint16_t)port);
    gtk_button_set_label(GTK_BUTTON(connectBtn), "Disconnect");
    gtk_widget_set_sensitive(ipEntry, FALSE); gtk_widget_set_sensitive(kbCombo, FALSE); gtk_widget_set_sensitive(bindingsBtn, FALSE);
    char status[128]; snprintf(status, sizeof(status), "Connected to %s:%d", ipBuf, port);
    gtk_label_set_text(GTK_LABEL(statusLabel), status);
}

extern "C" void on_kb_combo_changed(GtkComboBox* widget, gpointer) {
    g_keyboardMode = gtk_combo_box_get_active(widget);
    gtk_widget_set_sensitive(bindingsBtn, g_keyboardMode != KB_OFF);
    save_config(gtk_entry_get_text(GTK_ENTRY(ipEntry)), g_keyboardMode.load());
}

// ── Bindings Editor Dialog ──
static std::string g_listeningKey;
static GtkWidget* g_listeningBtn = nullptr;
static std::unordered_map<std::string, std::string> g_editBindings;

extern "C" gboolean on_dialog_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    if (!g_listeningBtn || g_listeningKey.empty()) return FALSE;
    if (event->keyval == GDK_KEY_Escape) {
        g_editBindings[g_listeningKey] = ""; gtk_button_set_label(GTK_BUTTON(g_listeningBtn), "");
    } else {
        const char* name = gdk_keyval_name(event->keyval);
        if (name) {
            for (auto& [k, v] : g_editBindings) { if (v == name) v = ""; } 
            g_editBindings[g_listeningKey] = name;
            gtk_button_set_label(GTK_BUTTON(g_listeningBtn), name);
        }
    }
    g_listeningBtn = nullptr; g_listeningKey = "";
    return TRUE; 
}

extern "C" void on_bind_btn_clicked(GtkWidget* btn, gpointer data) {
    g_listeningBtn = btn; g_listeningKey = (const char*)data;
    gtk_button_set_label(GTK_BUTTON(btn), "Press Key...");
}

extern "C" void on_bindings_clicked(GtkWidget*, gpointer parent_window) {
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Edit Bindings", GTK_WINDOW(parent_window), GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 400);
    g_signal_connect(dialog, "key-press-event", G_CALLBACK(on_dialog_key_press), nullptr);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 4); gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10); gtk_container_add(GTK_CONTAINER(content), grid);

    g_editBindings = g_keyBindings;
    std::vector<std::string> order = { "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "MINUS", "PLUS", "LSTICK", "RSTICK", "HOME", "CAPTURE", "LSTICK_UP", "LSTICK_DOWN", "LSTICK_LEFT", "LSTICK_RIGHT", "RSTICK_UP", "RSTICK_DOWN", "RSTICK_LEFT", "RSTICK_RIGHT", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT" };
    int row = 0, col = 0;
    for (size_t i = 0; i < order.size(); i++) {
        GtkWidget* lbl = gtk_label_new(order[i].c_str()); gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, col, row, 1, 1);
        GtkWidget* btn = gtk_button_new_with_label(g_editBindings[order[i]].c_str()); gtk_widget_set_size_request(btn, 100, -1);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_bind_btn_clicked), (gpointer)order[i].c_str());
        gtk_grid_attach(GTK_GRID(grid), btn, col + 1, row, 1, 1);
        row++; if (row >= 13) { row = 0; col += 2; }
    }
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        g_keyBindings = g_editBindings;
        save_config(gtk_entry_get_text(GTK_ENTRY(ipEntry)), g_keyboardMode.load());
    }
    g_listeningBtn = nullptr; gtk_widget_destroy(dialog);
}

extern "C" gboolean on_timer(gpointer) {
    // 1. Move the scan OUTSIDE the mutex lock to prevent deadlock
    if (!g_connected) scan_for_gamepads();
    
    // 2. Lock UI specifically for reading the mapped arrays safely
    std::lock_guard<std::mutex> lock(g_hw_mtx);
    int km = g_keyboardMode.load();
    
    for (int i = 0; i < 4; ++i) {
        char lbl[128];
        if (km == KB_SINGLE) {
            if (i == 0) {
                snprintf(lbl, sizeof(lbl), "🎮 P1: Keyboard");
            } else {
                int phys_i = i - 1; 
                if (g_hw_names[phys_i][0] != '\0') snprintf(lbl, sizeof(lbl), "🎮 P%d: %s", i + 1, g_hw_names[phys_i]);
                else snprintf(lbl, sizeof(lbl), "P%d: Waiting...", i + 1);
            }
        } else if (km == KB_OVERRIDE) {
            if (i == 0) {
                if (g_hw_names[0][0] != '\0') snprintf(lbl, sizeof(lbl), "🎮 P1: %s / Keyboard", g_hw_names[0]);
                else snprintf(lbl, sizeof(lbl), "🎮 P1: Idle / Keyboard");
            } else {
                if (g_hw_names[i][0] != '\0') snprintf(lbl, sizeof(lbl), "🎮 P%d: %s", i + 1, g_hw_names[i]);
                else snprintf(lbl, sizeof(lbl), "P%d: Waiting...", i + 1);
            }
        } else {
            if (g_hw_names[i][0] != '\0') snprintf(lbl, sizeof(lbl), "🎮 P%d: %s", i + 1, g_hw_names[i]);
            else snprintf(lbl, sizeof(lbl), "P%d: Waiting...", i + 1);
        }
        gtk_label_set_text(GTK_LABEL(ctrlLabels[i]), lbl);
    }
    return G_SOURCE_CONTINUE;
}

static std::string get_exe_dir() {
    char buf[1024]; ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) { buf[len] = '\0'; std::string exe(buf); size_t pos = exe.find_last_of("/"); if (pos != std::string::npos) return exe.substr(0, pos); }
    return ".";
}

extern "C" void on_window_destroy(GtkWidget*, gpointer) {
    if (g_connected) { g_senderRunning = false; if (g_senderThread.joinable()) g_senderThread.join(); }
    for (int fd : g_kb_fds) close(fd);
    gtk_main_quit();
}

int main(int argc, char* argv[]) {
    setpriority(PRIO_PROCESS, 0, -20);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) return 1;

    gtk_init(&argc, &argv);
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "NS PC Control"); gtk_window_set_default_size(GTK_WINDOW(window), 420, 320); gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), nullptr);
    gtk_window_set_icon_from_file(GTK_WINDOW(window), (get_exe_dir() + "/icon.png").c_str(), nullptr);

    GtkWidget* grid = gtk_grid_new(); gtk_container_add(GTK_CONTAINER(window), grid);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8); gtk_grid_set_column_spacing(GTK_GRID(grid), 10); gtk_container_set_border_width(GTK_CONTAINER(grid), 16);

    std::string savedIp; int savedMode; load_saved_config(savedIp, savedMode); g_keyboardMode = savedMode;

    GtkWidget* ipLabel = gtk_label_new("Raspberry Pi IP:"); gtk_widget_set_halign(ipLabel, GTK_ALIGN_END); gtk_grid_attach(GTK_GRID(grid), ipLabel, 0, 0, 1, 1);
    ipEntry = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(ipEntry), savedIp.c_str()); gtk_grid_attach(GTK_GRID(grid), ipEntry, 1, 0, 2, 1);

    GtkWidget* kbLabel = gtk_label_new("Keyboard Mode:"); gtk_widget_set_halign(kbLabel, GTK_ALIGN_END); gtk_grid_attach(GTK_GRID(grid), kbLabel, 0, 1, 1, 1);
    kbCombo = gtk_combo_box_text_new(); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(kbCombo), "OFF"); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(kbCombo), "ON (single)"); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(kbCombo), "ON (override)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(kbCombo), savedMode); g_signal_connect(kbCombo, "changed", G_CALLBACK(on_kb_combo_changed), nullptr); gtk_grid_attach(GTK_GRID(grid), kbCombo, 1, 1, 1, 1);

    bindingsBtn = gtk_button_new_with_label("Bindings..."); gtk_widget_set_sensitive(bindingsBtn, savedMode != KB_OFF); g_signal_connect(bindingsBtn, "clicked", G_CALLBACK(on_bindings_clicked), window); gtk_grid_attach(GTK_GRID(grid), bindingsBtn, 2, 1, 1, 1);

    connectBtn = gtk_button_new_with_label("Connect"); g_signal_connect(connectBtn, "clicked", G_CALLBACK(on_connect_clicked), nullptr); gtk_grid_attach(GTK_GRID(grid), connectBtn, 1, 2, 2, 1);
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL); gtk_grid_attach(GTK_GRID(grid), sep, 0, 3, 3, 1);
    statusLabel = gtk_label_new("Ready"); gtk_widget_set_halign(statusLabel, GTK_ALIGN_START); gtk_grid_attach(GTK_GRID(grid), statusLabel, 0, 4, 3, 1);

    for (int i = 0; i < 4; ++i) {
        char buf[64]; snprintf(buf, sizeof(buf), "P%d: Waiting...", i + 1);
        ctrlLabels[i] = gtk_label_new(buf); gtk_widget_set_margin_start(ctrlLabels[i], 10); gtk_widget_set_halign(ctrlLabels[i], GTK_ALIGN_START); gtk_grid_attach(GTK_GRID(grid), ctrlLabels[i], 0, 5 + i, 3, 1);
    }

    g_timeout_add(100, on_timer, nullptr);
    gtk_widget_show_all(window);
    gtk_main();

    SDL_Quit(); return 0;
}