#include "include/protocol.hpp"
#include "include/macros.hpp"
#include "include/sha256.h"

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
#include <sstream>
#include <fstream>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <dirent.h>
#include <ctype.h>
#include <cctype>

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#include <stdexcept>
#include <limits>
#endif

#include "webapp_embed.h"

using namespace ns;
using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

// ── Global flags ──────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static bool g_verbose = false;

// UDP clients can occasionally stall briefly while the app is still open.  Do
// not destroy/remap the whole PC session on a tiny UDP gap.  Instead neutralize
// stale input quickly, and only disconnect after a longer watchdog.
static constexpr uint64_t CLIENT_TIMEOUT_US = 30'000'000ULL;       // 30s hard disconnect
static constexpr uint64_t CLIENT_STALE_NEUTRAL_US = 350'000ULL; // 350ms release-to-neutral

// Precision rumble packets can encode very small non-neutral pulses.  Do not
// clamp every real packet to 80..255; preserve low magnitudes so clients can
// receive micro-rumble too.  all-zero per-motor frames are motor-off, not max.
static constexpr uint8_t RUMBLE_MIN_NONZERO = 4;      // floor for a real non-neutral half-frame
static constexpr int     RUMBLE_GAIN_PERCENT = 100;
// Normal-rumble build: decode console rumble into classic low/high packets only.
static std::string g_usb_serial = "NSBRIDGE000001";
static bool g_legacy_mode = false;

// Built-in USB gadget lifecycle.  ns-backend can now create/bind the
// USB gamepad gadget itself on startup and unbind/remove it
// on shutdown, so setup_gadget.sh is no longer needed at runtime.
static std::atomic<bool> g_gadget_setup_attempted{false};

// Experimental Switch 2 wake helper. A one-time setup (-wake) captures the
// paired Joy-Con 2 wake MAC + raw BLE advertising payload and stores them in
// /etc/ns-pc-control/switch2_wakeup.conf. At runtime, the first new client
// connection sends a short raw-HCI wake advert burst using that saved identity.
static bool g_switch2_wake_adv_enabled = true;
static bool g_switch2_wake_config_loaded = false;
static bool g_switch2_run_wake_setup = false;
static std::string g_switch2_wake_config_path = "/etc/ns-pc-control/switch2_wakeup.conf";
static std::string g_switch2_wake_mac;
static std::string g_switch2_wake_adv;
static std::atomic<bool> g_switch2_wake_adv_running{false};
static std::atomic<uint64_t> g_switch2_last_wake_adv_us{0};
// Writer threads update this using the same host-disconnect/open-failure
// signals already used for the "Host disconnected" backend logs. Runtime
// wake is therefore gated as: new client + console USB host not connected.
static std::atomic<bool> g_switch2_usb_host_connected{false};
static constexpr uint64_t SWITCH2_WAKE_ADV_COOLDOWN_US = 8'000'000ULL;
static constexpr int SWITCH2_WAKE_ADV_BURST_MS = 1000;

static constexpr int HID_PORT_COUNT = 4;

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
    ExtendedMultiReport report{}; // Inputs + optional motion coming from this specific PC/WebSocket

    // Match the simple ESP32 bridge behavior: keep three IMU samples and shift
    // them at roughly 5 ms spacing, with the newest sample in slot 2.
    MotionReport motion_samples[4][3]{};
    bool         has_motion[4]{};
    uint64_t     motion_last_collect_us[4]{};

    RumblePacket rumble[4]{};
    PrecisionRumblePacket precision_rumble[4]{};
    uint32_t    rumble_seq[4]{};
    bool        rumble_active[4]{};
    // Extended UDP clients can opt into server->client rumble by using the
    // authenticated extended packet format. Legacy UDP clients remain input-only.
    bool        udp_rumble_enabled = false;
    uint32_t    udp_last_rumble_seq[4]{};

    // Web/mobile packets set this even when the pad is neutral.  Without it,
    // the console port only maps after a non-neutral input, so early rumble can
    // be dropped before the browser has a rumble target.
    bool        pad_present[4]{};
    uint64_t    pad_last_present_us[4]{};
    bool        uses_pad_presence = false;
};

static std::mutex    g_mtx[MAX_CLIENTS];
static ClientSession g_clients[MAX_CLIENTS];

static uint64_t elapsed_us_saturated(uint64_t now, uint64_t then) {
    // All runtime timestamps should come from ns::now_us()/steady_clock, but
    // never let a bad/future timestamp wrap an unsigned subtraction into
    // ~UINT64_MAX.  That was causing bogus logs like:
    //   timed out after 18446744073709.6s
    if (then == 0 || then > now) return 0;
    return now - then;
}

static bool elapsed_us_over(uint64_t now, uint64_t then, uint64_t limit) {
    return then != 0 && elapsed_us_saturated(now, then) > limit;
}

static bool any_recent_client_active(uint64_t now) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_mtx[i]);
        const ClientSession& c = g_clients[i];
        if (c.active && c.last_rx_us != 0 &&
            elapsed_us_saturated(now, c.last_rx_us) <= CLIENT_TIMEOUT_US) {
            return true;
        }
    }
    return false;
}

static void repair_future_client_timestamp(ClientSession& c, uint64_t now) {
    if (c.active && (c.last_rx_us == 0 || c.last_rx_us > now)) {
        c.last_rx_us = now;
    }
}

static void clear_motion(ClientSession& c, int subpad) {
    if (subpad < 0 || subpad >= 4) return;
    for (int i = 0; i < 3; ++i) c.motion_samples[subpad][i].reset();
    c.has_motion[subpad] = false;
    c.motion_last_collect_us[subpad] = 0;
}

static void clear_all_motion(ClientSession& c) {
    for (int s = 0; s < 4; ++s) clear_motion(c, s);
}

static void set_motion(ClientSession& c, int subpad, const MotionReport& motion) {
    if (subpad < 0 || subpad >= 4) return;

    uint64_t now = now_us();
    if (!c.has_motion[subpad]) {
        for (int i = 0; i < 3; ++i) c.motion_samples[subpad][i] = motion;
    } else if (elapsed_us_saturated(now, c.motion_last_collect_us[subpad]) > 5000ULL) {
        c.motion_samples[subpad][0] = c.motion_samples[subpad][1];
        c.motion_samples[subpad][1] = c.motion_samples[subpad][2];
    }
    c.motion_samples[subpad][2] = motion;
    c.has_motion[subpad] = true;
    c.motion_last_collect_us[subpad] = now;
}

static void set_motion_samples(ClientSession& c, int subpad, const MotionReport samples[3]) {
    if (subpad < 0 || subpad >= 4 || !samples) return;
    c.motion_samples[subpad][0] = samples[0];
    c.motion_samples[subpad][1] = samples[1];
    c.motion_samples[subpad][2] = samples[2];
    c.has_motion[subpad] = true;
    c.motion_last_collect_us[subpad] = now_us();
}

// Diagnostics
static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};


// ── Server-side macro playback ───────────────────────────────────────────────────────────
// Shared parser/export/playback helpers live in include/macros.hpp.
// This file keeps only server-specific runtime/upload wiring.
struct ServerMacroRuntime {
    std::vector<ns::macro::Step> steps;
    bool running = false;
    uint64_t start_us = 0;
};

static std::mutex g_server_macro_mtx;
static ServerMacroRuntime g_server_macros[MAX_CLIENTS][4];

struct ServerMacroUploadRuntime {
    bool active = false;
    sockaddr_in sender{};
    uint32_t upload_id = 0;
    uint8_t subpad = 0;
    uint32_t total_len = 0;
    uint32_t chunk_count = 0;
    uint32_t received_count = 0;
    uint64_t last_rx_us = 0;
    std::vector<std::string> chunks;
    std::vector<uint8_t> got;
};

static std::mutex g_server_macro_upload_mtx;
static ServerMacroUploadRuntime g_server_macro_uploads[MAX_CLIENTS];

static bool rate_allow(uint32_t ip);
static bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands);
static void maybe_send_switch2_wake_advert(const char* reason);
static bool load_switch2_wakeup_config(bool verbose_missing);
static int run_switch2_wakeup_setup();

static int server_macro_client_for_sender(const sockaddr_in& sender) {
    uint32_t src_ip = sender.sin_addr.s_addr;
    uint64_t now = now_us();
    int client_idx = -1;
    bool wake_on_new_client = false;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_mtx[i]);
        if (g_clients[i].active && g_clients[i].addr.sin_addr.s_addr == src_ip && g_clients[i].addr.sin_port == sender.sin_port) { client_idx = i; break; }
    }
    if (client_idx == -1) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            std::lock_guard<std::mutex> lk(g_mtx[i]);
            repair_future_client_timestamp(g_clients[i], now);
            if (!g_clients[i].active || elapsed_us_over(now, g_clients[i].last_rx_us, CLIENT_TIMEOUT_US)) {
                client_idx = i;
                g_clients[i].active = true;
                g_clients[i].addr = sender;
                g_clients[i].first_pkt = true;
                g_clients[i].expected_seq = 0;
                g_clients[i].report.reset();
                clear_all_motion(g_clients[i]);
                g_clients[i].last_rx_us = now;
                wake_on_new_client = true;
                break;
            }
        }
    }
    if (client_idx >= 0) {
        std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
        g_clients[client_idx].active = true;
        g_clients[client_idx].addr = sender;
        g_clients[client_idx].last_rx_us = now;
    }
    if (wake_on_new_client)
        maybe_send_switch2_wake_advert("client connected via UDP macro upload");
    return client_idx;
}

static bool server_macro_handle_chunk_packet(const uint8_t* data, size_t bytes, const sockaddr_in& sender) {
    if (bytes < ns::macro::CHUNK_HEADER_SIZE + HMAC_TAG_SIZE) return false;
    ns::macro::MacroUdpChunkHeaderWire h{};
    memcpy(&h, data, sizeof(h));
    if (h.magic != ns::macro::UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION) { if (g_verbose) puts("bad macro chunk version, dropped"); return true; }
    if (h.total_len > ns::macro::UDP_TEXT_MAX) { if (g_verbose) puts("macro chunk total over 50MB, dropped"); return true; }
    if (h.chunk_len > ns::macro::UDP_CHUNK_MAX) { if (g_verbose) puts("macro chunk too large, dropped"); return true; }
    if (h.chunk_count == 0 || h.chunk_index >= h.chunk_count) { if (g_verbose) puts("bad macro chunk index/count, dropped"); return true; }
    if (bytes != ns::macro::CHUNK_HEADER_SIZE + (size_t)h.chunk_len + HMAC_TAG_SIZE) { if (g_verbose) puts("bad macro chunk packet size, dropped"); return true; }
    const uint8_t* recv_hmac = data + ns::macro::CHUNK_HEADER_SIZE + h.chunk_len;
    if (hmac_verify(g_hmac_key, 32, data, ns::macro::CHUNK_HEADER_SIZE + h.chunk_len, recv_hmac, HMAC_TAG_SIZE) != 0) { if (g_verbose) puts("bad macro chunk HMAC, dropped"); return true; }
    if (!rate_allow(sender.sin_addr.s_addr)) return true;

    uint64_t now = now_us();
    int client_idx = server_macro_client_for_sender(sender);
    if (client_idx < 0) return true;

    std::string completed;
    uint8_t completed_subpad = h.subpad < 4 ? h.subpad : 0;
    {
        std::lock_guard<std::mutex> lk(g_server_macro_upload_mtx);
        ServerMacroUploadRuntime& up = g_server_macro_uploads[client_idx];
        bool same = up.active && up.upload_id == h.upload_id &&
                    up.sender.sin_addr.s_addr == sender.sin_addr.s_addr &&
                    up.sender.sin_port == sender.sin_port;
        if (!same) {
            up = ServerMacroUploadRuntime{};
            up.active = true;
            up.sender = sender;
            up.upload_id = h.upload_id;
            up.subpad = h.subpad < 4 ? h.subpad : 0;
            up.total_len = h.total_len;
            up.chunk_count = h.chunk_count;
            try {
                up.chunks.assign(h.chunk_count, std::string());
                up.got.assign(h.chunk_count, 0);
            } catch (...) {
                up = ServerMacroUploadRuntime{};
                if (g_verbose) puts("macro chunk allocation failed");
                return true;
            }
        }
        if (up.total_len != h.total_len || up.chunk_count != h.chunk_count) { if (g_verbose) puts("macro chunk metadata mismatch, dropped"); return true; }
        up.last_rx_us = now;
        if (!up.got[h.chunk_index]) {
            up.chunks[h.chunk_index].assign(reinterpret_cast<const char*>(data + ns::macro::CHUNK_HEADER_SIZE), h.chunk_len);
            up.got[h.chunk_index] = 1;
            up.received_count++;
        }
        if (up.received_count == up.chunk_count) {
            size_t total = 0;
            for (const auto& c : up.chunks) total += c.size();
            if (total != up.total_len) { if (g_verbose) puts("macro chunk final size mismatch"); up = ServerMacroUploadRuntime{}; return true; }
            completed.reserve(total);
            for (const auto& c : up.chunks) completed += c;
            completed_subpad = up.subpad;
            up = ServerMacroUploadRuntime{};
        }
    }
    if (!completed.empty()) {
        if (g_verbose) std::printf("[macro] received chunked macro %zu bytes\n", completed.size());
        server_macro_start(client_idx, completed_subpad, completed);
    }
    return true;
}


static bool server_macro_handle_ws_chunk_packet(int client_idx, const uint8_t* data, size_t bytes) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return false;
    if (bytes < ns::macro::CHUNK_HEADER_SIZE) return false;
    ns::macro::MacroUdpChunkHeaderWire h{};
    memcpy(&h, data, sizeof(h));
    if (h.magic != ns::macro::UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION && h.version != WEB_PROTO_VERSION) return true;
    if (h.total_len > ns::macro::UDP_TEXT_MAX || h.chunk_len > ns::macro::UDP_CHUNK_MAX || h.chunk_count == 0 || h.chunk_index >= h.chunk_count) return true;
    if (bytes != ns::macro::CHUNK_HEADER_SIZE + (size_t)h.chunk_len) return true;
    uint64_t now = now_us();
    std::string completed;
    uint8_t completed_subpad = h.subpad < 4 ? h.subpad : 0;
    {
        std::lock_guard<std::mutex> lk(g_server_macro_upload_mtx);
        ServerMacroUploadRuntime& up = g_server_macro_uploads[client_idx];
        bool same = up.active && up.upload_id == h.upload_id;
        if (!same) {
            up = ServerMacroUploadRuntime{};
            up.active = true;
            up.upload_id = h.upload_id;
            up.subpad = h.subpad < 4 ? h.subpad : 0;
            up.total_len = h.total_len;
            up.chunk_count = h.chunk_count;
            try { up.chunks.assign(h.chunk_count, std::string()); up.got.assign(h.chunk_count, 0); }
            catch (...) { up = ServerMacroUploadRuntime{}; return true; }
        }
        if (up.total_len != h.total_len || up.chunk_count != h.chunk_count) return true;
        up.last_rx_us = now;
        if (!up.got[h.chunk_index]) {
            up.chunks[h.chunk_index].assign(reinterpret_cast<const char*>(data + ns::macro::CHUNK_HEADER_SIZE), h.chunk_len);
            up.got[h.chunk_index] = 1;
            up.received_count++;
        }
        if (up.received_count == up.chunk_count) {
            size_t total = 0; for (const auto& c : up.chunks) total += c.size();
            if (total != up.total_len) { up = ServerMacroUploadRuntime{}; return true; }
            completed.reserve(total); for (const auto& c : up.chunks) completed += c;
            completed_subpad = up.subpad;
            up = ServerMacroUploadRuntime{};
        }
    }
    if (!completed.empty()) server_macro_start(client_idx, completed_subpad, completed);
    return true;
}

static bool server_macro_running(int client_idx, int subpad) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return false;
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    ServerMacroRuntime& rt = g_server_macros[client_idx][subpad];
    if (!rt.running) return false;
    uint64_t elapsed_ms = (now_us() - rt.start_us) / 1000ULL;
    if (elapsed_ms > ns::macro::total_ms(rt.steps) + 120) { rt.running = false; return false; }
    return true;
}

static void server_macro_apply(int client_idx, int subpad, HIDReport& live) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return;
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    ServerMacroRuntime& rt = g_server_macros[client_idx][subpad];
    if (!rt.running) return;
    uint64_t elapsed_ms = (now_us() - rt.start_us) / 1000ULL;
    ns::macro::Step step{};
    if (!ns::macro::step_at(rt.steps, elapsed_ms, step)) {
        rt.running = false;
        return;
    }
    live.buttons |= step.buttons;
    if (step.hat != HAT_NEUTRAL && live.hat == HAT_NEUTRAL) live.hat = step.hat;
    if (step.has_lstick) { live.lx = step.lx; live.ly = step.ly; }
    if (step.has_rstick) { live.rx = step.rx; live.ry = step.ry; }
}

static bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return false;
    if (subpad < 0 || subpad >= 4) subpad = 0;
    std::vector<ns::macro::Step> steps;
    if (!ns::macro::validate_text(json_or_commands, steps, nullptr)) {
        if (g_verbose) std::printf("[macro] rejected: %s\n", ns::macro::last_error().c_str());
        return false;
    }
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    ServerMacroRuntime& rt = g_server_macros[client_idx][subpad];
    rt.steps = std::move(steps);
    rt.running = true;
    rt.start_us = now_us();
    if (g_verbose) std::printf("[macro] started server macro slot=%d pad=%d\n", client_idx + 1, subpad + 1);
    return true;
}

[[maybe_unused]] static void server_macro_stop_all_for_client(int client_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    for (int s = 0; s < 4; ++s) g_server_macros[client_idx][s].running = false;
}

// ── Signal ────────────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

static uint8_t pro_timer_from_us(uint64_t t_us) {
    // The byte after report ID 0x30/0x21 is a small controller timer.  Real
    // controllers advance it with time, not merely by "one per report"; using
    // a 5ms unit matches the 3x IMU sample spacing most software expects.
    return (uint8_t)((t_us / 5000ULL) & 0xFF);
}



// ── Vendor USB gamepad USB protocol support ─────────────────────────────
static constexpr size_t PRO_REPORT_SIZE = 64;
// Full input reports run at the shared 250Hz cadence.
static constexpr uint64_t PRO_REPORT_INTERVAL_US = 4'000ULL;
static constexpr int PRO_WRITER_HZ = 1'000'000 / PRO_REPORT_INTERVAL_US;
static constexpr uint8_t  PRO_BAT_CON = 0x91;
static constexpr uint8_t  PRO_VIBRATOR_REPORT = 0x0B;
static constexpr int PRO_IDLE_REPORT_HZ = 30;
static constexpr uint64_t PRO_IDLE_REPORT_INTERVAL_US = 1'000'000ULL / PRO_IDLE_REPORT_HZ;
static constexpr uint64_t PRO_RELEASE_NEUTRAL_US = 250'000ULL;
// Web/Gamepad APIs can occasionally report a connected pad as absent for one
// frame (especially while another tab/client is closing).  Do not release the
// console port instantly on a single absent sample, or held buttons like R get
// chopped into tiny taps.
static constexpr uint64_t WEB_PAD_ABSENT_RELEASE_US = 750'000ULL;
static constexpr uint8_t RID_INPUT_STANDARD = 0x30;
static constexpr uint8_t RID_INPUT_SUBCMD   = 0x21;
static constexpr uint8_t RID_OUTPUT_RUMBLE  = 0x10;
static constexpr uint8_t RID_OUTPUT_CMD     = 0x01;

static constexpr uint8_t CMD_BT_MANUAL_PAIRING   = 0x01;
static constexpr uint8_t CMD_GET_DEVICE_INFO     = 0x02;
static constexpr uint8_t CMD_SET_DATA_FORMAT     = 0x03;
static constexpr uint8_t CMD_TRIGGER_BUTTONS     = 0x04;
static constexpr uint8_t CMD_SET_SHIP_MODE       = 0x08;
static constexpr uint8_t CMD_SPI_FLASH_READ      = 0x10;
static constexpr uint8_t CMD_SET_NFC_IR_CONFIG   = 0x21;
static constexpr uint8_t CMD_SET_PLAYER_LIGHTS   = 0x30;
static constexpr uint8_t CMD_ENABLE_IMU          = 0x40;
static constexpr uint8_t CMD_SET_IMU_SENS        = 0x41;
static constexpr uint8_t CMD_ENABLE_VIBRATION    = 0x48;

#define NS_LOCAL_PACKED __attribute__((packed))

