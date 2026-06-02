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
#include <cmath>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/joystick.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
// linux/joystick.h defines macro BTN_A/BTN_B/BTN_X/BTN_Y which collide with our enum
#undef BTN_A
#undef BTN_B
#undef BTN_X
#undef BTN_Y
#include "sha256.h"

// ── Protocol ──
namespace ns {
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;
static constexpr uint8_t  PROTO_VERSION = 1;
static constexpr uint16_t DEFAULT_PORT  = 7331;
static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
static constexpr size_t HMAC_TAG_SIZE = 16;

enum Button : uint16_t {
    BTN_Y=1<<0, BTN_B=1<<1, BTN_A=1<<2, BTN_X=1<<3,
    BTN_L=1<<4, BTN_R=1<<5, BTN_ZL=1<<6, BTN_ZR=1<<7,
    BTN_MINUS=1<<8, BTN_PLUS=1<<9, BTN_LSTICK=1<<10, BTN_RSTICK=1<<11,
    BTN_HOME=1<<12, BTN_CAPTURE=1<<13,
};
enum Hat : uint8_t { HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3, HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8 };
#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0; uint8_t hat = HAT_NEUTRAL;
    uint8_t lx=128, ly=128, rx=128, ry=128, vendor=0;
    void reset() noexcept { *this = HIDReport{}; }
};
enum Flags : uint8_t { FLAG_NONE=0x00, FLAG_RESET=0x01, FLAG_AUTOFIRE=0x02 };
struct Packet {
    uint32_t magic; uint8_t version; uint8_t flags; uint16_t autofire_mask;
    uint32_t seq; uint64_t ts_us; HIDReport report; uint8_t hmac[HMAC_TAG_SIZE];
};
#pragma pack(pop)
static constexpr size_t PACKET_SIZE = sizeof(Packet);
static constexpr size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
}

// ── Config path helpers ──
static std::string get_config_dir() {
    const char* home = getenv("HOME");
    if (!home) return ".";
    return std::string(home) + "/.config/ns-pc-control";
}

static std::string load_saved_config() {
    std::string path = get_config_dir() + "/config";
    char buf[256]{};
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    if (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
    }
    fclose(f);
    return buf;
}

static void save_config(const char* full) {
    std::string dir = get_config_dir();
    if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) return;
    std::string path = dir + "/config";
    FILE* f = fopen(path.c_str(), "w");
    if (f) { fputs(full, f); fputc('\n', f); fclose(f); }
}

// ── Global state ──
static GtkWidget* ipEntry = nullptr;
static GtkWidget* ctrlCombo = nullptr;
static GtkWidget* connectBtn = nullptr;
static GtkWidget* statusLabel = nullptr;
static GtkWidget* ctrlLabel = nullptr;
static GtkWidget* pktLabel = nullptr;

static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_senderRunning{false};
static std::thread g_senderThread;
static uint8_t g_hmacKey[32]{};
static uint32_t g_packetCount = 0;

struct JoyInfo {
    std::string device;
    std::string name;
};

static std::vector<JoyInfo> g_joysticks;

// ── Scan joystick devices ──
static std::vector<JoyInfo> ScanJoysticks() {
    std::vector<JoyInfo> result;
    for (int i = 0; i < 32; i++) {
        std::string dev = "/dev/input/js" + std::to_string(i);
        int fd = open(dev.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            JoyInfo info;
            info.device = dev;
            char name[128]{};
            if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) >= 0)
                info.name = name;
            else
                info.name = "Joystick " + std::to_string(i);
            close(fd);
            result.push_back(info);
        }
    }
    return result;
}

// ── Joystick mapping (same as console client) ──
struct GamepadState {
    std::atomic<bool> buttons[16];
    std::atomic<int16_t> axes[8];
    GamepadState() {
        for (int i = 0; i < 16; i++) buttons[i].store(false, std::memory_order_relaxed);
        for (int i = 0; i < 8; i++) axes[i].store(0, std::memory_order_relaxed);
    }
    void copy_to(bool* dest_b, int16_t* dest_a) const {
        for (int i = 0; i < 16; i++) dest_b[i] = buttons[i].load(std::memory_order_relaxed);
        for (int i = 0; i < 8; i++) dest_a[i] = axes[i].load(std::memory_order_relaxed);
    }
};

