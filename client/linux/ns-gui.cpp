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
#include <cmath>
#include <mutex>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/joystick.h>
#include <sys/resource.h>

// Undefine conflicting macros
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

#include "../../server/rpi/include/sha256.h"

// Import external protocol structures (Version 4 with MultiReport)
#include "../../server/rpi/include/protocol.hpp"

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
static GtkWidget* connectBtn = nullptr;
static GtkWidget* statusLabel = nullptr;
static GtkWidget* pktLabel = nullptr;
static GtkWidget* ctrlLabels[4]; // Labels to display P1 to P4 status

static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_senderRunning{false};
static std::thread g_senderThread;
static uint8_t g_hmacKey[32]{};
static std::atomic<uint32_t> g_packetCount{0};

// ── Shared Gamepad State ──
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
static char         g_hw_names[4][128]; // Stored safely for the GTK thread to read
static std::mutex   g_hw_mtx;           // Protects hardware string arrays

// ── Axis conversion ──
static uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    else scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

// ── Input Mapping ──
static ns::HIDReport map_linux_js_to_switch(const GamepadState& pad) {
    ns::HIDReport r; r.reset();
    if (pad.buttons[0]) r.buttons |= ns::BTN_B; 
    if (pad.buttons[1]) r.buttons |= ns::BTN_A; 
    if (pad.buttons[2]) r.buttons |= ns::BTN_Y; 
    if (pad.buttons[3]) r.buttons |= ns::BTN_X; 
    if (pad.buttons[4]) r.buttons |= ns::BTN_L; 
    if (pad.buttons[5]) r.buttons |= ns::BTN_R; 
    if (pad.axes[2] > 0) r.buttons |= ns::BTN_ZL;
    if (pad.axes[5] > 0) r.buttons |= ns::BTN_ZR;
    if (pad.buttons[6]) r.buttons |= ns::BTN_MINUS;  
    if (pad.buttons[7]) r.buttons |= ns::BTN_PLUS;   
    if (pad.buttons[9])  r.buttons |= ns::BTN_LSTICK; 
    if (pad.buttons[10]) r.buttons |= ns::BTN_RSTICK; 
    if (pad.buttons[9] && pad.buttons[10]) { r.buttons |= ns::BTN_HOME; r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK); }
    if (pad.buttons[6] && pad.buttons[7]) { r.buttons |= ns::BTN_CAPTURE; r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS); }

    bool up = (pad.axes[7] < -16000), down = (pad.axes[7] > 16000);
    bool left = (pad.axes[6] < -16000), right = (pad.axes[6] > 16000);
    if (up && right) r.hat = ns::HAT_NE; else if (up && left) r.hat = ns::HAT_NW;
    else if (down && right) r.hat = ns::HAT_SE; else if (down && left) r.hat = ns::HAT_SW;
    else if (up) r.hat = ns::HAT_N; else if (down) r.hat = ns::HAT_S;
    else if (left) r.hat = ns::HAT_W; else if (right) r.hat = ns::HAT_E;

    r.lx = apply_deadzone(pad.axes[0], false); r.ly = apply_deadzone(pad.axes[1], false);  
    r.rx = apply_deadzone(pad.axes[3], false); r.ry = apply_deadzone(pad.axes[4], false);  
    return r;
}

// ── Smart Hardware Discovery ──
static bool is_valid_gamepad(int fd, std::string& out_name) {
    char name[128];
    if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) < 0) strncpy(name, "Unknown Device", sizeof(name));
    char axes = 0, buttons = 0;
    ioctl(fd, JSIOCGAXES, &axes);
    ioctl(fd, JSIOCGBUTTONS, &buttons);
    out_name = name;
    // Discard mice/keyboards: Real gamepads have at least 2 axes and 4 buttons
    return (axes >= 2 && buttons >= 4);
}

static void scan_for_gamepads() {
    static uint64_t last_scan = 0;
    uint64_t now = ns::now_us();
    if (now - last_scan < 1'000'000) return; // Scan once per second
    last_scan = now;

    for (int i = 0; i < 16; ++i) {
        char path[32]; snprintf(path, sizeof(path), "/dev/input/js%d", i);
        
        bool already_mapped = false;
        for (int p = 0; p < 4; ++p) {
            if (g_dev_paths[p] == path) { already_mapped = true; break; }
        }
        if (already_mapped) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        std::string hw_name;
        if (is_valid_gamepad(fd, hw_name)) {
            for (int p = 0; p < 4; ++p) {
                if (g_fds[p] < 0) {
                    std::lock_guard<std::mutex> lock(g_hw_mtx);
                    g_fds[p] = fd;
                    g_dev_paths[p] = path;
                    strncpy(g_hw_names[p], hw_name.c_str(), sizeof(g_hw_names[p]) - 1);
                    g_states[p].clear_inputs();
                    break;
                }
            }
        } else {
            close(fd);
        }
    }
}