struct NS_LOCAL_PACKED ProInputReport30 {
    uint8_t id;
    uint8_t timer;
    uint8_t conn_info;
    uint8_t buttons[3];
    uint8_t left_stick[3];
    uint8_t right_stick[3];
    uint8_t vibrator;
    // The 64-byte IMU wire order is Y, X, Z for each sample.
    // Keep field names matching the logical axis they contain so assignment
    // remains readable while packed layout matches hardware.
    int16_t accel_y_0, accel_x_0, accel_z_0;
    int16_t gyro_y_0,  gyro_x_0,  gyro_z_0;
    int16_t accel_y_1, accel_x_1, accel_z_1;
    int16_t gyro_y_1,  gyro_x_1,  gyro_z_1;
    int16_t accel_y_2, accel_x_2, accel_z_2;
    int16_t gyro_y_2,  gyro_x_2,  gyro_z_2;
    uint8_t vendor_rest[15];
};
static_assert(sizeof(ProInputReport30) == PRO_REPORT_SIZE, "ProInputReport30 must be 64 bytes");

struct NS_LOCAL_PACKED ProInputReport21 {
    uint8_t id;
    uint8_t timer;
    uint8_t conn_info;
    uint8_t buttons[3];
    uint8_t left_stick[3];
    uint8_t right_stick[3];
    uint8_t vibrator;
    uint8_t ack;
    uint8_t subcmd_id;
    uint8_t reply_data[49];
};
static_assert(sizeof(ProInputReport21) == PRO_REPORT_SIZE, "ProInputReport21 must be 64 bytes");

static uint8_t CTRL_MAC_BE[4][6] = {
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA0},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA1},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA2},
    {0x02, 0x4E, 0x53, 0x26, 0x06, 0xA3},
};

static std::string CTRL_SERIAL[4] = {
    "NSGP260606A0", "NSGP260606A1", "NSGP260606A2", "NSGP260606A3"
};

static bool read_random_bytes(uint8_t* dst, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t r = read(fd, dst + off, len - off);
            if (r <= 0) break;
            off += (size_t)r;
        }
        close(fd);
        if (off == len) return true;
    }

    uint64_t seed = (uint64_t)now_us() ^ ((uint64_t)getpid() << 32);
    for (size_t i = 0; i < len; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        dst[i] = (uint8_t)(seed & 0xFF);
    }
    return true;
}

static void randomize_controller_identity() {
    uint8_t rnd[16]{};
    read_random_bytes(rnd, sizeof(rnd));

    // Locally administered unicast MACs. Keep 02:4E:53 ("NS") as a stable
    // virtual vendor prefix and randomize the low bytes so the host cannot
    // reuse cached calibration/association for the previous virtual controller.
    for (int i = 0; i < 4; ++i) {
        CTRL_MAC_BE[i][0] = 0x02;
        CTRL_MAC_BE[i][1] = 0x4E;
        CTRL_MAC_BE[i][2] = 0x53;
        CTRL_MAC_BE[i][3] = rnd[(i * 3 + 0) % sizeof(rnd)];
        CTRL_MAC_BE[i][4] = rnd[(i * 3 + 1) % sizeof(rnd)];
        CTRL_MAC_BE[i][5] = (uint8_t)(rnd[(i * 3 + 2) % sizeof(rnd)] + i);
        CTRL_SERIAL[i] = "NSGP";
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "%02X%02X%02X%02X",
                      CTRL_MAC_BE[i][2], CTRL_MAC_BE[i][3], CTRL_MAC_BE[i][4], CTRL_MAC_BE[i][5]);
        CTRL_SERIAL[i] += suffix;
    }

    char usb_ser[32];
    std::snprintf(usb_ser, sizeof(usb_ser), "%02X%02X%02X%02X%02X%02X",
                  rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
    g_usb_serial = usb_ser;
}

static constexpr size_t SPI_FLASH_SIZE = 0x10000;
static uint8_t g_spi_flash[4][SPI_FLASH_SIZE];
static bool g_spi_initialized[4] = {};

struct ControllerRuntime {
    int fd = -1;
    int ctrl = 0;
    uint8_t timer = 0;
    bool full_report_enabled = false;
    bool imu_enabled = false;
    bool vibration_enabled = false;
    bool usb_seen_mac = false;
    bool usb_handshake_done = false;
    bool usb_baudrate_set = false;
    bool usb_timeout_disabled = false;
    bool pending_subcmd_reply = false;
    uint64_t last_standard_report_us = 0;
    uint64_t last_idle_neutral_us = 0;
    uint64_t neutral_burst_until_us = 0;
    ProInputReport21 pending_reply{};
};

[[maybe_unused]] static int16_t clamp_i16(int v) {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
}

[[maybe_unused]] static void pack12(uint16_t val, uint8_t& b0, uint8_t& b1) {
    b0 = val & 0xFF;
    b1 = (b1 & 0xF0) | ((val >> 8) & 0x0F);
}

static void init_spi_flash(int ctrl) {
    if (ctrl < 0 || ctrl >= 4 || g_spi_initialized[ctrl]) return;

    uint8_t* flash = g_spi_flash[ctrl];
    memset(flash, 0xFF, SPI_FLASH_SIZE);

    // Public synthetic SPI profile v2.
    // This intentionally contains no dump/private bytes.  It only builds a
    // coherent, boring factory profile from generic calibration constants.
    // Key points learned from compatibility testing:
    //   - report as USB gamepad type 0x03
    //   - no user IMU calibration magic at 0x8026
    //   - prefer factory calibration blocks instead of invented user blocks
    //   - keep 0x6098 as stick-model continuation, not IMU calibration
    flash[0x6012] = 0x03;
    flash[0x6013] = 0xA0;
    flash[0x601B] = 0x02;

    auto put_i16_le = [&](uint16_t addr, int16_t val) {
        flash[addr]     = (uint8_t)(val & 0xFF);
        flash[addr + 1] = (uint8_t)((uint16_t)val >> 8);
    };

    auto pack12_pair = [](uint8_t* dst, uint16_t x, uint16_t y) {
        x &= 0x0FFF;
        y &= 0x0FFF;
        dst[0] = (uint8_t)(x & 0xFF);
        dst[1] = (uint8_t)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
        dst[2] = (uint8_t)((y >> 4) & 0xFF);
    };

    // Factory stick calibration: neutral center with symmetric, conservative
    // range.  Do not set user-cal magic; forcing factory cal avoids a mixed
    // synthetic factory/user profile that some hosts handle badly.
    static constexpr uint16_t STICK_CENTER = 0x800;
    static constexpr uint16_t STICK_RANGE  = 0x600;

    uint8_t left_cal[9]{};
    uint8_t right_cal[9]{};
    pack12_pair(left_cal  + 0, STICK_RANGE,  STICK_RANGE);
    pack12_pair(left_cal  + 3, STICK_CENTER, STICK_CENTER);
    pack12_pair(left_cal  + 6, STICK_RANGE,  STICK_RANGE);
    pack12_pair(right_cal + 0, STICK_CENTER, STICK_CENTER);
    pack12_pair(right_cal + 3, STICK_RANGE,  STICK_RANGE);
    pack12_pair(right_cal + 6, STICK_RANGE,  STICK_RANGE);

    memcpy(flash + 0x603D, left_cal,  sizeof(left_cal));
    memcpy(flash + 0x6046, right_cal, sizeof(right_cal));

    // Explicitly erase user stick/IMU calibration magic areas.  The host should
    // use the factory blocks above rather than half-synthetic user cal.
    memset(flash + 0x8010, 0xFF, 0x30);
    flash[0x8026] = 0xFF;
    flash[0x8027] = 0xFF;
    memset(flash + 0x8028, 0xFF, 0x18);

    // Factory IMU calibration at 0x6020, 24 bytes:
    // accel offsets XYZ, accel scales XYZ, gyro offsets XYZ, gyro scales XYZ.
    // Values are generic and synthetic, not copied from any controller dump.
    static constexpr int16_t IMU_ACCEL_OFFSET = 0;
    static constexpr int16_t IMU_ACCEL_SCALE  = 0x4000;
    static constexpr int16_t IMU_GYRO_OFFSET  = 0;
    static constexpr int16_t IMU_GYRO_SCALE   = 0x343B;
    const int16_t imu_vals[12] = {
        IMU_ACCEL_OFFSET, IMU_ACCEL_OFFSET, IMU_ACCEL_OFFSET,
        IMU_ACCEL_SCALE,  IMU_ACCEL_SCALE,  IMU_ACCEL_SCALE,
        IMU_GYRO_OFFSET,  IMU_GYRO_OFFSET,  IMU_GYRO_OFFSET,
        IMU_GYRO_SCALE,   IMU_GYRO_SCALE,   IMU_GYRO_SCALE
    };
    for (int i = 0; i < 12; ++i)
        put_i16_le((uint16_t)(0x6020 + i * 2), imu_vals[i]);

    // 0x6080..0x6085: IMU horizontal offsets, three int16 values.
    put_i16_le(0x6080, 0);
    put_i16_le(0x6082, 0);
    put_i16_le(0x6084, 0);

    // 0x6086..0x60A9: stick model/parameter block.  Keep it coherent instead
    // of all-0xFF or random bytes.  Bytes 3..5 are commonly unpacked as two
    // packed 12-bit values: deadzone and range ratio.
    uint8_t stick_model[0x24]{};
    static constexpr uint16_t STICK_DEADZONE    = 0x0A0;
    static constexpr uint16_t STICK_RANGE_RATIO = 0x100;
    pack12_pair(stick_model + 3, STICK_DEADZONE, STICK_RANGE_RATIO);
    // Soft, generic model tail.  These are synthetic low-entropy defaults; they
    // are only here to avoid an empty/0xFF model continuation.
    for (size_t i = 6; i < sizeof(stick_model); ++i)
        stick_model[i] = (i & 1) ? 0x30 : 0x0F;
    memcpy(flash + 0x6086, stick_model, sizeof(stick_model));

    // Controller colors: synthetic per-slot colors with white buttons.
    static const uint8_t BODY_RGB[4][3] = {
        {0xE6, 0x00, 0x12},
        {0xFF, 0xCC, 0x00},
        {0x00, 0x64, 0xFF},
        {0x00, 0xC8, 0x53},
    };
    const uint8_t* body = BODY_RGB[ctrl];
    flash[0x6050] = body[0]; flash[0x6051] = body[1]; flash[0x6052] = body[2];
    flash[0x6053] = 0xFF;    flash[0x6054] = 0xFF;    flash[0x6055] = 0xFF;
    flash[0x6056] = body[0]; flash[0x6057] = body[1]; flash[0x6058] = body[2];
    flash[0x6059] = 0xFF;    flash[0x605A] = 0xFF;    flash[0x605B] = 0xFF;
    flash[0x605C] = 0x00;

    g_spi_initialized[ctrl] = true;
}

static void set_identity_in_0x81(uint8_t* resp_81, int ctrl) {
    const uint8_t* mac = CTRL_MAC_BE[ctrl];
    // USB 0x81 MAC reply stores MAC little-endian in bytes 4..9, matching
    // Chromium's MacAddressReport/UnpackconsoleMacAddress handling.
    resp_81[4] = mac[5]; resp_81[5] = mac[4]; resp_81[6] = mac[3];
    resp_81[7] = mac[2]; resp_81[8] = mac[1]; resp_81[9] = mac[0];
}

static size_t build_usb_81_response(uint8_t* out, uint8_t subtype, int ctrl) {
    memset(out, 0, PRO_REPORT_SIZE);
    out[0] = 0x81;
    out[1] = subtype;
    switch (subtype) {
    case 0x01: // request MAC/address/device type
        out[2] = 0x00; // padding
        out[3] = 0x03; // USB gamepad
        set_identity_in_0x81(out, ctrl);
        break;
    case 0x02: // USB handshake
    case 0x03: // set UART/baudrate
    case 0x04: // disable USB timeout; real devices may not ACK, but an ACK is accepted
    case 0x05: // enable USB timeout
    default:
        // Chromium and hid-Vendor only require report 0x81 subtype to advance
        // these USB-init steps. Keep remaining bytes zero, like a minimal ACK.
        break;
    }
    return PRO_REPORT_SIZE;
}

static void build_get_device_info_response(uint8_t* out, int ctrl) {
    memset(out, 0, 36);

    // Subcmd 0x02 device-info reply.
    //
    //   majorVersion   = 0x03
    //   minorVersion   = 0x49
    //   controllerType = 0x03
    //   unknown00      = 0x02
    //   macAddress     = generated MAC, reversed/little-endian
    //   unknown01      = 0x01
    //   storedColors   = 0x02
    //
    // Important: MAC stays generated per virtual controller.
    out[0] = 0x03; // majorVersion
    out[1] = 0x49; // minorVersion
    out[2] = 0x03; // controllerType: 64-byte controller
    out[3] = 0x02; // unknown00

    const uint8_t* mac = CTRL_MAC_BE[ctrl];

    // Device info wants MAC reversed / little-endian.
    out[4] = mac[5];
    out[5] = mac[4];
    out[6] = mac[3];
    out[7] = mac[2];
    out[8] = mac[1];
    out[9] = mac[0];

    out[10] = 0x01; // unknown01
    out[11] = 0x02; // storedColors
}

static void fill_neutral_controls(ProInputReport30& r) {
    r.conn_info = PRO_BAT_CON;
    // Real USB USB gamepad captures keep bit 0x80 set in the middle button byte
    // even at rest: neutral buttons are 00 80 00, not 00 00 00.  Keep this base
    // bit in both 0x30 and 0x21 snapshots because some console/game paths appear
    // to gate motion/precision-rumble capability on the complete controller state, not
    // merely on the IMU bytes.
    r.buttons[0] = 0x00;
    r.buttons[1] = 0x80;
    r.buttons[2] = 0x00;
    r.left_stick[0]  = 0x00; r.left_stick[1]  = 0x08; r.left_stick[2]  = 0x80;
    r.right_stick[0] = 0x00; r.right_stick[1] = 0x08; r.right_stick[2] = 0x80;
    r.vibrator = PRO_VIBRATOR_REPORT;
}

static void fill_neutral_controls(ProInputReport21& r) {
    r.conn_info = PRO_BAT_CON;
    r.buttons[0] = 0x00;
    r.buttons[1] = 0x80;
    r.buttons[2] = 0x00;
    r.left_stick[0]  = 0x00; r.left_stick[1]  = 0x08; r.left_stick[2]  = 0x80;
    r.right_stick[0] = 0x00; r.right_stick[1] = 0x08; r.right_stick[2] = 0x80;
    r.vibrator = PRO_VIBRATOR_REPORT;
}

static uint16_t axis8_to_12(uint8_t v) {
    // Match the fake calibration above: center 0x800 with about ±0x600 range.
    // Sending the full 0x000..0xFFF range can sit outside the advertised
    // calibration and some console paths appear to flatten/ignore the stick.
    if (v == 128) return 0x800;

    int32_t delta = (int32_t)v - 128;
    int32_t raw;
    if (delta > 0)
        raw = 0x800 + (delta * 0x600) / 127;
    else
        raw = 0x800 + (delta * 0x600) / 128;

    if (raw < 0x200) raw = 0x200;
    if (raw > 0xE00) raw = 0xE00;
    return (uint16_t)raw;
}

static uint8_t invert_axis8_centered(uint8_t v) {
    // 0 and 255 should swap, but keep the protocol's exact neutral value
    // neutral.  A raw 255-v inversion turns 128 into 127, which creates a tiny
    // permanent off-center Y value.
    return v == 128 ? 128 : (uint8_t)(255 - v);
}

static void pack_stick_12(uint8_t out[3], uint8_t x8, uint8_t y8) {
    // Input protocol uses 0 = up/left and 255 = down/right.  The console raw
    // stick format has Y in the opposite direction, so invert Y once here for
    // both sticks.
    uint16_t x = axis8_to_12(x8);
    uint16_t y = axis8_to_12(invert_axis8_centered(y8));
    out[0] = x & 0xFF;
    out[1] = ((x >> 8) & 0x0F) | ((y & 0x0F) << 4);
    out[2] = (y >> 4) & 0xFF;
}

static bool input_is_neutral(const HIDReport& r) {
    return r.buttons == 0 && r.hat == HAT_NEUTRAL &&
           r.lx == 128 && r.ly == 128 && r.rx == 128 && r.ry == 128;
}

static bool motion_is_neutral(const MotionReport& m) {
    return std::abs((int)m.ax) < 64 && std::abs((int)m.ay) < 64 && std::abs((int)m.az) < 64 &&
           std::abs((int)m.gx) < 64 && std::abs((int)m.gy) < 64 && std::abs((int)m.gz) < 64;
}

static bool extended_is_neutral(const ExtendedHIDReport& r) {
    return input_is_neutral(r.input) && (!r.has_motion || motion_is_neutral(r.motion));
}

static void hat_to_pro_buttons(uint8_t hat, uint8_t buttons[3]) {
    bool up = false, down = false, left = false, right = false;
    switch (hat) {
        case HAT_N:  up = true; break;
        case HAT_NE: up = true; right = true; break;
        case HAT_E:  right = true; break;
        case HAT_SE: down = true; right = true; break;
        case HAT_S:  down = true; break;
        case HAT_SW: down = true; left = true; break;
        case HAT_W:  left = true; break;
        case HAT_NW: up = true; left = true; break;
        default: break;
    }
    if (down)  buttons[2] |= 0x01;
    if (up)    buttons[2] |= 0x02;
    if (right) buttons[2] |= 0x04;
    if (left)  buttons[2] |= 0x08;
}

static void apply_input_controls_to_pro21(const ExtendedHIDReport& src, ProInputReport21& out) {
    // Subcommand replies (report 0x21) contain the same button/stick snapshot
    // fields as standard input reports.  Keep them in sync with the currently
    // held web/UDP input; otherwise frequent console output/subcommand traffic
    // injects neutral frames between normal 0x30 reports, which makes held
    // buttons such as R/ZR flicker in-game.
    out.conn_info = PRO_BAT_CON;
    memset(out.buttons, 0, sizeof(out.buttons));
    out.buttons[1] = 0x80; // real neutral USB Pro state is 00 80 00
    out.vibrator = PRO_VIBRATOR_REPORT;

    const HIDReport& in = src.input;
    if (in.buttons & BTN_Y)       out.buttons[0] |= 0x01;
    if (in.buttons & BTN_X)       out.buttons[0] |= 0x02;
    if (in.buttons & BTN_B)       out.buttons[0] |= 0x04;
    if (in.buttons & BTN_A)       out.buttons[0] |= 0x08;
    if (in.buttons & BTN_R)       out.buttons[0] |= 0x40;
    if (in.buttons & BTN_ZR)      out.buttons[0] |= 0x80;

    if (in.buttons & BTN_MINUS)   out.buttons[1] |= 0x01;
    if (in.buttons & BTN_PLUS)    out.buttons[1] |= 0x02;
    if (in.buttons & BTN_RSTICK)  out.buttons[1] |= 0x04;
    if (in.buttons & BTN_LSTICK)  out.buttons[1] |= 0x08;
    if (in.buttons & BTN_HOME)    out.buttons[1] |= 0x10;
    if (in.buttons & BTN_CAPTURE) out.buttons[1] |= 0x20;

    hat_to_pro_buttons(in.hat, out.buttons);
    if (in.buttons & BTN_L)       out.buttons[2] |= 0x40;
    if (in.buttons & BTN_ZL)      out.buttons[2] |= 0x80;

    pack_stick_12(out.left_stick,  in.lx, in.ly);
    pack_stick_12(out.right_stick, in.rx, in.ry);
}