static uint8_t apply_deadzone(int16_t val, bool invert, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    else scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

static ns::HIDReport map_linux_js_to_switch(const GamepadState& pad) {
    ns::HIDReport r; r.reset();
    bool b[16]; int16_t a[8];
    pad.copy_to(b, a);
    if (b[0]) r.buttons |= ns::BTN_B;
    if (b[1]) r.buttons |= ns::BTN_A;
    if (b[2]) r.buttons |= ns::BTN_Y;
    if (b[3]) r.buttons |= ns::BTN_X;
    if (b[4]) r.buttons |= ns::BTN_L;
    if (b[5]) r.buttons |= ns::BTN_R;
    if (a[2] > 0) r.buttons |= ns::BTN_ZL;
    if (a[5] > 0) r.buttons |= ns::BTN_ZR;
    if (b[6]) r.buttons |= ns::BTN_MINUS;
    if (b[7]) r.buttons |= ns::BTN_PLUS;
    if (b[9]) r.buttons |= ns::BTN_LSTICK;
    if (b[10]) r.buttons |= ns::BTN_RSTICK;
    if (b[9] && b[10]) { r.buttons |= ns::BTN_HOME; r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK); }
    if (b[6] && b[7]) { r.buttons |= ns::BTN_CAPTURE; r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS); }
    bool up = (a[7] < -16000), down = (a[7] > 16000);
    bool left = (a[6] < -16000), right = (a[6] > 16000);
    if (up && right) r.hat = ns::HAT_NE;
    else if (up && left) r.hat = ns::HAT_NW;
    else if (down && right) r.hat = ns::HAT_SE;
    else if (down && left) r.hat = ns::HAT_SW;
    else if (up) r.hat = ns::HAT_N;
    else if (down) r.hat = ns::HAT_S;
    else if (left) r.hat = ns::HAT_W;
    else if (right) r.hat = ns::HAT_E;
    r.lx = apply_deadzone(a[0], false);
    r.ly = apply_deadzone(a[1], false);
    r.rx = apply_deadzone(a[3], false);
    r.ry = apply_deadzone(a[4], false);
    return r;
}

// ── Sender thread ──
static void SenderThread(std::string device, std::string host, uint16_t port) {
    int js_fd = open(device.c_str(), O_RDONLY);
    if (js_fd < 0) return;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { close(js_fd); return; }

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dest.sin_addr) <= 0) {
        close(js_fd); close(sock);
        return;
    }

    GamepadState state;
    std::atomic<bool> running{true};
    g_senderRunning = true;

    std::thread sender_thread([&]() {
        uint32_t seq = 0;
        auto next_tick = std::chrono::steady_clock::now();
        while (running.load()) {
            while (std::chrono::steady_clock::now() < next_tick)
                std::atomic_thread_fence(std::memory_order_relaxed);
            ns::Packet pkt{};
            pkt.magic = ns::PROTO_MAGIC;
            pkt.version = ns::PROTO_VERSION;
            pkt.flags = ns::FLAG_NONE;
            pkt.seq = seq++;
            pkt.ts_us = ns::now_us();
            pkt.report = map_linux_js_to_switch(state);
            {
                uint8_t fullHmac[32];
                hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, fullHmac);
                memcpy(pkt.hmac, fullHmac, ns::HMAC_TAG_SIZE);
            }
            sendto(sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
            g_packetCount++;
            next_tick += std::chrono::milliseconds(2);
        }
    });

    // Main read loop
    struct js_event event;
    while (g_senderRunning && read(js_fd, &event, sizeof(event)) > 0) {
        uint8_t type = event.type & ~JS_EVENT_INIT;
        if (type == JS_EVENT_BUTTON && event.number < 16)
            state.buttons[event.number].store(event.value, std::memory_order_relaxed);
        else if (type == JS_EVENT_AXIS && event.number < 8)
            state.axes[event.number].store(event.value, std::memory_order_relaxed);
    }

    running = false;
    if (sender_thread.joinable()) sender_thread.join();
    close(js_fd);
    close(sock);
}

// ── GTK Callbacks ──
extern "C" void on_refresh_clicked(GtkWidget*, gpointer) {
    g_joysticks = ScanJoysticks();
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ctrlCombo));
    for (const auto& j : g_joysticks) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctrlCombo), j.name.c_str());
    }
    if (!g_joysticks.empty())
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctrlCombo), 0);
    if (g_joysticks.empty())
        gtk_label_set_text(GTK_LABEL(ctrlLabel), "No controllers detected");
    else
        gtk_label_set_text(GTK_LABEL(ctrlLabel), "");
}

extern "C" void on_connect_clicked(GtkWidget*, gpointer) {
    if (g_connected) {
        // Disconnect
        g_connected = false;
        g_senderRunning = false;
        if (g_senderThread.joinable()) g_senderThread.join();
        gtk_button_set_label(GTK_BUTTON(connectBtn), "Connect");
        gtk_widget_set_sensitive(ipEntry, TRUE);
        gtk_widget_set_sensitive(ctrlCombo, TRUE);
        gtk_label_set_text(GTK_LABEL(statusLabel), "Disconnected");
        gtk_label_set_text(GTK_LABEL(pktLabel), "Packets sent: 0");
        return;
    }

    const char* ipStr = gtk_entry_get_text(GTK_ENTRY(ipEntry));
    if (strlen(ipStr) == 0) return;

    // Parse ip:port (same as Windows client)
    char ipBuf[64];
    strncpy(ipBuf, ipStr, sizeof(ipBuf) - 1);
    ipBuf[sizeof(ipBuf) - 1] = '\0';
    int port = ns::DEFAULT_PORT;
    char* colon = strchr(ipBuf, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535) port = ns::DEFAULT_PORT;
    }

    int sel = gtk_combo_box_get_active(GTK_COMBO_BOX(ctrlCombo));
    if (sel < 0 || g_joysticks.empty()) return;

    save_config(ipStr);

    derive_key(ns::DEFAULT_SECRET, g_hmacKey);
    g_packetCount = 0;
    g_connected = true;

    std::string device = g_joysticks[sel].device;
    g_senderThread = std::thread(SenderThread, device, std::string(ipBuf), (uint16_t)port);

    gtk_button_set_label(GTK_BUTTON(connectBtn), "Disconnect");
    gtk_widget_set_sensitive(ipEntry, FALSE);
    gtk_widget_set_sensitive(ctrlCombo, FALSE);

    char status[128];
    snprintf(status, sizeof(status), "Connected to %s:%d", ipBuf, port);
    gtk_label_set_text(GTK_LABEL(statusLabel), status);
}