// ── Read Controller State ──
static void read_pad(int index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    if (g_fds[index] < 0) { conn = false; return; }

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
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // Device physically unplugged
                std::lock_guard<std::mutex> lock(g_hw_mtx);
                close(g_fds[index]);
                g_fds[index] = -1;
                g_dev_paths[index] = "";
                g_hw_names[index][0] = '\0';
                conn = false;
                return;
            }
            break;
        }
    }
    conn = true;
    rep = map_linux_js_to_switch(g_states[index]);
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
        while (std::chrono::steady_clock::now() < next_tick)
            std::atomic_thread_fence(std::memory_order_relaxed);

        scan_for_gamepads();

        ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet)); 
        pkt.magic         = ns::PROTO_MAGIC;
        pkt.version       = ns::PROTO_VERSION;
        pkt.flags         = ns::FLAG_NONE;
        pkt.seq           = seq++;
        pkt.ts_us         = ns::now_us();
        pkt.report.reset(); 

        ns::HIDReport* out_reports[4] = { &pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4 };
        int active_count = 0;

        for (int i = 0; i < 4; ++i) {
            ns::HIDReport temp_rep;
            bool is_conn = false;
            read_pad(i, temp_rep, is_conn);
            if (is_conn && active_count < 4) {
                *out_reports[active_count] = temp_rep;
                active_count++;
            }
        }

        {
            uint8_t full_hmac[32];
            hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        }

        sendto(sock, (const char*)&pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
        g_packetCount++;
        
        if (active_count > 0) next_tick += std::chrono::milliseconds(2);
        else next_tick += std::chrono::milliseconds(500);
    }

    for (int i = 0; i < 4; ++i) {
        if (g_fds[i] >= 0) { close(g_fds[i]); g_fds[i] = -1; g_hw_names[i][0] = '\0'; g_dev_paths[i] = ""; }
    }
    close(sock);
}


// ── GTK Callbacks ──
extern "C" void on_connect_clicked(GtkWidget*, gpointer) {
    if (g_connected) {
        g_connected = false;
        g_senderRunning = false;
        if (g_senderThread.joinable()) g_senderThread.join();
        g_packetCount = 0;
        
        gtk_button_set_label(GTK_BUTTON(connectBtn), "Connect");
        gtk_widget_set_sensitive(ipEntry, TRUE);
        gtk_label_set_text(GTK_LABEL(statusLabel), "Disconnected");
        gtk_label_set_text(GTK_LABEL(pktLabel), "Packets sent: 0");
        
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

    save_config(ipStr);
    derive_key(ns::DEFAULT_SECRET, g_hmacKey);
    g_packetCount = 0;
    g_connected = true;

    for (int i=0; i<4; ++i) { g_hw_names[i][0] = '\0'; g_dev_paths[i] = ""; g_fds[i] = -1; }

    g_senderRunning = true;
    g_senderThread = std::thread(SenderThread, std::string(ipBuf), (uint16_t)port);

    gtk_button_set_label(GTK_BUTTON(connectBtn), "Disconnect");
    gtk_widget_set_sensitive(ipEntry, FALSE);

    char status[128]; snprintf(status, sizeof(status), "Connected to %s:%d", ipBuf, port);
    gtk_label_set_text(GTK_LABEL(statusLabel), status);
}

extern "C" gboolean on_timer(gpointer) {
    if (g_connected) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Packets sent: %u", g_packetCount.load());
        gtk_label_set_text(GTK_LABEL(pktLabel), buf);

        std::lock_guard<std::mutex> lock(g_hw_mtx);
        for (int i = 0; i < 4; ++i) {
            char lbl[128];
            if (g_hw_names[i][0] != '\0') snprintf(lbl, sizeof(lbl), "🎮 P%d: %s", i + 1, g_hw_names[i]);
            else snprintf(lbl, sizeof(lbl), "P%d: Waiting...", i + 1);
            gtk_label_set_text(GTK_LABEL(ctrlLabels[i]), lbl);
        }
    } else {
        // Run a silent discovery to preview what's plugged in before connecting
        scan_for_gamepads();
        std::lock_guard<std::mutex> lock(g_hw_mtx);
        for (int i = 0; i < 4; ++i) {
            char lbl[128];
            if (g_hw_names[i][0] != '\0') snprintf(lbl, sizeof(lbl), "🎮 P%d: %s", i + 1, g_hw_names[i]);
            else snprintf(lbl, sizeof(lbl), "P%d: Waiting...", i + 1);
            gtk_label_set_text(GTK_LABEL(ctrlLabels[i]), lbl);
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
    
    gtk_init(&argc, &argv);

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Nintendo Switch PC Control");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 280);
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
        std::string saved = load_saved_config();
        gtk_entry_set_text(GTK_ENTRY(ipEntry), saved.empty() ? "192.168.1.100" : saved.c_str());
    }
    gtk_grid_attach(GTK_GRID(grid), ipEntry, 1, 0, 3, 1);

    // Row 1: Connect Button
    connectBtn = gtk_button_new_with_label("Connect");
    g_signal_connect(connectBtn, "clicked", G_CALLBACK(on_connect_clicked), nullptr);
    gtk_grid_attach(GTK_GRID(grid), connectBtn, 1, 1, 3, 1);

    // Row 2: Separator
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), sep, 0, 2, 4, 1);

    // Row 3: Status
    statusLabel = gtk_label_new("Ready");
    gtk_widget_set_halign(statusLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), statusLabel, 0, 3, 4, 1);

    // Rows 4-7: P1 to P4 Slots
    for (int i = 0; i < 4; ++i) {
        char buf[64]; snprintf(buf, sizeof(buf), "P%d: Waiting...", i + 1);
        ctrlLabels[i] = gtk_label_new(buf);
        
        // Add some margin for visual indentation
        gtk_widget_set_margin_start(ctrlLabels[i], 10);
        gtk_widget_set_halign(ctrlLabels[i], GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), ctrlLabels[i], 0, 4 + i, 4, 1);
    }

    // Row 8: Packets
    pktLabel = gtk_label_new("Packets sent: 0");
    gtk_widget_set_halign(pktLabel, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), pktLabel, 0, 8, 4, 1);

    // Timer for UI updates (100ms)
    g_timeout_add(100, on_timer, nullptr);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}