static void build_standard_report(const ExtendedHIDReport& src,
                                  const MotionReport motion_samples[3],
                                  bool has_motion,
                                  bool imu_enabled,
                                  uint8_t timer,
                                  ProInputReport30& out) {
    memset(&out, 0, sizeof(out));
    out.id = RID_INPUT_STANDARD;
    out.timer = timer;
    fill_neutral_controls(out);

    const HIDReport& in = src.input;
    if (in.buttons & BTN_Y)       out.buttons[0] |= 0x01;
    if (in.buttons & BTN_X)       out.buttons[0] |= 0x02;
    if (in.buttons & BTN_B)       out.buttons[0] |= 0x04;
    if (in.buttons & BTN_A)       out.buttons[0] |= 0x08;
    if (in.buttons & BTN_R)       out.buttons[0] |= 0x40;
    if (in.buttons & BTN_ZR)      out.buttons[0] |= 0x80;

    if (in.buttons & BTN_MINUS)   out.buttons[1] |= 0x01;
    if (in.buttons & BTN_PLUS)    out.buttons[1] |= 0x02;
    if (in.buttons & BTN_RSTICK)  out.buttons[1] |= 0x04;
    if (in.buttons & BTN_LSTICK)  out.buttons[1] |= 0x08;
    if (in.buttons & BTN_HOME)    out.buttons[1] |= 0x10;
    if (in.buttons & BTN_CAPTURE) out.buttons[1] |= 0x20;

    hat_to_pro_buttons(in.hat, out.buttons);
    if (in.buttons & BTN_L)       out.buttons[2] |= 0x40;
    if (in.buttons & BTN_ZL)      out.buttons[2] |= 0x80;

    pack_stick_12(out.left_stick,  in.lx, in.ly);
    pack_stick_12(out.right_stick, in.rx, in.ry);

    MotionReport imu[3]{};
    const bool has_imu = imu_enabled && has_motion && motion_samples;
    if (has_imu) {
        imu[0] = motion_samples[0];
        imu[1] = motion_samples[1];
        imu[2] = motion_samples[2];
    }

    if (g_verbose && (!input_is_neutral(in) || has_imu)) {
        static uint64_t last_log_us = 0;
        uint64_t t = now_us();
        if (t - last_log_us > 250000) {
            last_log_us = t;
            std::printf("[input] lx=%3u ly=%3u rx=%3u ry=%3u | L=%02X %02X %02X R=%02X %02X %02X | motion=%s samples=%u ax=%6d ay=%6d az=%6d gx=%6d gy=%6d gz=%6d\n",
                        in.lx, in.ly, in.rx, in.ry,
                        out.left_stick[0], out.left_stick[1], out.left_stick[2],
                        out.right_stick[0], out.right_stick[1], out.right_stick[2],
                        has_imu ? "yes" : "no",
                        (unsigned)(has_imu ? 3 : 0),
                        (int)imu[2].ax, (int)imu[2].ay, (int)imu[2].az,
                        (int)imu[2].gx, (int)imu[2].gy, (int)imu[2].gz);
        }
    }


auto store_imu_sample = [](ProInputReport30& dst, int idx, const MotionReport& m) {
    if (idx == 0) {
        dst.accel_y_0 = m.ax;
        dst.accel_x_0 = m.ay;
        dst.accel_z_0 = m.az;

        dst.gyro_y_0  = m.gx;
        dst.gyro_x_0  = m.gy;
        dst.gyro_z_0  = m.gz;
    } else if (idx == 1) {
        dst.accel_y_1 = m.ax;
        dst.accel_x_1 = m.ay;
        dst.accel_z_1 = m.az;

        dst.gyro_y_1  = m.gx;
        dst.gyro_x_1  = m.gy;
        dst.gyro_z_1  = m.gz;
    } else {
        dst.accel_y_2 = m.ax;
        dst.accel_x_2 = m.ay;
        dst.accel_z_2 = m.az;

        dst.gyro_y_2  = m.gx;
        dst.gyro_x_2  = m.gy;
        dst.gyro_z_2  = m.gz;
    }
};

store_imu_sample(out, 0, imu[0]);
store_imu_sample(out, 1, imu[1]);
store_imu_sample(out, 2, imu[2]);
}

static int handle_subcommand(ControllerRuntime& rt, uint8_t subcmd, const uint8_t* cmd_data, size_t cmd_len, ProInputReport21* reply) {
    memset(reply->reply_data, 0, sizeof(reply->reply_data));
    reply->ack = 0x80;
    reply->subcmd_id = subcmd;

    switch (subcmd) {
    case CMD_BT_MANUAL_PAIRING: {
        // Public/synthetic pairing pages.  Keep the shape but do not embed any
        // private pairing material.
        reply->ack = 0x81;
        if (cmd_len > 0 && cmd_data[0] == 0x02) {
            memset(reply->reply_data, 0x00, 16);
            return 16;
        }
        if (cmd_len > 0 && cmd_data[0] == 0x03) {
            memset(reply->reply_data, 0x00, 16);
            return 16;
        }
        return 0;
    }

    case CMD_TRIGGER_BUTTONS:
        reply->ack = 0x83;
        reply->reply_data[0] = 0x00;
        return 1;

    case CMD_SET_SHIP_MODE:
        reply->ack = 0x80;
        return 0;

    case CMD_SET_NFC_IR_CONFIG:
        reply->ack = 0xA0;
        reply->reply_data[0] = 0x01;
        return 1;

    case CMD_SET_IMU_SENS:
        reply->ack = 0x80;
        return 0;

    case CMD_GET_DEVICE_INFO: {
        uint8_t info[36];
        build_get_device_info_response(info, rt.ctrl);
        reply->ack = 0x82;
        memcpy(reply->reply_data, info, 36);
        return 36;
    }

    case CMD_SET_DATA_FORMAT:
        rt.full_report_enabled = true;
        reply->ack = 0x80;
        return 0;

    case CMD_SPI_FLASH_READ: {
        if (cmd_len < 5) {
            reply->ack = 0x00;
            return 0;
        }
        uint32_t addr = ((uint32_t)cmd_data[0]) |
                        ((uint32_t)cmd_data[1] << 8) |
                        ((uint32_t)cmd_data[2] << 16) |
                        ((uint32_t)cmd_data[3] << 24);
        uint8_t size = cmd_data[4];
        if (size > 44) size = 44;

        reply->ack = 0x90;
        reply->reply_data[0] = cmd_data[0];
        reply->reply_data[1] = cmd_data[1];
        reply->reply_data[2] = cmd_data[2];
        reply->reply_data[3] = cmd_data[3];
        reply->reply_data[4] = size;

        uint8_t* flash = g_spi_flash[rt.ctrl];
        if (addr < SPI_FLASH_SIZE) {
            size_t to_copy = std::min((size_t)size, (size_t)(SPI_FLASH_SIZE - addr));
            memcpy(reply->reply_data + 5, flash + addr, to_copy);
            if (to_copy < size) memset(reply->reply_data + 5 + to_copy, 0xFF, size - to_copy);
        } else {
            memset(reply->reply_data + 5, 0xFF, size);
        }
        if (g_verbose) {
            std::printf("[pro%d] SPI read addr=0x%04X size=%u", rt.ctrl + 1, addr, size);
            if (addr == 0x6020 || addr == 0x8026 || addr == 0x8028 || addr == 0x6080 || addr == 0x6086 || addr == 0x6098) {
                std::printf(" data=");
                for (uint8_t i = 0; i < size && i < 32 && addr + i < SPI_FLASH_SIZE; ++i)
                    std::printf("%02X%s", flash[addr + i], (i + 1 < size && i + 1 < 32 && addr + i + 1 < SPI_FLASH_SIZE) ? " " : "");
            }
            std::printf("\n");
        }
        return 5 + size;
    }

    case CMD_SET_PLAYER_LIGHTS:
        reply->ack = 0x80;
        return 0;

    case 0x33:
        reply->ack = 0x80;
        return 0;

    case CMD_ENABLE_IMU:
        rt.imu_enabled = (cmd_len == 0) || cmd_data[0] != 0;
        reply->ack = 0x80;
        return 0;

    case CMD_ENABLE_VIBRATION:
        rt.vibration_enabled = (cmd_len == 0) || cmd_data[0] != 0;
        reply->ack = 0x80;
        return 0;

    default:
        reply->ack = 0x80;
        return 0;
    }
}

static bool rumble_half_is_all_zero(const uint8_t* f) {
    return f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0;
}

static bool rumble_half_is_neutral_carrier(const uint8_t* f) {
    return f[0] == 0x00 && f[1] == 0x01 && f[2] == 0x40 && f[3] == 0x40;
}

struct DecodedPrecisionRumbleHalf {
    uint8_t low = 0;   // low-frequency/weak motor approximation
    uint8_t high = 0;  // high-frequency/strong motor approximation
};

static uint8_t rumble_scale_capture_delta(int v) {
    v = (v * RUMBLE_GAIN_PERCENT) / 100;
    if (v > 0 && v < (int)RUMBLE_MIN_NONZERO) v = RUMBLE_MIN_NONZERO;
    return (uint8_t)std::clamp(v, 0, 255);
}

static DecodedPrecisionRumbleHalf rumble_decode_half_precision_to_dual(const uint8_t* f) {
    DecodedPrecisionRumbleHalf out{};
    if (rumble_half_is_all_zero(f) || rumble_half_is_neutral_carrier(f))
        return out;

    // Precision rumble is not a simple dual-rumble packet: each 4-byte half
    // carries one actuator with low/high frequency components.  For SDL/Web
    // clients, preserve that split by decoding each half into weak+strong and
    // later combining left/right actuators with max().
    //
    // This approximation is capture-derived and intentionally conservative:
    // neutral carrier 00 01 40 40 maps to 0, but packets such as
    // 26 09 81 5D / B3 2F AF 52 / BB 9D 2D 40 produce meaningful low/high
    // values instead of being crushed into one generic strength.
    int high_delta = std::abs((int)(f[1] & 0x7F) - 0x01);
    int low_delta  = std::abs((int)(f[3] & 0x7F) - 0x40);

    // The first and third bytes mainly move with frequency/envelope.  They are
    // useful to keep tiny precision pulses alive after conversion to classic rumble.
    int high_env = std::abs((int)f[0] - 0x00) / 3;
    int low_env  = std::abs((int)(f[2] & 0x7F) - 0x40) / 2;

    out.high = rumble_scale_capture_delta(high_delta * 3 + high_env);
    out.low  = rumble_scale_capture_delta(low_delta  * 3 + low_env);
    return out;
}

static uint8_t rumble_decode_half_to_u8(const uint8_t* f) {
    static const uint8_t neutral[4] = {0x00, 0x01, 0x40, 0x40};

    // Important: a 4-byte all-zero half-frame is an OFF motor half, not a
    // full-power rumble.  The old max-difference decoder turned 00 00 00 00
    // into 255 because it is far away from 00 01 40 40.  That made many
    // asymmetric/tiny Precision rumble packets become fake huge pulses.
    if (rumble_half_is_all_zero(f) || rumble_half_is_neutral_carrier(f))
        return 0;

    int max_diff = 0;
    int sum_diff = 0;
    for (int i = 0; i < 4; ++i) {
        int d = std::abs((int)f[i] - (int)neutral[i]);
        max_diff = std::max(max_diff, d);
        sum_diff += d;
    }

    // This is still a compact precision-rumble -> classic dual-rumble approximation,
    // but it preserves low magnitudes instead of clamping everything to >=80.
    // Low console pulses therefore get forwarded as low 1..79 values.
    int strength = max_diff * 2 + sum_diff / 4;
    strength = (strength * RUMBLE_GAIN_PERCENT) / 100;
    if (strength > 0 && strength < (int)RUMBLE_MIN_NONZERO)
        strength = RUMBLE_MIN_NONZERO;
    return (uint8_t)std::clamp(strength, 0, 255);
}

static void publish_rumble_event(int client_idx, int sub_idx, const uint8_t* packet, ssize_t len, bool publish_neutral) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4 || len < 10)
        return;

    const uint8_t* rb = packet + 2;
    DecodedPrecisionRumbleHalf left  = rumble_decode_half_precision_to_dual(rb);
    DecodedPrecisionRumbleHalf right = rumble_decode_half_precision_to_dual(rb + 4);

    // Classic clients have only weak/strong motors, not left/right precision
    // actuators. Combine both console Actuators by frequency band.
    uint8_t low  = std::max(left.low,  right.low);
    uint8_t high = std::max(left.high, right.high);

    // Fallback for any odd non-neutral packet our precision split does not understand.
    if (low == 0 && high == 0 &&
        !(rumble_half_is_all_zero(rb) || rumble_half_is_neutral_carrier(rb)) &&
        !(rumble_half_is_all_zero(rb + 4) || rumble_half_is_neutral_carrier(rb + 4))) {
        low = rumble_decode_half_to_u8(rb);
        high = rumble_decode_half_to_u8(rb + 4);
    }

    bool neutral = (low == 0 && high == 0);

    if (neutral && !publish_neutral)
        return;

    std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
    if (neutral && !g_clients[client_idx].rumble_active[sub_idx]) {
        // The console sends the neutral carrier constantly. Forwarding every
        // neutral frame creates haptic spam.
        return;
    }

    RumblePacket& ev = g_clients[client_idx].rumble[sub_idx];
    ev.magic = RUMBLE_MAGIC;
    ev.subpad = (uint8_t)sub_idx;
    ev.low_freq = neutral ? 0 : low;
    ev.high_freq = neutral ? 0 : high;
    // The compatibility testing sends rumble at report cadence.  Keep pulses short so
    // small precision packets do not smear into a long full-power buzz on classic clients.
    ev.duration_10ms = neutral ? 0 : 3;

    PrecisionRumblePacket& precision_ev = g_clients[client_idx].precision_rumble[sub_idx];
    precision_ev.magic = PRECISION_RUMBLE_MAGIC;
    precision_ev.subpad = (uint8_t)sub_idx;
    precision_ev.low_freq = ev.low_freq;
    precision_ev.high_freq = ev.high_freq;
    precision_ev.duration_10ms = ev.duration_10ms;
    memcpy(precision_ev.precision, rb, sizeof(precision_ev.precision));

    g_clients[client_idx].rumble_active[sub_idx] = !neutral;
    g_clients[client_idx].rumble_seq[sub_idx]++;

    if (g_verbose) {
        std::printf("[rumble] client=%d pad=%d low=%u high=%u duration=%u neutral=%s raw=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                    client_idx + 1, sub_idx + 1, ev.low_freq, ev.high_freq,
                    ev.duration_10ms, neutral ? "yes" : "no",
                    rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7]);
    }
}

static constexpr const char* GADGET_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl";
static constexpr const char* CONFIG_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1";

// Vendor USB gamepad descriptor, same 64-byte input/output report
// descriptor previously written by setup_gadget.sh.
static const uint8_t PRO_CONTROLLER_REPORT_DESC[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0A, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0A, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0B, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0B, 0x01, 0x00, 0x01, 0x00, 0xA1, 0x00, 0x0B, 0x30, 0x00,
    0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00, 0x0B, 0x32, 0x00, 0x01, 0x00, 0x0B, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xC0, 0x0B,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19, 0x0F, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x34, 0x81, 0x03, 0x06, 0x00, 0xFF, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, 0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0xC0
};

static const uint8_t LEGACY_REPORT_DESC[] = {
    0x05,0x01,0x09,0x05,0xA1,0x01,0x15,0x00,0x25,0x01,0x35,0x00,0x45,0x01,0x75,0x01,
    0x95,0x0D,0x05,0x09,0x19,0x01,0x29,0x0D,0x81,0x02,0x95,0x03,0x81,0x01,0x05,0x01,
    0x25,0x07,0x46,0x3B,0x01,0x75,0x04,0x95,0x01,0x65,0x14,0x09,0x39,0x81,0x42,
    0x65,0x00,0x95,0x01,0x81,0x01,0x26,0xFF,0x00,0x46,0xFF,0x00,0x09,0x30,0x09,
    0x31,0x09,0x32,0x09,0x35,0x75,0x08,0x95,0x04,0x81,0x02,0x06,0x00,0xFF,0x09,
    0x20,0x75,0x08,0x95,0x01,0x81,0x02,0xC0
};

static bool hidg_nodes_ready() {
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/hidg%d", i);
        if (access(path, R_OK | W_OK) != 0)
            return false;
    }
    return true;
}

static bool dir_exists(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_exists(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0;
}

static bool mkdir_if_needed(const char* path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return true;
    std::fprintf(stderr, "[gadget] mkdir %s failed: %s\n", path, std::strerror(errno));
    return false;
}

static bool write_all_fd(int fd, const void* data, size_t len, const char* path) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "[gadget] write %s failed: %s\n", path, std::strerror(errno));
            return false;
        }
        if (w == 0) {
            std::fprintf(stderr, "[gadget] write %s wrote 0 bytes\n", path);
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    return true;
}

static bool write_bytes_file(const char* path, const void* data, size_t len) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        std::fprintf(stderr, "[gadget] open %s failed: %s\n", path, std::strerror(errno));
        return false;
    }
    bool ok = write_all_fd(fd, data, len, path);
    close(fd);
    return ok;
}

static bool write_text_file(const char* path, const char* text) {
    return write_bytes_file(path, text, std::strlen(text));
}

static void remove_link_if_exists(const char* path) {
    if (unlink(path) != 0) {
        if (errno != ENOENT && errno != EISDIR && errno != EPERM) {
            if (g_verbose) {
                std::fprintf(stderr, "[gadget] unlink %s failed: %s\n",
                             path, std::strerror(errno));
            }
        }
    }
}

static void rmdir_if_exists(const char* path) {
    if (rmdir(path) != 0 && errno != ENOENT) {
        if (g_verbose)
            std::fprintf(stderr, "[gadget] rmdir %s failed: %s\n", path, std::strerror(errno));
    }
}

static std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty() || a.back() == '/') return a + b;
    return a + "/" + b;
}

static std::string first_udc_name() {
    DIR* d = opendir("/sys/class/udc");
    if (!d) return "";
    std::string out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        out = e->d_name;
        break;
    }
    closedir(d);
    return out;
}

static bool create_hid_function(int id) {
    char func[256];
    std::snprintf(func, sizeof(func), "%s/functions/hid.usb%d", GADGET_DIR, id);
    if (!mkdir_if_needed(func)) return false;

    char path[320];
    std::snprintf(path, sizeof(path), "%s/protocol", func);
    if (!write_text_file(path, "0")) return false;

    std::snprintf(path, sizeof(path), "%s/subclass", func);
    if (!write_text_file(path, "0")) return false;

    std::snprintf(path, sizeof(path), "%s/report_length", func);
    if (!write_text_file(path, g_legacy_mode ? "8" : "64")) return false;

    std::snprintf(path, sizeof(path), "%s/report_desc", func);
    if (g_legacy_mode) {
        if (!write_bytes_file(path, LEGACY_REPORT_DESC, sizeof(LEGACY_REPORT_DESC))) return false;
    } else {
        if (!write_bytes_file(path, PRO_CONTROLLER_REPORT_DESC, sizeof(PRO_CONTROLLER_REPORT_DESC))) return false;
    }

    char link_path[320];
    std::snprintf(link_path, sizeof(link_path), "%s/hid.usb%d", CONFIG_DIR, id);
    unlink(link_path);
    if (symlink(func, link_path) != 0) {
        std::fprintf(stderr, "[gadget] symlink %s -> %s failed: %s\n",
                     link_path, func, std::strerror(errno));
        return false;
    }

    return true;
}

static void teardown_gadget() {
    if (!path_exists(GADGET_DIR)) return;

    g_switch2_usb_host_connected.store(false, std::memory_order_relaxed);
    std::puts("[gadget] Closing USB gadget...");

    // Unbind first.  This disconnects the virtual controllers from the console.
    std::string udc_path = join_path(GADGET_DIR, "UDC");
    write_text_file(udc_path.c_str(), "");

    // Remove config links before removing functions, mirroring setup_gadget.sh.
    for (int i = 0; i < 4; ++i) {
        char link_path[320];
        std::snprintf(link_path, sizeof(link_path), "%s/hid.usb%d", CONFIG_DIR, i);
        remove_link_if_exists(link_path);
    }

    // Configfs object directories are removed with rmdir; their pseudo-attribute
    // files must not be unlinked manually.
    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1/strings/0x409");
    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1");

    for (int i = 0; i < 4; ++i) {
        char func[256];
        std::snprintf(func, sizeof(func), "%s/functions/hid.usb%d", GADGET_DIR, i);
        rmdir_if_exists(func);
    }

    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/strings/0x409");
    rmdir_if_exists(GADGET_DIR);

    std::puts("[gadget] USB gadget closed");
}

static int run_shell_best_effort(const char* cmd) {
    int rc = std::system(cmd);
    return rc;
}

static std::string trim_copy(const std::string& in) {
    size_t a = 0;
    while (a < in.size() && std::isspace((unsigned char)in[a])) ++a;
    size_t b = in.size();
    while (b > a && std::isspace((unsigned char)in[b - 1])) --b;
    return in.substr(a, b - a);
}

static std::string uppercase_hex_no_space(std::string v) {
    std::string out;
    out.reserve(v.size());
    for (char ch : v) {
        if (!std::isspace((unsigned char)ch))
            out.push_back((char)std::toupper((unsigned char)ch));
    }
    return out;
}