extern "C" gboolean on_timer(gpointer) {
    if (g_connected) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Packets sent: %u", g_packetCount);
        gtk_label_set_text(GTK_LABEL(pktLabel), buf);
    } else {
        // Refresh joystick list periodically
        auto oldCount = g_joysticks.size();
        g_joysticks = ScanJoysticks();
        if (g_joysticks.size() != oldCount ||
            (g_joysticks.empty() && gtk_combo_box_get_has_entry(GTK_COMBO_BOX(ctrlCombo)))) {
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ctrlCombo));
            for (const auto& j : g_joysticks) {
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctrlCombo), j.name.c_str());
            }
            if (!g_joysticks.empty())
                gtk_combo_box_set_active(GTK_COMBO_BOX(ctrlCombo), 0);
            if (g_joysticks.empty())
                gtk_label_set_text(GTK_LABEL(ctrlLabel), "No controllers detected");
            else
                gtk_label_set_text(GTK_LABEL(ctrlLabel), "");
        }
    }
    return G_SOURCE_CONTINUE;
}

static std::string get_exe_dir() {
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        std::string exe(buf);
        size_t pos = exe.find_last_of("/");
        if (pos != std::string::npos)
            return exe.substr(0, pos);
    }
    return ".";
}

// ── Entry point ──
int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Nintendo Switch PC Control");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 240);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_window_set_icon_from_file(GTK_WINDOW(window), (get_exe_dir() + "/icon.png").c_str(), nullptr);

    GtkWidget* grid = gtk_grid_new();
    gtk_container_add(GTK_CONTAINER(window), grid);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

    // Row 0: IP (supports ip:port format like Windows client)
    GtkWidget* ipLabel = gtk_label_new("Raspberry Pi IP:");
    gtk_widget_set_halign(ipLabel, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), ipLabel, 0, 0, 1, 1);

    ipEntry = gtk_entry_new();
    {
        std::string saved = load_saved_config();
        gtk_entry_set_text(GTK_ENTRY(ipEntry), saved.empty() ? "192.168.1.100" : saved.c_str());
    }
    gtk_grid_attach(GTK_GRID(grid), ipEntry, 1, 0, 5, 1);

    // Row 1: Controller
    GtkWidget* ctrlLabel2 = gtk_label_new("Controller:");
    gtk_widget_set_halign(ctrlLabel2, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), ctrlLabel2, 0, 1, 1, 1);

    ctrlCombo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(ctrlCombo, 200, -1);
    gtk_grid_attach(GTK_GRID(grid), ctrlCombo, 1, 1, 3, 1);

    GtkWidget* refreshBtn = gtk_button_new_with_label("Refresh");
    g_signal_connect(refreshBtn, "clicked", G_CALLBACK(on_refresh_clicked), nullptr);
    gtk_grid_attach(GTK_GRID(grid), refreshBtn, 4, 1, 2, 1);

    // Row 2: Buttons
    connectBtn = gtk_button_new_with_label("Connect");
    g_signal_connect(connectBtn, "clicked", G_CALLBACK(on_connect_clicked), nullptr);
    gtk_grid_attach(GTK_GRID(grid), connectBtn, 1, 2, 2, 1);

    // Separator row
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep, 0, 3, 6, 1);

    // Row 4: Status
    statusLabel = gtk_label_new("Ready");
    gtk_widget_set_halign(statusLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), statusLabel, 0, 4, 6, 1);

    ctrlLabel = gtk_label_new("No controller detected");
    gtk_widget_set_halign(ctrlLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), ctrlLabel, 0, 5, 6, 1);

    pktLabel = gtk_label_new("Packets sent: 0");
    gtk_widget_set_halign(pktLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), pktLabel, 0, 6, 6, 1);

    // Initial scan
    g_joysticks = ScanJoysticks();
    for (const auto& j : g_joysticks) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ctrlCombo), j.name.c_str());
    }
    if (!g_joysticks.empty())
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctrlCombo), 0);
    if (g_joysticks.empty())
        gtk_label_set_text(GTK_LABEL(ctrlLabel), "No controllers detected");

    // Timer for UI updates (100ms)
    g_timeout_add(100, on_timer, nullptr);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