static bool is_hex_string(const std::string& v) {
    if (v.empty() || (v.size() % 2) != 0) return false;
    for (char ch : v) {
        if (!std::isxdigit((unsigned char)ch)) return false;
    }
    return true;
}

static bool is_valid_mac(const std::string& mac) {
    if (mac.size() != 17) return false;
    for (size_t i = 0; i < mac.size(); ++i) {
        if ((i + 1) % 3 == 0) {
            if (mac[i] != ':') return false;
        } else if (!std::isxdigit((unsigned char)mac[i])) {
            return false;
        }
    }
    return true;
}

static std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
}

static bool ensure_parent_dir_for_file(const std::string& file_path) {
    size_t slash = file_path.find_last_of('/');
    if (slash == std::string::npos) return true;
    std::string dir = file_path.substr(0, slash);
    if (dir.empty()) return true;

    std::string cmd = "mkdir -p " + shell_quote(dir);
    return std::system(cmd.c_str()) == 0;
}

static bool save_switch2_wakeup_config(const std::string& mac, const std::string& adv) {
    if (!ensure_parent_dir_for_file(g_switch2_wake_config_path)) {
        std::fprintf(stderr, "[wake] Failed to create config directory for %s\n", g_switch2_wake_config_path.c_str());
        return false;
    }

    std::ofstream f(g_switch2_wake_config_path, std::ios::out | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "[wake] Failed to write %s: %s\n", g_switch2_wake_config_path.c_str(), std::strerror(errno));
        return false;
    }
    f << "# NS-PC-Control Switch 2 wake identity.\n";
    f << "# Generated by: ns-backend -wake\n";
    f << "# Keep this private-ish: it contains the paired Joy-Con 2 wake identity.\n";
    f << "mac=" << mac << "\n";
    f << "adv=" << adv << "\n";
    f.close();

    chmod(g_switch2_wake_config_path.c_str(), 0600);
    return true;
}

static bool load_switch2_wakeup_config(bool verbose_missing) {
    g_switch2_wake_config_loaded = false;
    g_switch2_wake_mac.clear();
    g_switch2_wake_adv.clear();

    std::ifstream f(g_switch2_wake_config_path);
    if (!f) {
        if (verbose_missing && g_verbose)
            std::printf("[wake] No Switch 2 wake config found at %s; wake disabled\n", g_switch2_wake_config_path.c_str());
        return false;
    }

    std::string line;
    while (std::getline(f, line)) {
        line = trim_copy(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_copy(line.substr(0, eq));
        std::string val = trim_copy(line.substr(eq + 1));
        if (key == "mac") g_switch2_wake_mac = val;
        else if (key == "adv") g_switch2_wake_adv = uppercase_hex_no_space(val);
    }

    if (!is_valid_mac(g_switch2_wake_mac)) {
        std::fprintf(stderr, "[wake] Ignoring invalid wake config MAC in %s\n", g_switch2_wake_config_path.c_str());
        return false;
    }
    if (!is_hex_string(g_switch2_wake_adv) || g_switch2_wake_adv.size() / 2 > 31) {
        std::fprintf(stderr, "[wake] Ignoring invalid wake config ADV in %s\n", g_switch2_wake_config_path.c_str());
        return false;
    }

    g_switch2_wake_config_loaded = true;
    if (g_verbose) {
        std::printf("[wake] Loaded Switch 2 wake config from %s (mac=%s adv_bytes=%zu)\n",
                    g_switch2_wake_config_path.c_str(), g_switch2_wake_mac.c_str(), g_switch2_wake_adv.size() / 2);
    }
    return true;
}

static std::string adv_hex_to_hcitool_args(const std::string& adv) {
    std::ostringstream oss;
    const size_t bytes = adv.size() / 2;
    oss << std::uppercase << std::hex;
    char lenbuf[8];
    std::snprintf(lenbuf, sizeof(lenbuf), "%02zX", bytes);
    oss << lenbuf;
    for (size_t i = 0; i < bytes; ++i)
        oss << ' ' << adv.substr(i * 2, 2);
    for (size_t i = bytes; i < 31; ++i)
        oss << " 00";
    return oss.str();
}

static bool command_exists(const char* name) {
    std::string cmd = "command -v ";
    cmd += name;
    cmd += " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

static void switch2_wake_adv_worker(int burst_ms) {
    std::string mac = g_switch2_wake_mac;
    std::string adv = g_switch2_wake_adv;
    if (!g_switch2_wake_config_loaded || !is_valid_mac(mac) || !is_hex_string(adv) || adv.size() / 2 > 31) {
        if (g_verbose)
            std::puts("[wake] No valid Switch 2 wake config loaded; wake advert skipped");
        g_switch2_wake_adv_running.store(false, std::memory_order_relaxed);
        return;
    }

    const int seconds = std::max(1, (burst_ms + 999) / 1000);
    const std::string adv_args = adv_hex_to_hcitool_args(adv);

    // Same raw-HCI shape as wake-v4.sh: stop bluetoothd, set the controller's
    // public address to the captured Joy-Con 2 MAC, then advertise the captured
    // raw payload as non-connectable public-address BLE advertising.
    std::ostringstream cmd;
    cmd << "bash -c " << shell_quote(
        "set +e; "
        "HCI_DEV=${HCI_DEV:-hci0}; "
        "systemctl stop bluetooth >/dev/null 2>&1 || true; "
        "rfkill unblock bluetooth >/dev/null 2>&1 || true; "
        "DEV=$(btmgmt info 2>/dev/null | awk '/^hci[0-9]+:/{gsub(\":\",\"\",$1); print $1; exit}'); "
        "[ -n \"$DEV\" ] && HCI_DEV=\"$DEV\"; "
        "btmgmt -i \"$HCI_DEV\" power off >/dev/null 2>&1 || true; "
        "btmgmt -i \"$HCI_DEV\" privacy off >/dev/null 2>&1 || true; "
        "btmgmt -i \"$HCI_DEV\" bredr off >/dev/null 2>&1 || true; "
        "btmgmt -i \"$HCI_DEV\" le on >/dev/null 2>&1 || true; "
        "btmgmt -i \"$HCI_DEV\" public-addr " + shell_quote(mac) + " >/dev/null 2>&1 || true; "
        "sleep 0.4; "
        "DEV=$(btmgmt info 2>/dev/null | awk '/^hci[0-9]+:/{gsub(\":\",\"\",$1); print $1; exit}'); "
        "[ -n \"$DEV\" ] && HCI_DEV=\"$DEV\"; "
        "btmgmt -i \"$HCI_DEV\" power on >/dev/null 2>&1 || true; "
        "hcitool -i \"$HCI_DEV\" cmd 0x08 0x000A 00 >/dev/null 2>&1 || true; "
        "hcitool -i \"$HCI_DEV\" cmd 0x08 0x0006 20 00 40 00 03 00 00 00 00 00 00 00 00 07 00 >/dev/null 2>&1 || true; "
        "hcitool -i \"$HCI_DEV\" cmd 0x08 0x0008 " + adv_args + " >/dev/null 2>&1 || true; "
        "hcitool -i \"$HCI_DEV\" cmd 0x08 0x0009 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 >/dev/null 2>&1 || true; "
        "hcitool -i \"$HCI_DEV\" cmd 0x08 0x000A 01 >/dev/null 2>&1 || true; "
        "sleep " + std::to_string(seconds) + "; "
        "hcitool -i \"$HCI_DEV\" cmd 0x08 0x000A 00 >/dev/null 2>&1 || true"
    );

    (void)run_shell_best_effort(cmd.str().c_str());
    g_switch2_wake_adv_running.store(false, std::memory_order_relaxed);
}

static bool extract_mac_from_line(const std::string& line, std::string& mac_out) {
    for (size_t i = 0; i + 17 <= line.size(); ++i) {
        std::string cand = line.substr(i, 17);
        if (is_valid_mac(cand)) {
            mac_out = cand;
            std::transform(mac_out.begin(), mac_out.end(), mac_out.begin(), [](unsigned char c){ return (char)std::toupper(c); });
            return true;
        }
    }
    return false;
}

static bool extract_data_payload_from_line(const std::string& line, std::string& data_out) {
    size_t pos = line.find("Data[");
    if (pos == std::string::npos) return false;
    pos = line.find(':', pos);
    if (pos == std::string::npos) return false;
    std::string hex = uppercase_hex_no_space(line.substr(pos + 1));
    if (!is_hex_string(hex)) return false;
    data_out = hex;
    return true;
}

static int run_switch2_wakeup_setup() {
    if (geteuid() != 0) {
        std::fprintf(stderr, "[wake] -wake needs root because it scans/controls Bluetooth. Run with sudo.\n");
        return 1;
    }

    if (!command_exists("btmon") || !command_exists("btmgmt") || !command_exists("hcitool")) {
        std::fprintf(stderr, "[wake] Missing BlueZ tools. Install with: sudo apt install bluez\n");
        return 1;
    }

    std::puts("[wake] Switch 2 Joy-Con 2 wake setup");
    std::printf("[wake] Output config: %s\n", g_switch2_wake_config_path.c_str());
    std::puts("[wake] Put the Joy-Con 2 close to the Pi, then press HOME on the Joy-Con 2.");
    std::puts("[wake] Waiting up to 45 seconds for Nintendo BLE advertising data...");

    (void)run_shell_best_effort("rfkill unblock bluetooth >/dev/null 2>&1 || true");
    (void)run_shell_best_effort("btmgmt power on >/dev/null 2>&1 || true");

    std::atomic<bool> scan_running{true};
    std::thread scan_thread([&]() {
        // Active scan, while btmon below captures the HCI advertising reports.
        (void)run_shell_best_effort("timeout 45 btmgmt find >/dev/null 2>&1 || timeout 45 hcitool lescan --duplicates >/dev/null 2>&1 || true");
        scan_running.store(false, std::memory_order_relaxed);
    });

    FILE* fp = popen("timeout 46 btmon -T 2>&1", "r");
    if (!fp) {
        scan_running.store(false, std::memory_order_relaxed);
        scan_thread.join();
        std::fprintf(stderr, "[wake] Failed to start btmon\n");
        return 1;
    }

    char buf[1024];
    std::string current_mac;
    bool current_nintendo = false;
    std::string found_mac;
    std::string found_data;

    while (fgets(buf, sizeof(buf), fp)) {
        std::string line(buf);
        std::string mac;
        if (line.find("Address:") != std::string::npos && extract_mac_from_line(line, mac)) {
            current_mac = mac;
            current_nintendo = (line.find("Nintendo") != std::string::npos);
            if (g_verbose) std::printf("[wake] saw address %s%s\n", current_mac.c_str(), current_nintendo ? " Nintendo" : "");
        }
        if (line.find("Company:") != std::string::npos && line.find("Nintendo") != std::string::npos)
            current_nintendo = true;

        std::string data;
        if (current_nintendo && !current_mac.empty() && extract_data_payload_from_line(line, data)) {
            // Rebuild the raw ADV payload as Flags + Manufacturer Specific Data.
            // Most Joy-Con 2 wake packets observed so far are:
            //   02 01 06 1B FF 53 05 <24-byte Nintendo payload>
            const size_t data_bytes = data.size() / 2;
            const size_t manufacturer_len = data_bytes + 3; // type FF + Nintendo company ID LE + payload
            if (manufacturer_len <= 0xFF && 3 + 2 + manufacturer_len <= 31) {
                char lenbuf[8];
                std::snprintf(lenbuf, sizeof(lenbuf), "%02zX", manufacturer_len);
                found_mac = current_mac;
                found_data = std::string("020106") + lenbuf + "FF5305" + data;
                break;
            }
        }
    }

    if (!found_mac.empty() && !found_data.empty()) {
        // Stop helper processes quickly instead of waiting for timeout(1).
        (void)run_shell_best_effort("pkill -INT -x btmon >/dev/null 2>&1 || true; pkill -TERM -x btmgmt >/dev/null 2>&1 || true; pkill -TERM -x hcitool >/dev/null 2>&1 || true");
    }

    pclose(fp);
    scan_running.store(false, std::memory_order_relaxed);
    scan_thread.join();

    if (found_mac.empty() || found_data.empty()) {
        std::fprintf(stderr, "[wake] Could not capture Joy-Con 2 Nintendo wake advert. Try again with the Joy-Con closer and press HOME, not SYNC.\n");
        return 1;
    }

    std::printf("[wake] Captured MAC: %s\n", found_mac.c_str());
    std::printf("[wake] Captured ADV: %s\n", found_data.c_str());

    if (!save_switch2_wakeup_config(found_mac, found_data))
        return 1;

    std::printf("[wake] Saved Switch 2 wake config to %s\n", g_switch2_wake_config_path.c_str());
    std::puts("[wake] Setup complete. Start the backend normally; wake triggers when a new client connects while the USB host is disconnected.");
    return 0;
}

static void maybe_send_switch2_wake_advert(const char* reason) {
    if (!g_switch2_wake_adv_enabled)
        return;
    if (!g_switch2_wake_config_loaded)
        return;

    // Do not wake-spam when the Switch/USB host is already connected/awake.
    // This mirrors the backend's existing disconnected-host detection.
    if (g_switch2_usb_host_connected.load(std::memory_order_relaxed)) {
        if (g_verbose) {
            std::printf("[wake] %s; Switch USB host already connected, skipping wake advert\n",
                        reason ? reason : "client connected");
        }
        return;
    }

    const uint64_t now = now_us();

    // Only send wake advertisements when a PC/web/mobile client is actually alive.
    if (!any_recent_client_active(now))
        return;

    const uint64_t last = g_switch2_last_wake_adv_us.load(std::memory_order_relaxed);
    if (last != 0 && elapsed_us_saturated(now, last) < SWITCH2_WAKE_ADV_COOLDOWN_US)
        return;

    bool expected = false;
    if (!g_switch2_wake_adv_running.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        return;

    g_switch2_last_wake_adv_us.store(now, std::memory_order_relaxed);

    if (g_verbose) {
        std::printf("[wake] %s; sending Switch 2 Joy-Con 2 BLE wake advert for %dms\n",
                    reason ? reason : "client connected", SWITCH2_WAKE_ADV_BURST_MS);
    }

    std::thread(switch2_wake_adv_worker, SWITCH2_WAKE_ADV_BURST_MS).detach();
}

static bool setup_gadget_builtin(bool force, const char* reason) {
    // Non-forced calls are still cheap/retry-safe for internal recovery paths,
    // but startup passes force=true so the gadget is always torn down and
    // recreated from a known-good state.
    if (!force && hidg_nodes_ready())
        return true;

    if (!force && g_gadget_setup_attempted.exchange(true))
        return hidg_nodes_ready();
    if (force)
        g_gadget_setup_attempted.store(true);

    if (geteuid() != 0) {
        std::fprintf(stderr,
            "[gadget] requested /dev/hidg* nodes are not ready and built-in setup needs root.\n"
            "[gadget] Run: sudo ./ns-backend ...\n");
        return false;
    }

    if (g_verbose)
        std::printf("[gadget] %s; creating built-in %d-interface %s gadget\n",
                    reason ? reason : "HID gadget not ready",
                    HID_PORT_COUNT,
                    g_legacy_mode ? "legacy 8-byte" : "64-byte motion");

    // Try to load and mount configfs.  Ignore failures here because both may
    // already be active on systems that previously used setup_gadget.sh.
    (void)run_shell_best_effort("modprobe libcomposite >/dev/null 2>&1 || true");
    (void)run_shell_best_effort("mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config >/dev/null 2>&1 || true");

    if (!dir_exists("/sys/kernel/config/usb_gadget")) {
        std::fprintf(stderr,
            "[gadget] /sys/kernel/config/usb_gadget is unavailable.\n"
            "[gadget] Check libcomposite/configfs and dtoverlay=dwc2.\n");
        return false;
    }

    // Always remove our previous gadget object before creating a new one when
    // force=true.  This protects normal startup from stale configfs state left
    // by crashes, kill -9, power loss, or older versions of setup_gadget.sh.
    if (path_exists(GADGET_DIR)) {
        if (force || !hidg_nodes_ready()) {
            teardown_gadget();
            std::this_thread::sleep_for(ms(300));
        } else {
            return true;
        }
    }

    if (!mkdir_if_needed(GADGET_DIR)) return false;

    std::string strings_dir = join_path(GADGET_DIR, "strings/0x409");
    std::string configs_dir = join_path(GADGET_DIR, "configs/c.1");
    std::string config_strings_dir = join_path(configs_dir, "strings/0x409");
    std::string functions_dir = join_path(GADGET_DIR, "functions");

    if (!mkdir_if_needed(join_path(GADGET_DIR, "strings").c_str())) return false;
    if (!mkdir_if_needed(strings_dir.c_str())) return false;
    if (!mkdir_if_needed(join_path(GADGET_DIR, "configs").c_str())) return false;
    if (!mkdir_if_needed(configs_dir.c_str())) return false;
    if (g_legacy_mode) {
        if (!mkdir_if_needed(join_path(configs_dir, "strings").c_str())) return false;
        if (!mkdir_if_needed(config_strings_dir.c_str())) return false;
    }
    if (!mkdir_if_needed(functions_dir.c_str())) return false;

    if (!write_text_file(join_path(GADGET_DIR, "bcdDevice").c_str(), g_legacy_mode ? "0x0200" : "0x0210")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bcdUSB").c_str(), "0x0200")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "idVendor").c_str(), g_legacy_mode ? "0x0F0D" : "0x057e")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "idProduct").c_str(), g_legacy_mode ? "0x0092" : "0x2009")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceClass").c_str(), g_legacy_mode ? "0xFF" : "0x00")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceSubClass").c_str(), g_legacy_mode ? "0xFF" : "0x00")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceProtocol").c_str(), g_legacy_mode ? "0xFF" : "0x00")) return false;

    // USB descriptor serial belongs here, not in the controller SPI area.
    if (!write_text_file(join_path(strings_dir, "serialnumber").c_str(), g_legacy_mode ? "000000000001" : g_usb_serial.c_str())) return false;
    if (!write_text_file(join_path(strings_dir, "manufacturer").c_str(), "NS Bridge")) return false;
    if (!write_text_file(join_path(strings_dir, "product").c_str(), g_legacy_mode ? "Legacy USB Gamepad" : "Motion USB Gamepad")) return false;

    if (!write_text_file(join_path(configs_dir, "MaxPower").c_str(), "500")) return false;
    if (!write_text_file(join_path(configs_dir, "bmAttributes").c_str(), g_legacy_mode ? "0x80" : "0xA0")) return false;
    if (g_legacy_mode) {
        if (!write_text_file(join_path(config_strings_dir, "configuration").c_str(), "USB 4-Player Hub Config")) return false;
    }

    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        if (!create_hid_function(i)) return false;
    }

    std::string udc = first_udc_name();
    if (udc.empty()) {
        std::fprintf(stderr,
            "[gadget] No UDC found. Check dtoverlay=dwc2 in /boot/config.txt.\n");
        return false;
    }

    if (!write_text_file(join_path(GADGET_DIR, "UDC").c_str(), udc.c_str())) return false;
    if (g_verbose)
        std::printf("[gadget] Bound to UDC: %s\n", udc.c_str());

    // /dev/hidg* can appear shortly after binding.
    for (int tries = 0; tries < 20; ++tries) {
        bool all_seen = true;
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            char path[32];
            std::snprintf(path, sizeof(path), "/dev/hidg%d", i);
            if (access(path, F_OK) != 0) all_seen = false;
            chmod(path, 0666);
        }
        if (all_seen && hidg_nodes_ready()) {
            std::printf("[gadget] Done. Exposed %d USB gamepad HID interface(s) (/dev/hidg0..%d)\n", HID_PORT_COUNT, HID_PORT_COUNT - 1);
            return true;
        }
        std::this_thread::sleep_for(ms(100));
    }

    std::fprintf(stderr, "[gadget] setup finished, but requested /dev/hidg* nodes are still not ready.\n");
    return false;
}

static bool run_gadget_setup_if_needed(bool force, const char* reason) {
    return setup_gadget_builtin(force, reason);
}

static void drain_hid_output_queue(int fd) {
    if (fd < 0) return;

    uint8_t discard[PRO_REPORT_SIZE];
    for (int i = 0; i < 32; ++i) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) break;
        ssize_t r = read(fd, discard, sizeof(discard));
        if (r <= 0) break;
    }
}

// ── Smart Multiplexer HID Writer Thread ───────────────────────────────────────
static void legacy_writer_thread(int hz) {
    const auto tick = us(1'000'000 / hz);
    int fds[HID_PORT_COUNT] = {-1, -1, -1, -1};
    std::string devs[HID_PORT_COUNT] = {"/dev/hidg0", "/dev/hidg1", "/dev/hidg2", "/dev/hidg3"};
    bool was_connected = false;

    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[HID_PORT_COUNT];

    while (g_running.load(std::memory_order_relaxed)) {
        bool all_open = true;
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            if (fds[i] < 0) {
                fds[i] = open(devs[i].c_str(), O_WRONLY | O_NONBLOCK);
                if (fds[i] < 0) all_open = false;
            }
        }

        if (!all_open) {
            g_switch2_usb_host_connected.store(false, std::memory_order_relaxed);
            for (int i = 0; i < HID_PORT_COUNT; ++i) {
                if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; }
            }
            run_gadget_setup_if_needed(false, "requested legacy /dev/hidg* nodes could not all be opened");
            for (int wait_i = 0; wait_i < 50 && g_running.load(std::memory_order_relaxed); ++wait_i) std::this_thread::sleep_for(ms(10));
            continue;
        }

        if (g_verbose || !was_connected)
            std::printf("%dx legacy /dev/hidg* opened\n", HID_PORT_COUNT);
        was_connected = true;
        g_switch2_usb_host_connected.store(true, std::memory_order_relaxed);

        auto next = Clock::now() + tick;
        HIDReport prev[HID_PORT_COUNT];
        for (int i = 0; i < HID_PORT_COUNT; ++i) prev[i].buttons = 0xFFFF;
        bool error_shown = false;
        bool timeout_printed[MAX_CLIENTS] = {};

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            uint64_t now_stamp = now_us();
            bool active_snap[MAX_CLIENTS] = {};
            bool uses_presence_snap[MAX_CLIENTS] = {};
            bool present_snap[MAX_CLIENTS][4] = {};
            uint64_t last_present_snap[MAX_CLIENTS][4] = {};
            ExtendedHIDReport report_snap[MAX_CLIENTS][4];
            for (int c = 0; c < MAX_CLIENTS; ++c)
                for (int s = 0; s < 4; ++s)
                    report_snap[c][s].reset();

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                std::lock_guard<std::mutex> lk(g_mtx[c]);
                repair_future_client_timestamp(g_clients[c], now_stamp);
                const uint64_t client_idle_us = elapsed_us_saturated(now_stamp, g_clients[c].last_rx_us);
                if (g_clients[c].active && g_clients[c].last_rx_us != 0 && client_idle_us > CLIENT_TIMEOUT_US && !server_macro_running(c,0) && !server_macro_running(c,1) && !server_macro_running(c,2) && !server_macro_running(c,3)) {
                    g_clients[c].active = false;
                    g_clients[c].report.reset();
                    clear_all_motion(g_clients[c]);
                    g_clients[c].uses_pad_presence = false;
                    for (int s = 0; s < 4; ++s) {
                        g_clients[c].pad_present[s] = false;
                        g_clients[c].pad_last_present_us[s] = 0;
                    }
                    if (g_verbose && !timeout_printed[c]) {
                        std::printf("PC %d timed out after %.1fs without UDP input and was disconnected.\n",
                                    c + 1, (double)client_idle_us / 1000000.0);
                        timeout_printed[c] = true;
                    }
                } else if (g_clients[c].active) {
                    timeout_printed[c] = false;
                }

                active_snap[c] = g_clients[c].active;
                uses_presence_snap[c] = g_clients[c].uses_pad_presence;
                const bool input_stream_stale =
                    g_clients[c].active &&
                    g_clients[c].last_rx_us != 0 &&
                    client_idle_us > CLIENT_STALE_NEUTRAL_US;

                for (int s = 0; s < 4; ++s) {
                    present_snap[c][s] = g_clients[c].pad_present[s];
                    last_present_snap[c][s] = g_clients[c].pad_last_present_us[s];
                }

                if (!input_stream_stale) {
                    report_snap[c][0] = g_clients[c].report.p1;
                    report_snap[c][1] = g_clients[c].report.p2;
                    report_snap[c][2] = g_clients[c].report.p3;
                    report_snap[c][3] = g_clients[c].report.p4;
                }
            }

            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (hw_slots[h].client_idx == -1) continue;

                int cidx = hw_slots[h].client_idx;
                int sidx = hw_slots[h].sub_idx;
                bool absent_too_long = false;
                if (uses_presence_snap[cidx] && !present_snap[cidx][sidx]) {
                    uint64_t last_seen = last_present_snap[cidx][sidx];
                    absent_too_long = (last_seen == 0) ||
                                      (now_stamp - last_seen >= WEB_PAD_ABSENT_RELEASE_US);
                }

                if ((!active_snap[cidx] || absent_too_long) && !server_macro_running(cidx, sidx)) {
                    hw_slots[h].client_idx = -1;
                    hw_slots[h].sub_idx = -1;
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!active_snap[c]) continue;
                for (int s = 0; s < 4; ++s) {
                    bool mapped = false;
                    for (int h = 0; h < HID_PORT_COUNT; ++h) {
                        if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                            mapped = true;
                            break;
                        }
                    }
                    if (mapped) continue;

                    bool macro_active_for_pad = server_macro_running(c, s);
                    if (uses_presence_snap[c]) {
                        if (!present_snap[c][s] && !macro_active_for_pad) continue;
                    } else if (input_is_neutral(report_snap[c][s].input) && !macro_active_for_pad) {
                        continue;
                    }

                    int chosen = -1;
                    if (s >= 0 && s < HID_PORT_COUNT && hw_slots[s].client_idx == -1) {
                        chosen = s;
                    } else {
                        for (int h = 0; h < HID_PORT_COUNT; ++h) {
                            if (hw_slots[h].client_idx == -1) {
                                chosen = h;
                                break;
                            }
                        }
                    }

                    if (chosen != -1) {
                        hw_slots[chosen].client_idx = c;
                        hw_slots[chosen].sub_idx = s;
                        if (g_verbose)
                            std::printf("Map -> PC %d (Pad %d) took console legacy Port %d\n", c + 1, s + 1, chosen + 1);
                    }
                }
            }

            HIDReport out_reports[HID_PORT_COUNT];
            for (int h = 0; h < HID_PORT_COUNT; ++h) out_reports[h].reset();
            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (hw_slots[h].client_idx == -1) continue;
                int cidx = hw_slots[h].client_idx;
                int sidx = hw_slots[h].sub_idx;
                out_reports[h] = report_snap[cidx][sidx].input;
                out_reports[h].vendor = 0;
                server_macro_apply(cidx, sidx, out_reports[h]);
                out_reports[h].vendor = 0;
            }

            bool ok = true;
            static_assert(sizeof(HIDReport) == 8, "HIDReport size mismatch with legacy HID gadget descriptor");
            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (out_reports[h] == prev[h]) continue;
                ssize_t w = write(fds[h], &out_reports[h], sizeof(HIDReport));
                if (w < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
                } else if (w == (ssize_t)sizeof(HIDReport)) {
                    prev[h] = out_reports[h];
                    ++g_hid_writes;
                } else if (w > 0) {
                    ok = false;
                }
            }

            if (!ok) {
                if (!error_shown) { std::puts("Host disconnected - waiting for reconnect..."); error_shown = true; }
                g_switch2_usb_host_connected.store(false, std::memory_order_relaxed);
                for (int i = 0; i < HID_PORT_COUNT; ++i) { close(fds[i]); fds[i] = -1; }
                for (int wait_i = 0; wait_i < 100 && g_running.load(std::memory_order_relaxed); ++wait_i) std::this_thread::sleep_for(ms(10));
                break;
            }
        }
    }

    HIDReport neutral{};
    neutral.reset();
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        if (fds[i] >= 0) {
            ssize_t unused = write(fds[i], &neutral, sizeof(HIDReport));
            (void)unused;
            close(fds[i]);
        }
    }
}

static void writer_thread(int hz) {
    if (g_legacy_mode) {
        legacy_writer_thread(hz);
        return;
    }

    for (int i = 0; i < HID_PORT_COUNT; ++i) init_spi_flash(i);

    const auto tick = us(1'000'000 / hz);
    int fds[4] = {-1, -1, -1, -1};
    std::string devs[4] = {"/dev/hidg0", "/dev/hidg1", "/dev/hidg2", "/dev/hidg3"};
    bool was_connected = false;

    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[4];
    ControllerRuntime rt[4];
    for (int i = 0; i < HID_PORT_COUNT; ++i) rt[i].ctrl = i;

    while (g_running.load(std::memory_order_relaxed)) {
        bool all_open = true;
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            if (fds[i] < 0) {
                fds[i] = open(devs[i].c_str(), O_RDWR | O_NONBLOCK);
                if (fds[i] >= 0) {
                    rt[i].fd = fds[i];
                    rt[i].timer = 0;
                    rt[i].full_report_enabled = false;
                    rt[i].usb_seen_mac = false;
                    rt[i].usb_handshake_done = false;
                    rt[i].usb_baudrate_set = false;
                    rt[i].usb_timeout_disabled = false;
                    rt[i].pending_subcmd_reply = false;
                    rt[i].last_standard_report_us = 0;
                    rt[i].last_idle_neutral_us = 0;
                    rt[i].neutral_burst_until_us = 0;
                    memset(&rt[i].pending_reply, 0, sizeof(rt[i].pending_reply));
                } else {
                    all_open = false;
                }
            }
        }

        if (!all_open) {
            g_switch2_usb_host_connected.store(false, std::memory_order_relaxed);
            // Do not keep a partial set of opened endpoints around while the
            // gadget is being recreated/rebound.  Retry with a clean fd set.
            for (int i = 0; i < HID_PORT_COUNT; ++i) {
                if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; rt[i].fd = -1; }
            }
            run_gadget_setup_if_needed(false, "requested /dev/hidg* nodes could not all be opened");
            for (int wait_i = 0; wait_i < 50 && g_running.load(std::memory_order_relaxed); ++wait_i) std::this_thread::sleep_for(ms(10));
            continue;
        }

        if (g_verbose || !was_connected)
            std::printf("%dx USB gamepad /dev/hidg* opened\n", HID_PORT_COUNT);
        was_connected = true;
        g_switch2_usb_host_connected.store(true, std::memory_order_relaxed);

        auto next = Clock::now() + tick;
        bool error_shown = false;
        bool timeout_printed[MAX_CLIENTS] = {};
        uint64_t writes_this_second = 0;
        auto last_rate_log = Clock::now();

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            uint64_t now_stamp = now_us();
            bool active_snap[MAX_CLIENTS] = {};
            bool uses_presence_snap[MAX_CLIENTS] = {};
            bool present_snap[MAX_CLIENTS][4] = {};
            uint64_t last_present_snap[MAX_CLIENTS][4] = {};
            ExtendedHIDReport report_snap[MAX_CLIENTS][4];
            MotionReport motion_snap[MAX_CLIENTS][4][3];
            bool has_motion_snap[MAX_CLIENTS][4] = {};
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                for (int s = 0; s < 4; ++s) {
                    report_snap[c][s].reset();
                    for (int i = 0; i < 3; ++i) motion_snap[c][s][i].reset();
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                std::lock_guard<std::mutex> lk(g_mtx[c]);
                repair_future_client_timestamp(g_clients[c], now_stamp);
                const uint64_t client_idle_us = elapsed_us_saturated(now_stamp, g_clients[c].last_rx_us);
                if (g_clients[c].active && g_clients[c].last_rx_us != 0 && client_idle_us > CLIENT_TIMEOUT_US && !server_macro_running(c,0) && !server_macro_running(c,1) && !server_macro_running(c,2) && !server_macro_running(c,3)) {
                    g_clients[c].active = false;
                    g_clients[c].report.reset();
                    clear_all_motion(g_clients[c]);
                    g_clients[c].uses_pad_presence = false;
                    for (int s = 0; s < 4; ++s) {
                        g_clients[c].pad_present[s] = false;
                        g_clients[c].pad_last_present_us[s] = 0;
                    }
                    if (g_verbose && !timeout_printed[c]) {
                        std::printf("PC %d timed out after %.1fs without UDP input and was disconnected.\n",
                                    c + 1, (double)elapsed_us_saturated(now_stamp, g_clients[c].last_rx_us) / 1000000.0);
                        timeout_printed[c] = true;
                    }
                } else if (g_clients[c].active) {
                    timeout_printed[c] = false;
                }
                active_snap[c] = g_clients[c].active;
                uses_presence_snap[c] = g_clients[c].uses_pad_presence;
                const bool input_stream_stale =
                    g_clients[c].active &&
                    g_clients[c].last_rx_us != 0 &&
                    client_idle_us > CLIENT_STALE_NEUTRAL_US;

                for (int s = 0; s < 4; ++s) {
                    present_snap[c][s] = g_clients[c].pad_present[s];
                    last_present_snap[c][s] = g_clients[c].pad_last_present_us[s];
                }

                if (input_stream_stale) {
                    // Keep the session/port alive through a short Windows UDP stall,
                    // but release all controls quickly so held R/ZR/sticks do not get
                    // stuck.  A real disconnect is handled later by CLIENT_TIMEOUT_US.
                    report_snap[c][0].reset();
                    report_snap[c][1].reset();
                    report_snap[c][2].reset();
                    report_snap[c][3].reset();
                    for (int s = 0; s < 4; ++s)
                        has_motion_snap[c][s] = false;
                } else {
                    report_snap[c][0] = g_clients[c].report.p1;
                    report_snap[c][1] = g_clients[c].report.p2;
                    report_snap[c][2] = g_clients[c].report.p3;
                    report_snap[c][3] = g_clients[c].report.p4;
                    for (int s = 0; s < 4; ++s) {
                        for (int i = 0; i < 3; ++i)
                            motion_snap[c][s][i] = g_clients[c].motion_samples[s][i];
                        has_motion_snap[c][s] = g_clients[c].has_motion[s];
                    }
                }
            }

            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (hw_slots[h].client_idx == -1) continue;

                int cidx = hw_slots[h].client_idx;
                int sidx = hw_slots[h].sub_idx;
                bool absent_too_long = false;
                if (uses_presence_snap[cidx] && !present_snap[cidx][sidx]) {
                    uint64_t last_seen = last_present_snap[cidx][sidx];
                    absent_too_long = (last_seen == 0) ||
                                      (now_stamp - last_seen >= WEB_PAD_ABSENT_RELEASE_US);
                }

                if ((!active_snap[cidx] || absent_too_long) && !server_macro_running(cidx, sidx)) {
                    hw_slots[h].client_idx = -1;
                    hw_slots[h].sub_idx = -1;

                    // The USB interface stays alive so the console can keep talking to
                    // it, but the player assignment is gone.  Send a short neutral
                    // release burst to clear any held buttons, drain stale output, then
                    // fall back to the low-rate idle heartbeat.
                    rt[h].neutral_burst_until_us = now_stamp + PRO_RELEASE_NEUTRAL_US;
                    drain_hid_output_queue(fds[h]);
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!active_snap[c]) continue;
                for (int s = 0; s < 4; ++s) {
                    bool mapped = false;
                    for (int h = 0; h < HID_PORT_COUNT; ++h) {
                        if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                            mapped = true;
                            break;
                        }
                    }

                    // A browser/mobile pad that is connected but currently neutral still
                    // needs to claim its console port so rumble has a target immediately.
                    if (mapped) continue;
                    bool macro_active_for_pad = server_macro_running(c, s);
                    if (uses_presence_snap[c]) {
                        if (!present_snap[c][s] && !macro_active_for_pad) continue;
                    } else if (extended_is_neutral(report_snap[c][s]) && !macro_active_for_pad) {
                        continue;
                    }

                    // Preserve logical pad order.  The previous "first free port" mapper
                    // let Pad 2 steal console port 1 whenever keyboard/mobile Pad 1 was
                    // neutral, which made keyboard mode and mobile mode look broken.
                    int chosen = -1;
                    if (s >= 0 && s < HID_PORT_COUNT && hw_slots[s].client_idx == -1) {
                        chosen = s;
                    } else {
                        // Fallback only for multi-client cases where the preferred port
                        // is already occupied.
                        for (int h = 0; h < HID_PORT_COUNT; ++h) {
                            if (hw_slots[h].client_idx == -1) {
                                chosen = h;
                                break;
                            }
                        }
                    }

                    if (chosen != -1) {
                        hw_slots[chosen].client_idx = c;
                        hw_slots[chosen].sub_idx = s;
                        if (g_verbose)
                            std::printf("Map -> PC %d (Pad %d) took console Pro Port %d\n", c + 1, s + 1, chosen + 1);
                    }
                }
            }

            ExtendedHIDReport out_reports[4];
            for (int h = 0; h < HID_PORT_COUNT; ++h) out_reports[h].reset();
            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (hw_slots[h].client_idx != -1) {
                    out_reports[h] = report_snap[hw_slots[h].client_idx][hw_slots[h].sub_idx];
                    server_macro_apply(hw_slots[h].client_idx, hw_slots[h].sub_idx, out_reports[h].input);
                }
            }

            bool ok = true;
            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                const bool port_needed = (hw_slots[h].client_idx != -1);

                uint8_t write_buf[PRO_REPORT_SIZE] = {};
                bool have_report_to_write = false;
                bool wrote_subcmd_reply = false;

                if (rt[h].pending_subcmd_reply) {
                    rt[h].pending_reply.id = RID_INPUT_SUBCMD;
                    rt[h].pending_reply.timer = pro_timer_from_us(now_stamp);
                    if (port_needed)
                        apply_input_controls_to_pro21(out_reports[h], rt[h].pending_reply);
                    else
                        fill_neutral_controls(rt[h].pending_reply);
                    memcpy(write_buf, &rt[h].pending_reply, sizeof(ProInputReport21));
                    have_report_to_write = true;
                    wrote_subcmd_reply = true;
                } else if (rt[h].full_report_enabled) {
                    // Active/player-assigned ports run at the shared boring
                    // 250Hz / 4ms cadence used by UDP clients and servers.
                    // Unassigned ports still send neutral keepalive reports,
                    // but only at a low heartbeat rate so we do not spam neutral data.
                    bool release_burst = rt[h].neutral_burst_until_us != 0 &&
                                         now_stamp < rt[h].neutral_burst_until_us;
                    if (rt[h].neutral_burst_until_us != 0 && now_stamp >= rt[h].neutral_burst_until_us)
                        rt[h].neutral_burst_until_us = 0;

                    bool idle_due = (rt[h].last_idle_neutral_us == 0) ||
                                    (elapsed_us_saturated(now_stamp, rt[h].last_idle_neutral_us) >= PRO_IDLE_REPORT_INTERVAL_US);
                    bool standard_due = (rt[h].last_standard_report_us == 0) ||
                                        (elapsed_us_saturated(now_stamp, rt[h].last_standard_report_us) >= PRO_REPORT_INTERVAL_US);

                    // Standard 0x30 reports use the same 250Hz cadence as the
                    // UDP input stream so reconnects and backend switching do
                    // not change timing behavior.
                    bool should_write_standard = false;
                    if (port_needed || release_burst) should_write_standard = standard_due;
                    else should_write_standard = idle_due;

                    if (should_write_standard) {
                        ExtendedHIDReport report_for_port;
                        report_for_port.reset();
                        if (port_needed) report_for_port = out_reports[h];

                        const MotionReport* motion_for_port = nullptr;
                        bool has_motion_for_port = false;
                        if (port_needed) {
                            int cidx = hw_slots[h].client_idx;
                            int sidx = hw_slots[h].sub_idx;
                            motion_for_port = motion_snap[cidx][sidx];
                            has_motion_for_port = has_motion_snap[cidx][sidx];
                        }

                        ProInputReport30 std_in{};
                        build_standard_report(report_for_port,
                                              motion_for_port,
                                              has_motion_for_port,
                                              rt[h].imu_enabled,
                                              pro_timer_from_us(now_stamp),
                                              std_in);
                        memcpy(write_buf, &std_in, sizeof(ProInputReport30));
                        have_report_to_write = true;

                        if (port_needed || release_burst)
                            rt[h].last_standard_report_us = now_stamp;
                        if (!port_needed && !release_burst)
                            rt[h].last_idle_neutral_us = now_stamp;
                    }
                }

                if (!have_report_to_write) continue;

                ssize_t w = write(fds[h], write_buf, PRO_REPORT_SIZE);
                if (w < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
                    // If a subcommand reply could not be written, keep it pending.
                    // Dropping it makes the console repeat commands such as 0x02 forever.
                } else if (w == (ssize_t)PRO_REPORT_SIZE) {
                    if (wrote_subcmd_reply) rt[h].pending_subcmd_reply = false;
                    writes_this_second++;
                } else if (w > 0) {
                    // Partial HID report writes should not happen.  Treat as an error so
                    // we reconnect cleanly rather than sending malformed controller data.
                    ok = false;
                }
            }

            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                // Always serve the HID control/output side for every exposed Pro
                // Controller interface.  HID gadgets are not lazily created: once
                // setup_gadget.sh exposes hidg0..hidg3, the the host may send init
                // commands to any of them.  Ignoring those commands until a pad maps
                // to the port breaks keyboard/mobile/web input and leaves stale output
                // reports queued in /dev/hidgX.
                for (int output_reads = 0; output_reads < 8; ++output_reads) {
                    struct pollfd pfd = {fds[h], POLLIN, 0};
                    uint8_t read_buf[PRO_REPORT_SIZE];
                    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
                        break;

                    ssize_t r = read(fds[h], read_buf, PRO_REPORT_SIZE);
                    if (r <= 0) continue;

                    // USB gadget reads may occasionally return 2-byte 00 00
                    // frames from /dev/hidg0. They are not valid controller output
                    // reports and forwarding them to a real USB gamepad caused
                    // WriteFile(ERROR_INVALID_PARAMETER).  Ignore them here too.
                    if (r < 2 || (r == 2 && read_buf[0] == 0x00 && read_buf[1] == 0x00))
                        continue;

                    uint8_t id = read_buf[0];
                    if (id == RID_OUTPUT_CMD) {
                        if (hw_slots[h].client_idx != -1)
                            publish_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, false);

                        uint8_t subcmd_id = read_buf[10];
                        size_t subcmd_data_len = r > 11 ? std::min((size_t)53, (size_t)(r - 11)) : 0;
                        memset(&rt[h].pending_reply, 0, sizeof(rt[h].pending_reply));
                        int reply_len = handle_subcommand(
                            rt[h], subcmd_id,
                            subcmd_data_len > 0 ? read_buf + 11 : nullptr,
                            subcmd_data_len,
                            &rt[h].pending_reply
                        );
                        rt[h].pending_subcmd_reply = (reply_len >= 0);
                        if (g_verbose) {
                            std::printf("[pro%d] subcmd 0x%02X reply=0x%02X 0x%02X",
                                        h + 1, subcmd_id, rt[h].pending_reply.ack, rt[h].pending_reply.subcmd_id);
                            if ((subcmd_id == CMD_SET_DATA_FORMAT || subcmd_id == CMD_ENABLE_IMU ||
                                 subcmd_id == CMD_ENABLE_VIBRATION) &&
                                subcmd_data_len > 0) {
                                std::printf(" data=");
                                for (size_t bi = 0; bi < subcmd_data_len && bi < 8; ++bi)
                                    std::printf("%s%02X", bi ? " " : "", read_buf[11 + bi]);
                            }
                            std::printf("\n");
                        }
                    } else if (id == RID_OUTPUT_RUMBLE) {
                        if (hw_slots[h].client_idx != -1)
                            publish_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, true);
                    } else if (id == 0x80) {
                        const uint8_t usb_cmd = read_buf[1];
                        uint8_t resp_81[PRO_REPORT_SIZE] = {};
                        build_usb_81_response(resp_81, usb_cmd, h);

                        switch (usb_cmd) {
                        case 0x01: rt[h].usb_seen_mac = true; break;
                        case 0x02: rt[h].usb_handshake_done = true; break;
                        case 0x03: rt[h].usb_baudrate_set = true; break;
                        case 0x04: rt[h].usb_timeout_disabled = true; break;
                        case 0x05: rt[h].usb_timeout_disabled = false; break;
                        default: break;
                        }

                        ssize_t w = write(fds[h], resp_81, PRO_REPORT_SIZE);
                        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
                        if (g_verbose) {
                            std::printf("[pro%d] usb 0x80 cmd=0x%02X -> 0x81 subtype=0x%02X mac=%02X:%02X:%02X:%02X:%02X:%02X timeout=%s\n",
                                        h + 1, usb_cmd, resp_81[1],
                                        CTRL_MAC_BE[h][0], CTRL_MAC_BE[h][1], CTRL_MAC_BE[h][2],
                                        CTRL_MAC_BE[h][3], CTRL_MAC_BE[h][4], CTRL_MAC_BE[h][5],
                                        rt[h].usb_timeout_disabled ? "disabled" : "enabled");
                        }
                    } else {
                        if (g_verbose && id != 0x00)
                            std::printf("[pro%d] unknown output report id=0x%02X len=%zd\n", h + 1, id, r);
                    }
                }
            }

            if (!ok) {
                if (!error_shown) { std::puts("Host disconnected — waiting for reconnect..."); error_shown = true; }
                g_switch2_usb_host_connected.store(false, std::memory_order_relaxed);
                for (int i = 0; i < 4; ++i) { close(fds[i]); fds[i] = -1; rt[i].fd = -1; }
                for (int wait_i = 0; wait_i < 100 && g_running.load(std::memory_order_relaxed); ++wait_i) std::this_thread::sleep_for(ms(10));
                break;
            }

            auto now_log = Clock::now();
            if (g_verbose && now_log - last_rate_log >= ms(1000)) {
                std::printf("pro_hid_writes/sec=%llu\n", (unsigned long long)writes_this_second);
                writes_this_second = 0;
                last_rate_log = now_log;
            }
            if (writes_this_second) ++g_hid_writes;
        }
    }

    for (int i = 0; i < 4; ++i) {
        if (fds[i] >= 0) close(fds[i]);
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
static std::mutex g_rate_mtx;

// ── Stats thread ──────────────────────────────────────────────────────────────
static void stats_thread() {
    uint64_t last_cleanup = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        for (int wait_i = 0; wait_i < 50 && g_running.load(std::memory_order_relaxed); ++wait_i)
            std::this_thread::sleep_for(ms(100));
        if (!g_running.load(std::memory_order_relaxed)) break;

        // Periodic rate limiter cleanup (every 60s)
        uint64_t now = now_us();
        if (now - last_cleanup > 60000000) {
            last_cleanup = now;
            std::lock_guard<std::mutex> lk(g_rate_mtx);
            for (int i = 0; i < RATE_TABLE; ++i) {
                if (g_rate_table[i].ip != 0 &&
                    now - g_rate_table[i].window_start > RATE_WINDOW_US * 2)
                    g_rate_table[i].ip = 0;
            }
        }

        if (!g_verbose) continue;
        std::printf("pkts_rx=%-8llu  hid_writes=%-8llu\n",
            (unsigned long long)g_pkts_rx.load(),
            (unsigned long long)g_hid_writes.load());
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
static bool rate_allow(uint32_t ip) {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    uint64_t now = now_us();
    uint32_t idx = ip % RATE_TABLE;
    RateSlot &s = g_rate_table[idx];
    if (s.ip != ip) {
        s.ip = ip; s.count = 1; s.window_start = now; return true;
    }
    if (now - s.window_start > RATE_WINDOW_US) {
        s.count = 1; s.window_start = now; return true;
    }
    if (s.count < UINT32_MAX) s.count++;
    return s.count <= RATE_MAX_PKT;
}


#ifdef USE_UPNP
// ── UPnP port forwarding ──
static bool     g_upnp_active = false;
static uint16_t g_upnp_port   = 0;
static UPNPUrls g_upnp_urls{};
static IGDdatas g_upnp_data{};
static char     g_upnp_lan_addr[64]{};

static bool upnp_add_mapping(uint16_t port) {
    if (g_upnp_active) return false; 

    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
    if (!devlist) return false;
    
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
    freeUPNPDevlist(devlist);
    
    if (igd != 1 && igd != 2) { FreeUPNPUrls(&g_upnp_urls); return false; }
    
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    
    int r = UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype,
                                port_str, port_str, g_upnp_lan_addr, "ns-backend", "UDP", nullptr, "0");
    if (r != 0) { FreeUPNPUrls(&g_upnp_urls); return false; }
    
    g_upnp_active = true;
    g_upnp_port = port;
    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, external_ip) == 0) {
        std::printf("UPnP: UDP port %u successfully forwarded!\n", port);
        std::printf("UPnP: Tell your clients to connect to -> %s:%u\n", external_ip, port);
    }
    return true;
}

static void upnp_remove_mapping(uint16_t) {
    if (!g_upnp_active) return;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", g_upnp_port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str, "UDP", nullptr);
    std::puts("UPnP: port mapping removed cleanly");
    FreeUPNPUrls(&g_upnp_urls); g_upnp_active = false; g_upnp_port = 0;
}
#else
static bool upnp_add_mapping(uint16_t) { return false; }
static void upnp_remove_mapping(uint16_t) {}
#endif


// ══════════════════════════════════════════════════════════════════════════════
// ── Embedded Web Server (HTTP + WebSocket proxy, enabled with -w) ────────────
// ══════════════════════════════════════════════════════════════════════════════

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


// ── Single-threaded WebSocket/HTTP client state ─────────────────────────────
static constexpr int MAX_WS_CLIENTS = 32;

struct WebClient {
    int fd = -1;

    uint8_t buf[65536];
    size_t fill = 0;

    enum State : uint8_t {
        READ_HTTP,
        WRITE_RESP,
        WS_ACTIVE,
        CLOSED
    } state = CLOSED;

    char http_buf[8192];
    size_t http_len = 0;
    uint64_t connect_time = 0;
    uint32_t ip = 0;

    int      ws_slot = -1;
    uint32_t ws_seq = 0;
    bool     ws_first = true;
    uint64_t ws_last_rx = 0;
    uint64_t last_ping_us = 0;
    uint32_t last_rumble_seq[4] = {};

    uint8_t *wbuf = nullptr;
    size_t   wlen = 0;
    size_t   woff = 0;
    State    after_write = CLOSED;
};

static void legacy_multi_to_extended(const MultiReport& in, ExtendedMultiReport& out) {
    out.reset();
    out.p1.input = in.p1;
    out.p2.input = in.p2;
    out.p3.input = in.p3;
    out.p4.input = in.p4;
}

struct NS_LOCAL_PACKED ExtendedUdpPacket {
    uint32_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t seq;
    uint64_t timestamp_us;
    ExtendedMultiReport report;
    uint8_t  hmac[HMAC_TAG_SIZE];
};

struct NS_LOCAL_PACKED ExtendedUdpPacket3 {
    uint32_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t seq;
    uint64_t timestamp_us;
    ExtendedMultiReport3 report;
    uint8_t  hmac[HMAC_TAG_SIZE];
};

static constexpr size_t EXT_UDP_PACKET_AUTH_SIZE  = 20 + sizeof(ExtendedMultiReport);
static constexpr size_t EXT_UDP_PACKET_SIZE       = EXT_UDP_PACKET_AUTH_SIZE + HMAC_TAG_SIZE;
static constexpr size_t EXT3_UDP_PACKET_AUTH_SIZE = 20 + sizeof(ExtendedMultiReport3);
static constexpr size_t EXT3_UDP_PACKET_SIZE      = EXT3_UDP_PACKET_AUTH_SIZE + HMAC_TAG_SIZE;
static constexpr size_t UDP_RX_MAX_PACKET_SIZE    =
    sizeof(ExtendedUdpPacket3) > sizeof(Packet) ? sizeof(ExtendedUdpPacket3) : sizeof(Packet);
static_assert(sizeof(ExtendedUdpPacket) == EXT_UDP_PACKET_SIZE,
              "ExtendedUdpPacket size must match its wire format");
static_assert(sizeof(ExtendedUdpPacket3) == EXT3_UDP_PACKET_SIZE,
              "ExtendedUdpPacket3 size must match its wire format");

static bool extended_udp_packet_ok(const ExtendedUdpPacket& p) {
    return p.magic == PROTO_MAGIC &&
           (p.version == WEB_PROTO_VERSION || p.version == PROTO_VERSION);
}

static bool extended_udp3_packet_ok(const ExtendedUdpPacket3& p) {
    return p.magic == PROTO_MAGIC && p.version == WEB_PROTO_VERSION_3;
}

static bool extended_report_pad_present(const ExtendedMultiReport& report, int subpad) {
    if (subpad < 0 || subpad >= 4) return false;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&report);
    return (raw[subpad * sizeof(ExtendedHIDReport) + 7] & 0x01) != 0;
}

static bool extended3_report_pad_present(const ExtendedMultiReport3& report, int subpad) {
    if (subpad < 0 || subpad >= 4) return false;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&report);
    return (raw[subpad * sizeof(ExtendedHIDReport3) + 7] & 0x01) != 0;
}

static void extended3_to_extended_latest(const ExtendedHIDReport3& in, ExtendedHIDReport& out) {
    out.reset();
    out.input = in.input;
    out.has_motion = in.has_motion;
    if (in.has_motion) out.motion = in.motion[2];
}

static void clear_udp_rumble_state(ClientSession& c) {
    c.udp_rumble_enabled = false;
    for (int s = 0; s < 4; ++s)
        c.udp_last_rumble_seq[s] = c.rumble_seq[s];
}

static void reset_udp_client_session_locked(ClientSession& c) {
    c.active = false;
    c.first_pkt = true;
    c.expected_seq = 0;
    c.last_rx_us = 0;
    c.report.reset();
    clear_all_motion(c);
    c.uses_pad_presence = false;
    clear_udp_rumble_state(c);
    for (int s = 0; s < 4; ++s) {
        c.pad_present[s] = false;
        c.pad_last_present_us[s] = 0;
    }
}

static void enable_udp_rumble_state(ClientSession& c) {
    if (!c.udp_rumble_enabled) {
        c.udp_rumble_enabled = true;
        for (int s = 0; s < 4; ++s)
            c.udp_last_rumble_seq[s] = c.rumble_seq[s];
    }
}

static void flush_rumble_to_udp(int sock, int client_idx) {
    if (sock < 0 || client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    sockaddr_in dest{};
    RumblePacket pending[4]{};
    bool has[4]{};
    {
        std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
        ClientSession& c = g_clients[client_idx];
        if (!c.active || !c.udp_rumble_enabled) return;
        dest = c.addr;
        for (int s = 0; s < 4; ++s) {
            uint32_t seq = c.rumble_seq[s];
            if (seq != c.udp_last_rumble_seq[s]) {
                pending[s] = c.rumble[s];
                c.udp_last_rumble_seq[s] = seq;
                has[s] = true;
            }
        }
    }
    for (int s = 0; s < 4; ++s) {
        if (!has[s]) continue;
        ssize_t sent = sendto(sock, &pending[s], sizeof(RumblePacket), 0,
                              reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (g_verbose && sent != (ssize_t)sizeof(RumblePacket))
            std::fprintf(stderr, "[udp] failed to send rumble packet: %s\n", std::strerror(errno));
    }
}

static bool send_ws_binary_frame(WebClient* c, const uint8_t* payload, size_t len) {
    if (!c || c->state != WebClient::WS_ACTIVE || c->fd < 0) return false;
    if (c->wbuf != nullptr) return false;
    if (len >= 126) return false;

    const size_t hdr = 2;
    const size_t total = hdr + len;
    uint8_t small_frame[2 + sizeof(RumblePacket)] = {};
    if (total > sizeof(small_frame)) return false;

    small_frame[0] = 0x82;
    small_frame[1] = (uint8_t)len;
    memcpy(small_frame + hdr, payload, len);

    ssize_t w = write(c->fd, small_frame, total);
    if (w == (ssize_t)total) return true;

    size_t written = 0;
    if (w < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
    } else if (w > 0) {
        written = (size_t)w;
    }

    c->wbuf = (uint8_t*)malloc(total);
    if (!c->wbuf) return false;
    memcpy(c->wbuf, small_frame, total);
    c->wlen = total;
    c->woff = written;
    c->after_write = WebClient::WS_ACTIVE;
    return true;
}

static void flush_rumble_to_ws(WebClient* c) {
    if (!c || c->state != WebClient::WS_ACTIVE || c->ws_slot < 0) return;

    RumblePacket pending[4]{};
    uint32_t seqs[4]{};
    bool has[4]{};

    {
        std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
        for (int s = 0; s < 4; ++s) {
            uint32_t seq = g_clients[c->ws_slot].rumble_seq[s];
            if (seq != c->last_rumble_seq[s]) {
                pending[s] = g_clients[c->ws_slot].rumble[s];
                seqs[s] = seq;
                has[s] = true;
            }
        }
    }

    for (int s = 0; s < 4; ++s) {
        if (!has[s]) continue;
        if (send_ws_binary_frame(c, (const uint8_t*)&pending[s], sizeof(RumblePacket))) {
            c->last_rumble_seq[s] = seqs[s];
        }
    }
}


// ── Process one complete WebSocket frame from client buffer ──────────────────
// Returns bytes consumed, or 0 if need more data.  Sets c->state = CLOSED on close/error.
static size_t process_ws_frame(WebClient *c) {
    uint8_t *buf = c->buf;
    size_t len  = c->fill;
    if (len < 2) return 0;

    int      opcode  = buf[0] & 0x0F;
    bool     masked  = buf[1] & 0x80;
    uint64_t flen    = buf[1] & 0x7F;
    size_t   hdr_sz  = 2;

    if (flen == 126) {
        if (len < 4) return 0;
        flen   = ((uint64_t)buf[2] << 8) | buf[3];
        hdr_sz = 4;
    } else if (flen == 127) {
        if (len < 10) return 0;
        flen = 0;
        for (int i = 0; i < 8; i++) flen = (flen << 8) | buf[2 + i];
        hdr_sz = 10;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (len < hdr_sz + 4) return 0;
        memcpy(mask, buf + hdr_sz, 4);
        hdr_sz += 4;
    }

    if (len < hdr_sz + flen) return 0;

    uint8_t *payload = buf + hdr_sz;
    size_t   total   = hdr_sz + flen;

    if (opcode == 9) {
        if (masked)
            for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];
        uint8_t pong[2] = {0x8A, 0x00};
        ssize_t _u = write(c->fd, pong, 2); (void)_u;
        return total;
    }
    if (opcode == 8) {
        uint8_t close_frame[] = {0x88, 0x00};
        ssize_t _u = write(c->fd, close_frame, 2); (void)_u;
        c->state = WebClient::CLOSED;
        return total;
    }

    if (opcode == 1) {
        if (masked)
            for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];
        std::string text(reinterpret_cast<char*>(payload), (size_t)flen);
        const std::string prefix = "MACRO_RUN:";
        if (text.rfind(prefix, 0) == 0) {
            uint64_t now = now_us();
            bool wake_on_new_client = false;
            if (c->ws_slot < 0) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_mtx[i]);
                    if (!g_clients[i].active) {
                        c->ws_slot = i;
                        g_clients[i].active = true;
                        g_clients[i].first_pkt = true;
                        g_clients[i].report.reset();
                        clear_all_motion(g_clients[i]);
                        g_clients[i].uses_pad_presence = true;
                        clear_udp_rumble_state(g_clients[i]);
                        for (int s = 0; s < 4; ++s) { g_clients[i].pad_present[s] = false; g_clients[i].pad_last_present_us[s] = 0; }
                        g_clients[i].last_rx_us = now;
                        wake_on_new_client = true;
                        break;
                    }
                }
            }
            if (wake_on_new_client)
                maybe_send_switch2_wake_advert("client connected via WebSocket macro");
            if (c->ws_slot >= 0) {
                {
                    std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
                    g_clients[c->ws_slot].active = true;
                    g_clients[c->ws_slot].uses_pad_presence = true;
                    g_clients[c->ws_slot].pad_present[0] = true;
                    g_clients[c->ws_slot].pad_last_present_us[0] = now;
                    g_clients[c->ws_slot].last_rx_us = now;
                }
                server_macro_start(c->ws_slot, 0, text.substr(prefix.size()));
            }
        }
        return total;
    }
    if (opcode == 0) {
        c->state = WebClient::CLOSED;
        return total;
    }
    if (opcode != 2) return total;

    if (masked)
        for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];

    if (flen >= ns::macro::CHUNK_HEADER_SIZE) {
        uint32_t maybe_macro_magic = 0;
        memcpy(&maybe_macro_magic, payload, 4);
        if (maybe_macro_magic == ns::macro::UDP_CHUNK_MAGIC) {
            uint64_t now = now_us();
            bool wake_on_new_client = false;
            if (c->ws_slot < 0) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_mtx[i]);
                    if (!g_clients[i].active) {
                        c->ws_slot = i;
                        g_clients[i].active = true;
                        g_clients[i].first_pkt = true;
                        g_clients[i].report.reset();
                        clear_all_motion(g_clients[i]);
                        g_clients[i].uses_pad_presence = true;
                        g_clients[i].last_rx_us = now;
                        wake_on_new_client = true;
                        break;
                    }
                }
            }
            if (wake_on_new_client)
                maybe_send_switch2_wake_advert("client connected via WebSocket macro chunk");
            if (c->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
                g_clients[c->ws_slot].active = true;
                g_clients[c->ws_slot].uses_pad_presence = true;
                g_clients[c->ws_slot].pad_present[0] = true;
                g_clients[c->ws_slot].pad_last_present_us[0] = now;
                g_clients[c->ws_slot].last_rx_us = now;
            }
            if (c->ws_slot >= 0) server_macro_handle_ws_chunk_packet(c->ws_slot, payload, (size_t)flen);
            return total;
        }
    }

    if (flen != PACKET_SIZE && flen != WEB_PACKET_SIZE && flen != WEB_PACKET3_SIZE) {
        c->state = WebClient::CLOSED;
        return total;
    }

    uint32_t magic; memcpy(&magic, payload, 4);
    if (magic != PROTO_MAGIC) return total;
    uint8_t ver; memcpy(&ver, payload + 4, 1);
    uint8_t flags; memcpy(&flags, payload + 5, 1);
    bool is_reset = (flags & FLAG_RESET);
    uint32_t seq; memcpy(&seq, payload + 8, 4);

    ExtendedMultiReport report;
    ExtendedMultiReport3 report3;
    report.reset();
    report3.reset();
    bool is_report3 = false;
    bool pad_present[4] = {};

    if (ver == PROTO_VERSION && flen == PACKET_SIZE) {
        MultiReport legacy;
        memcpy(&legacy, payload + 20, sizeof(MultiReport));
        legacy_multi_to_extended(legacy, report);
        pad_present[0] = !extended_is_neutral(report.p1);
        pad_present[1] = !extended_is_neutral(report.p2);
        pad_present[2] = !extended_is_neutral(report.p3);
        pad_present[3] = !extended_is_neutral(report.p4);
    } else if ((ver == WEB_PROTO_VERSION || ver == PROTO_VERSION) && flen == WEB_PACKET_SIZE) {
        memcpy(&report, payload + 20, sizeof(ExtendedMultiReport));
        for (int s = 0; s < 4; ++s)
            pad_present[s] = (payload[20 + s * sizeof(ExtendedHIDReport) + 7] & 0x01) != 0;
        if (flags & FLAG_SINGLE_PAD) {
            report.p2.reset(); report.p3.reset(); report.p4.reset();
            pad_present[0] = true;
            pad_present[1] = false; pad_present[2] = false; pad_present[3] = false;
        }
    } else if (ver == WEB_PROTO_VERSION_3 && flen == WEB_PACKET3_SIZE) {
        is_report3 = true;
        memcpy(&report3, payload + 20, sizeof(ExtendedMultiReport3));
        const ExtendedHIDReport3* src3[4] = { &report3.p1, &report3.p2, &report3.p3, &report3.p4 };
        ExtendedHIDReport* dst1[4] = { &report.p1, &report.p2, &report.p3, &report.p4 };
        for (int s = 0; s < 4; ++s) {
            pad_present[s] = (payload[20 + s * sizeof(ExtendedHIDReport3) + 7] & 0x01) != 0;
            extended3_to_extended_latest(*src3[s], *dst1[s]);
        }
        if (flags & FLAG_SINGLE_PAD) {
            report.p2.reset(); report.p3.reset(); report.p4.reset();
            report3.p2.reset(); report3.p3.reset(); report3.p4.reset();
            pad_present[0] = true;
            pad_present[1] = false; pad_present[2] = false; pad_present[3] = false;
        }
    } else {
        return total;
    }

    if (!c->ws_first && !is_reset && (int32_t)(seq - c->ws_seq) < 0) return total;
    c->ws_first = false;
    c->ws_seq = seq + 1;

    uint64_t now = now_us();
    bool wake_on_new_client = false;

    if (c->ws_slot >= 0) {
        std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
        if (!g_clients[c->ws_slot].active)
            c->ws_slot = -1;
    }
    if (c->ws_slot < 0) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            std::lock_guard<std::mutex> lk(g_mtx[i]);
            if (!g_clients[i].active) {
                c->ws_slot = i;
                g_clients[i].active = true;
                g_clients[i].first_pkt = true;
                g_clients[i].report.reset();
                clear_all_motion(g_clients[i]);
                g_clients[i].uses_pad_presence = true;
                clear_udp_rumble_state(g_clients[i]);
                for (int s = 0; s < 4; ++s) {
                    g_clients[i].pad_present[s] = false;
                    g_clients[i].pad_last_present_us[s] = 0;
                }
                g_clients[i].last_rx_us = now;
                wake_on_new_client = true;
                for (int s = 0; s < 4; ++s)
                    c->last_rumble_seq[s] = g_clients[i].rumble_seq[s];
                break;
            }
        }
    }
    if (wake_on_new_client)
        maybe_send_switch2_wake_advert("client connected via WebSocket input");
    if (c->ws_slot >= 0) {
        std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);

        ExtendedHIDReport* dst_pads[4] = {
            &g_clients[c->ws_slot].report.p1,
            &g_clients[c->ws_slot].report.p2,
            &g_clients[c->ws_slot].report.p3,
            &g_clients[c->ws_slot].report.p4,
        };
        const ExtendedHIDReport* src_pads[4] = {
            &report.p1, &report.p2, &report.p3, &report.p4,
        };
        const ExtendedHIDReport3* src_pads3[4] = {
            &report3.p1, &report3.p2, &report3.p3, &report3.p4,
        };

        if (is_reset) {
            g_clients[c->ws_slot].report.reset();
            clear_all_motion(g_clients[c->ws_slot]);
            for (int s = 0; s < 4; ++s) {
                g_clients[c->ws_slot].pad_present[s] = false;
                g_clients[c->ws_slot].pad_last_present_us[s] = 0;
            }
        } else {
            for (int s = 0; s < 4; ++s) {
                if (pad_present[s]) {
                    *dst_pads[s] = *src_pads[s];
                    if (is_report3) {
                        if (src_pads3[s]->has_motion)
                            set_motion_samples(g_clients[c->ws_slot], s, src_pads3[s]->motion);
                        else
                            clear_motion(g_clients[c->ws_slot], s);
                    } else {
                        if (src_pads[s]->has_motion)
                            set_motion(g_clients[c->ws_slot], s, src_pads[s]->motion);
                        else
                            clear_motion(g_clients[c->ws_slot], s);
                    }
                    g_clients[c->ws_slot].pad_present[s] = true;
                    g_clients[c->ws_slot].pad_last_present_us[s] = now;
                } else {
                    g_clients[c->ws_slot].pad_present[s] = false;
                    uint64_t last_seen = g_clients[c->ws_slot].pad_last_present_us[s];
                    if (last_seen == 0 || now - last_seen >= WEB_PAD_ABSENT_RELEASE_US) {
                        dst_pads[s]->reset();
                        clear_motion(g_clients[c->ws_slot], s);
                    }
                }
            }
        }
        g_clients[c->ws_slot].last_rx_us = now;
    }
    c->ws_last_rx = now;
    ++g_pkts_rx;

    return total;
}


// ── Perform WebSocket upgrade handshake ──────────────────────────────────────
// Returns response length, or -1 on failure.  Caller must queue via async write.
static int ws_upgrade(const char *key_line, char *resp, size_t resp_sz) {
    const char *key_start = nullptr;
    const char *header_name = "sec-websocket-key:";
    const char *p = key_line;
    while (*p) {
        const char *h = header_name;
        const char *b = p;
        while (*h && *b && (tolower((unsigned char)*b) == tolower((unsigned char)*h))) {
            b++; h++;
        }
        if (!*h) { key_start = b; break; }
        p++;
    }

    if (!key_start) return -1;

    while (*key_start == ' ') key_start++;
    const char *key_end = strchr(key_start, '\r');
    if (!key_end) key_end = strchr(key_start, '\n');
    if (!key_end) return -1;

    char key[256];
    size_t klen = key_end - key_start;
    if (klen >= sizeof(key)) return -1;
    memcpy(key, key_start, klen);
    key[klen] = '\0';

    const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t sha_input[256];
    size_t slen = snprintf((char*)sha_input, sizeof(sha_input), "%s%s", key, magic);

    Sha1Ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, sha_input, slen);
    uint8_t digest[20];
    sha1_final(&ctx, digest);

    char b64out[64];
    base64_encode(digest, 20, b64out);

    return snprintf(resp, resp_sz,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", b64out);
}


// ── Format HTML body (heap-allocated, caller must free) ──────────────────────
// ── Case-insensitive header check ───────────────────────────────────────────
static bool has_header(const char *buf, const char *header) {
    size_t hlen = strlen(header);
    const char *p = buf;
    while (*p) {
        if ((p == buf || p[-1] == '\n') &&
            strncasecmp(p, header, hlen) == 0)
            return true;
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return false;
}

// ── Request-line prefix match (line-start only) ──────────────────────────────
static bool req_match(const char *buf, const char *path) {
    size_t plen = strlen(path);
    const char *p = buf;
    while (*p) {
        if ((p == buf || p[-1] == '\n') &&
            strncmp(p, path, plen) == 0)
            return true;
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return false;
}


enum class WebServerMode : uint8_t {
    WebSocketOnly,
    WebApp
};

// ── WebSocket/Web Server Thread (single-threaded poll reactor, fully non-blocking) ─────
static void web_server_thread(int web_port, uint16_t udp_port, WebServerMode mode) {
    (void)udp_port;
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

    if (mode == WebServerMode::WebApp)
        std::printf("[web] HTTP + WebSocket server listening on port %d\n", web_port);
    else
        std::printf("[ws] WebSocket server listening on port %d\n", web_port);

    struct pollfd pfds[1 + MAX_WS_CLIENTS];
    static WebClient clients[MAX_WS_CLIENTS];
    int           n_clients = 0;

    pfds[0].fd = srv; pfds[0].events = POLLIN; pfds[0].revents = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) { pfds[i+1].fd = -1; }

    while (g_running.load(std::memory_order_relaxed)) {
        // Push pending classic NSVR rumble events back to browser/mobile WebSocket clients.
        for (int i = 0; i < n_clients; i++)
            if (clients[i].state == WebClient::WS_ACTIVE)
                flush_rumble_to_ws(&clients[i]);

        // Periodic WebSocket ping every 10s so clients detect dead connections.
        uint64_t now_ws = now_us();
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state == WebClient::WS_ACTIVE &&
                now_ws - clients[i].last_ping_us >= 10000000 &&
                clients[i].wbuf == nullptr) {
                uint8_t ping[2] = {0x89, 0x00};
                ssize_t n = write(clients[i].fd, ping, sizeof(ping));
                if (n == (ssize_t)sizeof(ping))
                    clients[i].last_ping_us = now_ws;
            }
        }

        // Idle WS timeout (30s) and HTTP handshake timeout (5s)
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state == WebClient::WS_ACTIVE &&
                now_ws - clients[i].ws_last_rx > 30000000)
                clients[i].state = WebClient::CLOSED;
            if (clients[i].state == WebClient::READ_HTTP &&
                clients[i].connect_time > 0 &&
                now_ws - clients[i].connect_time > 5000000)
                clients[i].state = WebClient::CLOSED;
        }

        // Update poll events based on client state (POLLOUT for write, POLLIN for read)
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state != WebClient::CLOSED) {
                if (clients[i].state == WebClient::WRITE_RESP)
                    pfds[i+1].events = POLLOUT;
                else if (clients[i].state == WebClient::WS_ACTIVE && clients[i].wbuf != nullptr)
                    pfds[i+1].events = POLLIN | POLLOUT;
                else
                    pfds[i+1].events = POLLIN;
                pfds[i+1].revents = 0;
            }
        }

        int rc = poll(pfds, 1 + n_clients, 200);
        if (rc <= 0) continue;

        // ── Accept new connections ─────────────────────────────────────────
        if (pfds[0].revents & POLLIN) {
            sockaddr_in peer{};
            socklen_t   plen = sizeof(peer);
            int fd = accept(srv, (sockaddr*)&peer, &plen);
            if (fd >= 0) {
                uint32_t peer_ip = peer.sin_addr.s_addr;
                int cnt = 0;
                for (int i = 0; i < n_clients; i++)
                    if (clients[i].state != WebClient::CLOSED && clients[i].ip == peer_ip)
                        cnt++;
                if (cnt >= 8) {
                    close(fd);
                    if (g_verbose) std::printf("[web] client rejected: %d connections from this IP\n", cnt);
                    continue;
                }

                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                struct timeval tv = {10, 0};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                int slot = -1;
                for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                    if (clients[i].state == WebClient::CLOSED) { slot = i; break; }
                }
                if (slot >= 0) {
                    clients[slot] = WebClient{};
                    clients[slot].fd = fd;
                    clients[slot].state = WebClient::READ_HTTP;
                    clients[slot].connect_time = now_us();
                    clients[slot].ip = peer_ip;
                    pfds[slot+1].fd = fd;
                    pfds[slot+1].events = POLLIN;
                    pfds[slot+1].revents = 0;
                    if (slot >= n_clients) n_clients = slot + 1;
                    if (g_verbose) std::printf("[web] client %d accepted (slot %d)\n", fd, slot);
                } else {
                    close(fd);
                    if (g_verbose) std::puts("[web] rejected: all slots full");
                }
            }
        }

        // ── Service existing clients ───────────────────────────────────────
        for (int i = 0; i < n_clients; i++) {
            WebClient *c = &clients[i];
            if (c->state == WebClient::CLOSED) continue;
            short rev = pfds[i+1].revents;
            if (rev & (POLLHUP | POLLERR)) { c->state = WebClient::CLOSED; continue; }

            // ── WRITE_RESP: flush queued HTTP response ──────────────────────
            if (c->state == WebClient::WRITE_RESP && (rev & POLLOUT)) {
                ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        continue; // will retry on next poll cycle
                    c->state = WebClient::CLOSED;
                } else {
                    c->woff += n;
                    if (c->woff >= c->wlen) {
                        free(c->wbuf);
                        c->wbuf = nullptr;
                        c->state = c->after_write;
                    }
                }
                continue;
            }

            // ── WS_ACTIVE: flush queued server→browser binary frames ─────────
            if (c->state == WebClient::WS_ACTIVE && c->wbuf != nullptr && (rev & POLLOUT)) {
                ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
                if (n < 0) {
                    if (!(errno == EAGAIN || errno == EWOULDBLOCK))
                        c->state = WebClient::CLOSED;
                } else {
                    c->woff += n;
                    if (c->woff >= c->wlen) {
                        free(c->wbuf);
                        c->wbuf = nullptr;
                        c->wlen = c->woff = 0;
                    }
                }
                if (c->state == WebClient::CLOSED) continue;
            }

            if (!(rev & POLLIN)) continue;

            // Read available data (defensive: check for buffer full first)
            if (c->fill >= sizeof(c->buf)) { c->state = WebClient::CLOSED; continue; }
            ssize_t n = read(c->fd, c->buf + c->fill, sizeof(c->buf) - c->fill);
            if (n <= 0) { c->state = WebClient::CLOSED; continue; }
            c->fill += n;

            // ── READ_HTTP: accumulate headers, then dispatch ────────────────
            if (c->state == WebClient::READ_HTTP) {
                size_t copy = std::min(sizeof(c->http_buf) - 1 - c->http_len, c->fill);
                if (copy == 0) { c->state = WebClient::CLOSED; continue; }
                memcpy(c->http_buf + c->http_len, c->buf, copy);
                c->http_len += copy;
                memmove(c->buf, c->buf + copy, c->fill - copy);
                c->fill -= copy;
                c->http_buf[c->http_len] = '\0';

                if (c->http_len >= 4 &&
                    c->http_buf[c->http_len-1] == '\n' &&
                    c->http_buf[c->http_len-2] == '\r' &&
                    c->http_buf[c->http_len-3] == '\n' &&
                    c->http_buf[c->http_len-4] == '\r')
                {
                    bool is_ws = has_header(c->http_buf, "upgrade: websocket") &&
                                 has_header(c->http_buf, "sec-websocket-key:");
                    if (is_ws) {
                        // Queue WS upgrade response via async write (never block on non-blocking socket)
                        char resp[512];
                        int n = ws_upgrade(c->http_buf, resp, sizeof(resp));
                        if (n > 0) {
                            c->wbuf = (uint8_t*)malloc(n);
                            if (c->wbuf) {
                                memcpy(c->wbuf, resp, n);
                                c->wlen = n;
                                c->woff = 0;
                                c->after_write = WebClient::WS_ACTIVE;
                                c->state = WebClient::WRITE_RESP;
                                c->ws_first = true;
                                c->ws_seq = 0;
                                c->ws_slot = -1;
                                c->ws_last_rx = now_us();
                                if (g_verbose) std::puts("[web] WS upgrade queued");
                            } else {
                                c->state = WebClient::CLOSED;
                            }
                        } else {
                            if (g_verbose) std::puts("[web] WS upgrade failed");
                            c->state = WebClient::CLOSED;
                        }
                    } else {
                        // Build HTTP response and queue it for non-blocking write
                        const char *body = nullptr;
                        size_t body_len = 0;
                        int status = 200;
                        const char *status_str = "OK";

                        if (mode == WebServerMode::WebSocketOnly) {
                            body = "WebSocket only\n";
                            body_len = strlen(body);
                            status = 404;
                            status_str = "Not Found";
                        } else if (req_match(c->http_buf, "GET / ") ||
                                   req_match(c->http_buf, "GET /index.html ")) {
                            body = (const char*)index_html;
                            body_len = index_html_len - 1;
                        } else if (req_match(c->http_buf, "GET /mobile.html ")) {
                            body = (const char*)mobile_html;
                            body_len = mobile_html_len - 1;
                        } else if (req_match(c->http_buf, "GET /editor.html ")) {
                            body = (const char*)editor_html;
                            body_len = editor_html_len - 1;
                        } else {
                            body = "Not Found";
                            body_len = 9;
                            status = 404;
                            status_str = "Not Found";
                        }

                        if (!body) { c->state = WebClient::CLOSED; continue; }

                        char hdr[512];
                        int hdr_len = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 %d %s\r\n"
                            "Content-Type: text/html; charset=utf-8\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n"
                            "Cache-Control: no-cache\r\n"
                            "\r\n", status, status_str, body_len);

                        c->wbuf = (uint8_t*)malloc(hdr_len + body_len);
                        memcpy(c->wbuf, hdr, hdr_len);
                        memcpy(c->wbuf + hdr_len, body, body_len);
                        c->wlen = hdr_len + body_len;
                        c->woff = 0;
                        c->after_write = WebClient::CLOSED;
                        c->state = WebClient::WRITE_RESP;

                        if (g_verbose) std::printf("[web] HTTP %d queued (%zu bytes)\n", status, c->wlen);
                    }
                }
                continue;
            }

            // ── WS_ACTIVE: process WebSocket frames ─────────────────────────
            if (c->state == WebClient::WS_ACTIVE) {
                size_t used;
                do {
                    used = process_ws_frame(c);
                    if (used > 0) {
                        memmove(c->buf, c->buf + used, c->fill - used);
                        c->fill -= used;
                    }
                } while (used > 0 && c->state == WebClient::WS_ACTIVE);
            }
        }

        // ── Cleanup closed clients ────────────────────────────────────────
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state == WebClient::CLOSED && clients[i].fd >= 0) {
                free(clients[i].wbuf);
                clients[i].wbuf = nullptr;
                if (clients[i].ws_slot >= 0) {
                    std::lock_guard<std::mutex> lk(g_mtx[clients[i].ws_slot]);
                    if (g_clients[clients[i].ws_slot].last_rx_us == clients[i].ws_last_rx) {
                        g_clients[clients[i].ws_slot].active = false;
                        g_clients[clients[i].ws_slot].report.reset();
                        clear_all_motion(g_clients[clients[i].ws_slot]);
                        g_clients[clients[i].ws_slot].uses_pad_presence = false;
                        for (int s = 0; s < 4; ++s) {
                            g_clients[clients[i].ws_slot].pad_present[s] = false;
                            g_clients[clients[i].ws_slot].pad_last_present_us[s] = 0;
                        }
                    }
                }
                if (g_verbose) std::printf("[web] client %d closed\n", clients[i].fd);
                close(clients[i].fd);
                clients[i].fd = -1;
                pfds[i+1].fd = -1;
            }
        }

        // Shrink n_clients
        while (n_clients > 0 && pfds[n_clients].fd == -1)
            n_clients--;
    }

    // Final cleanup
    for (int i = 0; i < n_clients; i++) {
        if (clients[i].fd >= 0) {
            free(clients[i].wbuf);
            if (clients[i].ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_mtx[clients[i].ws_slot]);
                if (g_clients[clients[i].ws_slot].last_rx_us == clients[i].ws_last_rx) {
                    g_clients[clients[i].ws_slot].active = false;
                    g_clients[clients[i].ws_slot].report.reset();
                    clear_all_motion(g_clients[clients[i].ws_slot]);
                    g_clients[clients[i].ws_slot].uses_pad_presence = false;
                    clear_udp_rumble_state(g_clients[clients[i].ws_slot]);
                    for (int s = 0; s < 4; ++s) {
                        g_clients[clients[i].ws_slot].pad_present[s] = false;
                        g_clients[clients[i].ws_slot].pad_last_present_us[s] = 0;
                    }
                }
            }
            close(clients[i].fd);
        }
    }
    close(srv);
    std::printf("[web] server stopped\n");
}


// ══════════════════════════════════════════════════════════════════════════════
// ── UDP receive loop (main thread) ────────────────────────────────────────────
static bool parse_bind_arg(const std::string& raw, std::string& bind_addr, uint16_t& port) {
    if (raw.empty()) return false;

    uint32_t numeric_port = 0;
    if (ns::macro::parse_uint32_strict(raw, numeric_port)) {
        if (numeric_port > 65535) return false;
        bind_addr = "0.0.0.0";
        port = (uint16_t)numeric_port;
        return true;
    }

    std::string addr = raw;
    size_t sep = raw.rfind(':');
    if (sep != std::string::npos) {
        std::string port_text = raw.substr(sep + 1);
        uint32_t parsed_port = 0;
        if (!ns::macro::parse_uint32_strict(port_text, parsed_port) || parsed_port > 65535)
            return false;

        addr = raw.substr(0, sep);
        port = (uint16_t)parsed_port;
    }

    if (addr.empty()) addr = "0.0.0.0";
    bind_addr = addr;
    return true;
}

int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
    std::string bind_addr = "0.0.0.0";
    bool        do_upnp   = false;
    int         web_port  = 8080; // WebSocket server is enabled by default
    WebServerMode web_mode = WebServerMode::WebSocketOnly;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "-p") {
            std::fprintf(stderr, "error: -p was removed; use -b PORT or -b ADDR:PORT instead\n");
            return 1;
        }
        else if (a == "-b") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -b requires ADDR, PORT, or ADDR:PORT\n");
                return 1;
            }
            if (!parse_bind_arg(argv[++i], bind_addr, port)) {
                std::fprintf(stderr, "error: invalid bind value; use -b ADDR, -b PORT, or -b ADDR:PORT\n");
                return 1;
            }
        }
        else if (a == "-v")               g_verbose  = true;
        else if (a == "-wake")           g_switch2_run_wake_setup = true;
        else if (a == "--wakeup-config") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --wakeup-config requires a path\n");
                return 1;
            }
            g_switch2_wake_config_path = argv[++i];
        }
        else if (a == "-hori")          g_legacy_mode = true;
        else if (a == "--no-switch2-wake-adv" || a == "--no-switch2-wakeup") g_switch2_wake_adv_enabled = false;
        else if (a == "--upnp")           do_upnp    = true;
        else if (a == "-w") {
            web_mode = WebServerMode::WebApp;
            if (i+1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
                web_port = std::atoi(argv[++i]);
            else
                web_port = 8080;
        }
        else if (a == "-h") {
            puts("ns-backend  [-b ADDR[:PORT]|PORT] [--upnp] [-w [WEB_PORT]] [-v] [-wake] [-hori]");
            puts("");
            puts("  By default, UDP and WebSocket input are both enabled.");
            puts("  WebSocket listens on port 8080 and does not serve the browser webapp.");
            puts("");
            puts("  -b ADDR[:PORT]  Bind UDP to an address and optional port.");
            puts("  -b PORT         Keep 0.0.0.0 with a custom UDP port.");
            puts("  -w [PORT]       Serve the browser webapp too, using this port or 8080.");
            puts("  --upnp          Forward the UDP port via UPnP for PC clients only.");
            puts("                  Mobile/web clients connect via WebSocket and don't need this.");
            puts("  -wake           Capture Joy-Con 2 wake MAC/advert, save switch2_wakeup.conf, then exit.");
            puts("  --wakeup-config PATH");
            puts("                  Wake config path. Default: /etc/ns-pc-control/switch2_wakeup.conf");
            puts("  --no-switch2-wakeup");
            puts("                  Disable Switch 2 Joy-Con 2 BLE wake advertisement bursts.");
            puts("  -hori           Expose the legacy 8-byte HORI controller gadget.");
            puts("                  Default mode exposes the 64-byte motion/rumble gadget.");
            puts("");
            return 0;
        }
        else {
            std::fprintf(stderr, "error: unknown argument: %s\n", a.c_str());
            return 1;
        }
    }

    if (g_switch2_run_wake_setup)
        return run_switch2_wakeup_setup();

    if (g_switch2_wake_adv_enabled)
        load_switch2_wakeup_config(true);

    randomize_controller_identity();

    // Always recreate the built-in gadget at process startup.  This makes every
    // launch self-healing: stale configfs state, leftover /dev/hidg* nodes, or a
    // previous unclean shutdown are cleared before the backend starts talking to
    // the console.
    run_gadget_setup_if_needed(true, "startup gadget recreation requested");

    derive_key(DEFAULT_SECRET, g_hmac_key);
    signal(SIGINT,  on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (do_upnp) upnp_add_mapping(port);

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int rbuf = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "error: invalid IPv4 bind address: %s\n", bind_addr.c_str());
        close(sock);
        return 1;
    }
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }

    // Start WebSocket server only after UDP socket is up.
    std::thread web_thread;
    if (web_port > 0)
        web_thread = std::thread(web_server_thread, web_port, port, web_mode);
    
    std::printf("UDP %s:%u writer=%d Hz mode=%s\n",
                bind_addr.c_str(), port, PRO_WRITER_HZ,
                g_legacy_mode ? "hori" : "modern");
    std::thread wt(writer_thread, PRO_WRITER_HZ);
    std::thread st(stats_thread);

    int ep = epoll_create1(0); epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = sock; epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);

    std::vector<uint8_t> udp_rx(std::max(UDP_RX_MAX_PACKET_SIZE, ns::macro::CHUNK_HEADER_SIZE + ns::macro::UDP_CHUNK_MAX + HMAC_TAG_SIZE));
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 4, 200);
        if (n <= 0) continue;

        sockaddr_in sender{};
        socklen_t slen;
        ssize_t bytes;

        // Drain all available packets from the kernel buffer.
        // Two UDP packet formats are accepted:
        //   1) legacy Packet: input only, unchanged, authenticated with HMAC.
        //   2) ExtendedUdpPacket: ExtendedMultiReport with motion/gyro, authenticated
        //      with HMAC, and opted into UDP rumble replies.
        while (g_running.load(std::memory_order_relaxed)) {
            slen = sizeof(sender);
            bytes = recvfrom(sock, udp_rx.data(), udp_rx.size(), 0, (sockaddr*)&sender, &slen);
            if (bytes <= 0) break; // EAGAIN or error — ring is drained

            if (bytes == (ssize_t)sizeof(ServerInfoProbe)) {
                ServerInfoProbe probe{};
                memcpy(&probe, udp_rx.data(), sizeof(probe));
                if (probe.magic == SERVER_INFO_MAGIC && probe.version == SERVER_INFO_VERSION) {
                    ServerInfoReply reply{};
                    reply.backend = g_legacy_mode ? SERVER_BACKEND_LEGACY : SERVER_BACKEND_PRO;
                    reply.udp_interval_ms = g_legacy_mode ? LEGACY_UDP_INTERVAL_MS : PRO_UDP_INTERVAL_MS;
                    reply.udp_hz = g_legacy_mode ? LEGACY_UDP_HZ : PRO_UDP_HZ;
                    sendto(sock, &reply, sizeof(reply), 0, (sockaddr*)&sender, slen);
                    continue;
                }
            }


            if (bytes >= 4) {
                uint32_t mmagic = 0;
                memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == ns::macro::UDP_CHUNK_MAGIC) {
                    server_macro_handle_chunk_packet(udp_rx.data(), (size_t)bytes, sender);
                    continue;
                }
            }

            if (bytes >= (ssize_t)(ns::macro::UDP_HEADER_SIZE + HMAC_TAG_SIZE)) {
                uint32_t mmagic = 0;
                memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == ns::macro::UDP_MAGIC) {
                    ns::macro::MacroUdpHeaderWire mh{};
                    memcpy(&mh, udp_rx.data(), sizeof(mh));
                    uint32_t text_len = mh.text_len;
                    if (text_len <= ns::macro::UDP_TEXT_MAX && bytes == (ssize_t)(ns::macro::UDP_HEADER_SIZE + text_len + HMAC_TAG_SIZE)) {
                        const uint8_t* recv_hmac = udp_rx.data() + ns::macro::UDP_HEADER_SIZE + text_len;
                        if (hmac_verify(g_hmac_key, 32, udp_rx.data(), ns::macro::UDP_HEADER_SIZE + text_len, recv_hmac, HMAC_TAG_SIZE) == 0) {
                            if (!rate_allow(sender.sin_addr.s_addr)) continue;
                            int client_idx = server_macro_client_for_sender(sender);
                            if (client_idx >= 0) {
                                {
                                    std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
                                    g_clients[client_idx].uses_pad_presence = true;
                                    int sp = mh.subpad < 4 ? mh.subpad : 0;
                                    g_clients[client_idx].pad_present[sp] = true;
                                    g_clients[client_idx].pad_last_present_us[sp] = now_us();
                                }
                                std::string text(reinterpret_cast<char*>(udp_rx.data() + ns::macro::UDP_HEADER_SIZE), text_len);
                                server_macro_start(client_idx, mh.subpad < 4 ? mh.subpad : 0, text);
                            }
                        } else if (g_verbose) puts("bad macro HMAC, dropped");
                    } else if (g_verbose) puts("bad macro packet size, dropped");
                    continue;
                }
            }

            bool is_extended_udp = false;
            bool is_extended_udp3 = false;
            Packet pkt{};
            ExtendedUdpPacket ext_pkt{};
            ExtendedUdpPacket3 ext3_pkt{};

            if (bytes == (ssize_t)PACKET_SIZE) {
                memcpy(&pkt, udp_rx.data(), sizeof(pkt));
            } else if (bytes == (ssize_t)EXT_UDP_PACKET_SIZE) {
                memcpy(&ext_pkt, udp_rx.data(), sizeof(ext_pkt));
                is_extended_udp = true;
            } else if (bytes == (ssize_t)EXT3_UDP_PACKET_SIZE) {
                memcpy(&ext3_pkt, udp_rx.data(), sizeof(ext3_pkt));
                is_extended_udp3 = true;
            } else {
                if (g_verbose) std::printf("[udp] unexpected packet size=%zd, dropped\n", bytes);
                continue;
            }

            // ── 1. Per-IP rate limiter ────────────────────────────────────────────
            uint32_t src_ip = sender.sin_addr.s_addr;
            if (!rate_allow(src_ip)) {
                if (g_verbose) puts("rate limit exceeded, dropped");
                continue;
            }

            // ── 2. Magic + version check ──────────────────────────────────────────
            if (is_extended_udp) {
                if (!extended_udp_packet_ok(ext_pkt)) {
                    if (g_verbose) puts("bad extended UDP magic/version, dropped");
                    continue;
                }
            } else if (is_extended_udp3) {
                if (!extended_udp3_packet_ok(ext3_pkt)) {
                    if (g_verbose) puts("bad extended UDP v3 magic/version, dropped");
                    continue;
                }
            } else if (!packet_ok(pkt)) {
                if (g_verbose) puts("bad magic/version, dropped");
                continue;
            }

            // ── 3. Find Client Session or Pin new IP:port ─────────────────────────
            int client_idx = -1;
            uint64_t now = now_us();
            bool wake_on_new_client = false;

            for (int i = 0; i < MAX_CLIENTS; ++i) {
                std::lock_guard<std::mutex> lk(g_mtx[i]);
                if (g_clients[i].active &&
                    g_clients[i].addr.sin_addr.s_addr == src_ip &&
                    g_clients[i].addr.sin_port == sender.sin_port) {
                    client_idx = i;
                    break;
                }
            }

            // If not found, assign to a free/timed-out slot.
            if (client_idx == -1) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_mtx[i]);
                    if (!g_clients[i].active || elapsed_us_over(now, g_clients[i].last_rx_us, CLIENT_TIMEOUT_US)) {
                        client_idx = i;
                        g_clients[i].active = true;
                        g_clients[i].addr = sender;
                        g_clients[i].first_pkt = true;
                        g_clients[i].expected_seq = 0;
                        g_clients[i].report.reset();
                        clear_all_motion(g_clients[i]);
                        g_clients[i].uses_pad_presence = false;
                        clear_udp_rumble_state(g_clients[i]);
                        for (int s = 0; s < 4; ++s) {
                            g_clients[i].pad_present[s] = false;
                            g_clients[i].pad_last_present_us[s] = 0;
                        }
                        g_clients[i].last_rx_us = now;
                        wake_on_new_client = true;
                        if (g_verbose) std::printf("New UDP client accepted into Server Slot %d/4\n", i+1);
                        break;
                    }
                }
            }

            if (wake_on_new_client)
                maybe_send_switch2_wake_advert("client connected via UDP input");

            // If all 4 slots are taken by active PCs, drop the packet.
            if (client_idx == -1) {
                if (g_verbose) puts("server is full (4 PCs already active), dropped");
                continue;
            }

            // ── 4. HMAC authentication ────────────────────────────────────────────
            int hmac_ok = 0;
            if (is_extended_udp) {
                hmac_ok = hmac_verify(g_hmac_key, 32,
                                      reinterpret_cast<const uint8_t*>(&ext_pkt),
                                      EXT_UDP_PACKET_AUTH_SIZE,
                                      ext_pkt.hmac,
                                      HMAC_TAG_SIZE);
            } else if (is_extended_udp3) {
                hmac_ok = hmac_verify(g_hmac_key, 32,
                                      reinterpret_cast<const uint8_t*>(&ext3_pkt),
                                      EXT3_UDP_PACKET_AUTH_SIZE,
                                      ext3_pkt.hmac,
                                      HMAC_TAG_SIZE);
            } else {
                hmac_ok = hmac_verify(g_hmac_key, 32,
                                      reinterpret_cast<const uint8_t*>(&pkt),
                                      PACKET_AUTH_SIZE,
                                      pkt.hmac,
                                      HMAC_TAG_SIZE);
            }
            if (hmac_ok != 0) {
                if (g_verbose) puts("bad HMAC, dropped");
                continue;
            }

            uint8_t packet_flags = is_extended_udp3 ? ext3_pkt.flags : (is_extended_udp ? ext_pkt.flags : pkt.flags);
            if (packet_flags & FLAG_DISCONNECT) {
                server_macro_stop_all_for_client(client_idx);
                {
                    std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
                    reset_udp_client_session_locked(g_clients[client_idx]);
                }
                if (g_verbose) std::printf("UDP client %d sent disconnect and was released.\n", client_idx + 1);
                ++g_pkts_rx;
                continue;
            }

            // ── 5. Sequence counter + Apply to shared state ───────────────────────
            bool accepted = false;
            {
                std::lock_guard<std::mutex> lk(g_mtx[client_idx]);

                // Re-validate: writer may have deactivated the slot during HMAC.
                if (!g_clients[client_idx].active) continue;

                uint8_t flags = is_extended_udp3 ? ext3_pkt.flags : (is_extended_udp ? ext_pkt.flags : pkt.flags);
                uint32_t seq = is_extended_udp3 ? ext3_pkt.seq : (is_extended_udp ? ext_pkt.seq : pkt.seq);
                bool is_reset = (flags & FLAG_RESET);
                bool sequence_jump = (g_clients[client_idx].expected_seq > seq) &&
                                     ((g_clients[client_idx].expected_seq - seq) > 100);

                if (!g_clients[client_idx].first_pkt && seq < g_clients[client_idx].expected_seq && !is_reset && !sequence_jump) {
                    if (g_verbose)
                        std::printf("UDP client %d out-of-order seq=%u, dropped\n", client_idx+1, seq);
                    continue;
                }
                g_clients[client_idx].first_pkt = false;
                g_clients[client_idx].expected_seq = seq + 1;

                if (is_reset) {
                    g_clients[client_idx].report.reset();
                    clear_all_motion(g_clients[client_idx]);
                    for (int s = 0; s < 4; ++s) {
                        g_clients[client_idx].pad_present[s] = false;
                        g_clients[client_idx].pad_last_present_us[s] = 0;
                    }
                } else if (is_extended_udp || is_extended_udp3) {
                    // Extended UDP carries motion/gyro and pad-present flags, so
                    // neutral-but-connected pads can still receive rumble.  Version 6
                    // also carries the three Pro-controller IMU samples explicitly.
                    g_clients[client_idx].uses_pad_presence = true;
                    enable_udp_rumble_state(g_clients[client_idx]);

                    ExtendedHIDReport* dst_pads[4] = {
                        &g_clients[client_idx].report.p1,
                        &g_clients[client_idx].report.p2,
                        &g_clients[client_idx].report.p3,
                        &g_clients[client_idx].report.p4,
                    };
                    const ExtendedHIDReport* src_pads[4] = {
                        &ext_pkt.report.p1, &ext_pkt.report.p2,
                        &ext_pkt.report.p3, &ext_pkt.report.p4,
                    };
                    const ExtendedHIDReport3* src_pads3[4] = {
                        &ext3_pkt.report.p1, &ext3_pkt.report.p2,
                        &ext3_pkt.report.p3, &ext3_pkt.report.p4,
                    };

                    for (int s = 0; s < 4; ++s) {
                        bool present = is_extended_udp3 ?
                            extended3_report_pad_present(ext3_pkt.report, s) :
                            extended_report_pad_present(ext_pkt.report, s);
                        if (present) {
                            if (is_extended_udp3) {
                                extended3_to_extended_latest(*src_pads3[s], *dst_pads[s]);
                                if (src_pads3[s]->has_motion)
                                    set_motion_samples(g_clients[client_idx], s, src_pads3[s]->motion);
                                else
                                    clear_motion(g_clients[client_idx], s);
                            } else {
                                *dst_pads[s] = *src_pads[s];
                                if (src_pads[s]->has_motion)
                                    set_motion(g_clients[client_idx], s, src_pads[s]->motion);
                                else
                                    clear_motion(g_clients[client_idx], s);
                            }
                            g_clients[client_idx].pad_present[s] = true;
                            g_clients[client_idx].pad_last_present_us[s] = now;
                        } else {
                            g_clients[client_idx].pad_present[s] = false;
                            uint64_t last_seen = g_clients[client_idx].pad_last_present_us[s];
                            if (last_seen == 0 || now - last_seen >= WEB_PAD_ABSENT_RELEASE_US) {
                                dst_pads[s]->reset();
                                clear_motion(g_clients[client_idx], s);
                            }
                        }
                    }
                } else {
                    // Legacy UDP remains 100% compatible: input-only, no pad-present
                    // tracking, no gyro, and no UDP rumble replies.
                    g_clients[client_idx].uses_pad_presence = false;
                    clear_udp_rumble_state(g_clients[client_idx]);
                    for (int s = 0; s < 4; ++s) {
                        g_clients[client_idx].pad_present[s] = false;
                        g_clients[client_idx].pad_last_present_us[s] = 0;
                    }
                    clear_all_motion(g_clients[client_idx]);
                    legacy_multi_to_extended(pkt.report, g_clients[client_idx].report);
                }

                g_clients[client_idx].last_rx_us = now_us();
                accepted = true;
            }

            if (!accepted) continue;
            ++g_pkts_rx;

            // Extended UDP clients opted into rumble by using the new packet
            // format.  Legacy clients are not sent unexpected traffic.
            if (is_extended_udp || is_extended_udp3) {
                flush_rumble_to_udp(sock, client_idx);
            }
        } // drain loop
    } // epoll loop

    puts("[backend] shutting down");
    upnp_remove_mapping(port);
    close(ep); close(sock);
    wt.join(); st.join();
    if (web_thread.joinable()) web_thread.join();

    teardown_gadget();

    return 0;
}
