#include "../../include/protocol.hpp"
#include "../../include/sha256.h"

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

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

using namespace ns;
using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

// ── Global flags ──────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static bool g_verbose = false;

// Built-in USB gadget lifecycle for the old HORI/Pokken 4-port setup.
// This mirrors setup_gadget.sh: remove our old gadget first, recreate four
// 8-byte HORI HID functions, bind them, chmod /dev/hidg0..3, and tear down
// again on clean shutdown.
static bool g_auto_gadget_setup    = true;
static bool g_teardown_gadget_exit = true;

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
    MultiReport report{}; // The inputs coming from this specific PC
};

static std::mutex    g_mtx[MAX_CLIENTS];
static ClientSession g_clients[MAX_CLIENTS];

#define NS_LOCAL_PACKED __attribute__((packed))

// Modern UDP clients can send an extended packet containing a 24-byte report
// per pad (8-byte HIDReport + 12-byte motion + 4 bytes of flags/padding).
// This HORI backend has no gyro/rumble path, so it only extracts the first
// HIDReport from each extended pad and ignores the rest.
static constexpr uint8_t MODERN_EXT_PROTO_VERSION = 5;
static constexpr uint8_t EXT_PAD_PRESENT = 0x01;

struct NS_LOCAL_PACKED ModernMotionReportWire {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
};
static_assert(sizeof(ModernMotionReportWire) == 12, "ModernMotionReportWire must be 12 bytes");

struct NS_LOCAL_PACKED ModernExtendedHIDReportWire {
    HIDReport input;             // byte 0..7; byte 7/vendor also carries PAD_PRESENT
    ModernMotionReportWire motion;
    uint8_t has_motion;
    uint8_t reserved[3];
};
static_assert(sizeof(ModernExtendedHIDReportWire) == 24, "ModernExtendedHIDReportWire must be 24 bytes");

struct NS_LOCAL_PACKED ModernExtendedMultiReportWire {
    ModernExtendedHIDReportWire p1, p2, p3, p4;
};
static_assert(sizeof(ModernExtendedMultiReportWire) == 96, "ModernExtendedMultiReportWire must be 96 bytes");

struct NS_LOCAL_PACKED ModernExtendedUdpPacketWire {
    uint32_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t seq;
    uint64_t timestamp_us;
    ModernExtendedMultiReportWire report;
    uint8_t  hmac[HMAC_TAG_SIZE];
};

static constexpr size_t EXT_UDP_PACKET_AUTH_SIZE = 20 + sizeof(ModernExtendedMultiReportWire);
static constexpr size_t EXT_UDP_PACKET_SIZE      = EXT_UDP_PACKET_AUTH_SIZE + HMAC_TAG_SIZE;
static constexpr size_t UDP_RX_MAX_PACKET_SIZE   =
    EXT_UDP_PACKET_SIZE > PACKET_SIZE ? EXT_UDP_PACKET_SIZE : PACKET_SIZE;
static_assert(sizeof(ModernExtendedUdpPacketWire) == EXT_UDP_PACKET_SIZE,
              "ModernExtendedUdpPacketWire size must match its wire format");

static bool modern_extended_udp_packet_ok(const ModernExtendedUdpPacketWire& p) {
    return p.magic == PROTO_MAGIC &&
           (p.version == PROTO_VERSION || p.version == MODERN_EXT_PROTO_VERSION);
}

static bool modern_pad_present(const ModernExtendedHIDReportWire& p) {
    return (p.input.vendor & EXT_PAD_PRESENT) != 0;
}

static bool hid_report_neutral(const HIDReport& r) {
    return r.buttons == 0 && r.hat == HAT_NEUTRAL &&
           r.lx == 128 && r.ly == 128 && r.rx == 128 && r.ry == 128;
}

static void modern_extended_to_legacy_multi(const ModernExtendedMultiReportWire& in, MultiReport& out) {
    out.reset();
    const ModernExtendedHIDReportWire* src[4] = { &in.p1, &in.p2, &in.p3, &in.p4 };
    HIDReport* dst[4] = { &out.p1, &out.p2, &out.p3, &out.p4 };
    for (int i = 0; i < 4; ++i) {
        // Keep non-neutral input even if a future/buggy client forgets the
        // present flag; otherwise absent neutral pads stay neutral.
        if (modern_pad_present(*src[i]) || !hid_report_neutral(src[i]->input)) {
            *dst[i] = src[i]->input;
            dst[i]->vendor = 0; // hide EXT_PAD_PRESENT from the HORI HID report
        }
    }
}

// Diagnostics
static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};

// ── Signal ────────────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }


// ── Built-in HORI USB gadget setup/teardown ──────────────────────────────────
static constexpr const char* GADGET_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl";
static constexpr const char* CONFIG_DIR = "/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1";

// Standard HORI Pokken Tournament Controller descriptor from setup_gadget.sh.
static const uint8_t HORI_REPORT_DESC[] = {
    0x05,0x01,0x09,0x05,0xA1,0x01,0x15,0x00,0x25,0x01,0x35,0x00,0x45,0x01,0x75,0x01,
    0x95,0x0D,0x05,0x09,0x19,0x01,0x29,0x0D,0x81,0x02,0x95,0x03,0x81,0x01,0x05,0x01,
    0x25,0x07,0x46,0x3B,0x01,0x75,0x04,0x95,0x01,0x65,0x14,0x09,0x39,0x81,0x42,
    0x65,0x00,0x95,0x01,0x81,0x01,0x26,0xFF,0x00,0x46,0xFF,0x00,0x09,0x30,0x09,
    0x31,0x09,0x32,0x09,0x35,0x75,0x08,0x95,0x04,0x81,0x02,0x06,0x00,0xFF,0x09,
    0x20,0x75,0x08,0x95,0x01,0x81,0x02,0xC0
};

static bool hidg_nodes_ready() {
    for (int i = 0; i < 4; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/hidg%d", i);
        if (access(path, R_OK | W_OK) != 0) return false;
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

static std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty() || a.back() == '/') return a + b;
    return a + "/" + b;
}

static void remove_link_if_exists(const char* path) {
    struct stat st{};
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode))
        unlink(path);
}

static void rmdir_if_exists(const char* path) {
    if (rmdir(path) != 0 && errno != ENOENT) {
        if (g_verbose)
            std::fprintf(stderr, "[gadget] rmdir %s failed: %s\n", path, std::strerror(errno));
    }
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

static void teardown_gadget() {
    if (!path_exists(GADGET_DIR)) return;

    std::puts("[gadget] Removing HORI gadget...");
    write_text_file(join_path(GADGET_DIR, "UDC").c_str(), "");

    for (int i = 0; i < 4; ++i) {
        char link_path[320];
        std::snprintf(link_path, sizeof(link_path), "%s/hid.usb%d", CONFIG_DIR, i);
        remove_link_if_exists(link_path);
    }

    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1/strings/0x409");
    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/configs/c.1");

    for (int i = 0; i < 4; ++i) {
        char func[256];
        std::snprintf(func, sizeof(func), "%s/functions/hid.usb%d", GADGET_DIR, i);
        rmdir_if_exists(func);
    }

    rmdir_if_exists("/sys/kernel/config/usb_gadget/ns_ctrl/strings/0x409");
    rmdir_if_exists(GADGET_DIR);
}

static bool create_hori_hid_function(int id) {
    char func[256];
    std::snprintf(func, sizeof(func), "%s/functions/hid.usb%d", GADGET_DIR, id);
    if (!mkdir_if_needed(func)) return false;

    char path[320];
    std::snprintf(path, sizeof(path), "%s/protocol", func);
    if (!write_text_file(path, "0")) return false;

    std::snprintf(path, sizeof(path), "%s/subclass", func);
    if (!write_text_file(path, "0")) return false;

    std::snprintf(path, sizeof(path), "%s/report_length", func);
    if (!write_text_file(path, "8")) return false;

    std::snprintf(path, sizeof(path), "%s/report_desc", func);
    if (!write_bytes_file(path, HORI_REPORT_DESC, sizeof(HORI_REPORT_DESC))) return false;

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

static bool setup_hori_gadget_builtin(bool force, const char* reason) {
    if (!g_auto_gadget_setup) return hidg_nodes_ready();

    if (geteuid() != 0) {
        if (hidg_nodes_ready()) return true;
        std::fprintf(stderr,
            "[gadget] /dev/hidg0..3 are not ready and built-in HORI setup needs root.\n"
            "[gadget] Run: sudo ./ns-backend ...\n");
        return false;
    }

    std::printf("[gadget] %s; creating built-in 4-player HORI gadget\n",
                reason ? reason : "Preparing HORI gadget");

    std::system("modprobe libcomposite >/dev/null 2>&1 || true");
    std::system("mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config >/dev/null 2>&1 || true");

    if (!dir_exists("/sys/kernel/config/usb_gadget")) {
        std::fprintf(stderr,
            "[gadget] /sys/kernel/config/usb_gadget is unavailable.\n"
            "[gadget] Check libcomposite/configfs and dtoverlay=dwc2.\n");
        return false;
    }

    if (force || path_exists(GADGET_DIR)) {
        teardown_gadget();
        std::this_thread::sleep_for(ms(300));
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
    if (!mkdir_if_needed(join_path(configs_dir, "strings").c_str())) return false;
    if (!mkdir_if_needed(config_strings_dir.c_str())) return false;
    if (!mkdir_if_needed(functions_dir.c_str())) return false;

    if (!write_text_file(join_path(GADGET_DIR, "bcdDevice").c_str(), "0x0200")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bcdUSB").c_str(), "0x0200")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "idVendor").c_str(), "0x0F0D")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "idProduct").c_str(), "0x0092")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceClass").c_str(), "0xFF")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceSubClass").c_str(), "0xFF")) return false;
    if (!write_text_file(join_path(GADGET_DIR, "bDeviceProtocol").c_str(), "0xFF")) return false;

    if (!write_text_file(join_path(strings_dir, "serialnumber").c_str(), "000000000001")) return false;
    if (!write_text_file(join_path(strings_dir, "manufacturer").c_str(), "HORI CO., LTD.")) return false;
    if (!write_text_file(join_path(strings_dir, "product").c_str(), "POKKEN CONTROLLER")) return false;
    if (!write_text_file(join_path(configs_dir, "MaxPower").c_str(), "500")) return false;
    if (!write_text_file(join_path(config_strings_dir, "configuration").c_str(), "Switch 4-Player Hub Config")) return false;

    for (int i = 0; i < 4; ++i) {
        if (!create_hori_hid_function(i)) return false;
    }

    std::string udc = first_udc_name();
    if (udc.empty()) {
        std::fprintf(stderr,
            "[gadget] No UDC found. Check dtoverlay=dwc2 in /boot/config.txt.\n");
        return false;
    }

    if (!write_text_file(join_path(GADGET_DIR, "UDC").c_str(), udc.c_str())) return false;
    std::printf("[gadget] Bound to UDC: %s\n", udc.c_str());

    for (int tries = 0; tries < 20; ++tries) {
        bool all_seen = true;
        for (int i = 0; i < 4; ++i) {
            char path[32];
            std::snprintf(path, sizeof(path), "/dev/hidg%d", i);
            if (access(path, F_OK) != 0) all_seen = false;
            chmod(path, 0666);
        }
        if (all_seen && hidg_nodes_ready()) {
            std::puts("[gadget] Done. Exposed /dev/hidg0 to /dev/hidg3 (HORI/Pokken)");
            return true;
        }
        std::this_thread::sleep_for(ms(100));
    }

    std::fprintf(stderr, "[gadget] setup finished, but /dev/hidg0..3 are still not ready.\n");
    return false;
}


// ── Smart Multiplexer HID Writer Thread ───────────────────────────────────────
static void writer_thread(int hz) {
    const auto tick = us(1'000'000 / hz);
    int fds[4] = {-1, -1, -1, -1};
    std::string devs[4] = {"/dev/hidg0", "/dev/hidg1", "/dev/hidg2", "/dev/hidg3"};
    bool was_connected = false;

    // Tracks which physical Switch port is claimed by which (Client, SubController)
    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[4];

    auto is_neutral = [](const HIDReport& r) {
        return r.buttons == 0 && r.hat == 8 && r.lx == 128 && r.ly == 128 && r.rx == 128 && r.ry == 128;
    };

    while (g_running.load(std::memory_order_relaxed)) {
        bool all_open = true;
        for(int i=0; i<4; ++i) {
            if (fds[i] < 0) {
                fds[i] = open(devs[i].c_str(), O_WRONLY);
                if (fds[i] < 0) all_open = false;
            }
        }

        if (!all_open) {
            std::this_thread::sleep_for(ms(500));
            continue;
        }
        
        if (g_verbose || !was_connected)
            std::puts("4x /dev/hidg* opened");
        was_connected = true;

        auto next = Clock::now() + tick;
        MultiReport prev{}; prev.p1.buttons = 0xFFFF; // Force first write
        bool error_shown = false;
        bool timeout_printed[MAX_CLIENTS] = {};

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now(); next = std::max(next + tick, now + tick);

            MultiReport r;
            r.reset(); // Base neutral state

            // Per-slot snapshot (each slot locked independently)
            uint64_t now_stamp = now_us();
            bool active_snap[MAX_CLIENTS];
            HIDReport report_snap[MAX_CLIENTS][4];

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                std::lock_guard<std::mutex> lk(g_mtx[c]);
                if (g_clients[c].active && (now_stamp - g_clients[c].last_rx_us > WATCHDOG_MS * 1000ULL)) {
                    g_clients[c].active = false;
                    if (g_verbose && !timeout_printed[c]) {
                        std::printf("PC %d timed out and was disconnected.\n", c+1);
                        timeout_printed[c] = true;
                    }
                } else if (g_clients[c].active) {
                    timeout_printed[c] = false; // reset flag if client recovers
                }
                active_snap[c] = g_clients[c].active;
                HIDReport* src[4] = { &g_clients[c].report.p1, &g_clients[c].report.p2,
                                       &g_clients[c].report.p3, &g_clients[c].report.p4 };
                for (int s = 0; s < 4; ++s)
                    report_snap[c][s] = *src[s];
            }

            // 2. Free hardware slots mapped to inactive clients (snapshot, no lock needed)
            for (int h = 0; h < 4; ++h) {
                if (hw_slots[h].client_idx != -1 && !active_snap[hw_slots[h].client_idx]) {
                    hw_slots[h].client_idx = -1;
                    hw_slots[h].sub_idx = -1;
                }
            }

            // 3. Auto-assign unmapped active inputs to free hardware slots
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!active_snap[c]) continue;

                HIDReport* subs[4] = { &report_snap[c][0], &report_snap[c][1],
                                        &report_snap[c][2], &report_snap[c][3] };

                for (int s = 0; s < 4; ++s) {
                    bool mapped = false;
                    for (int h = 0; h < 4; ++h) {
                        if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                            mapped = true; break;
                        }
                    }

                    if (!mapped && !is_neutral(*subs[s])) {
                        for (int h = 0; h < 4; ++h) {
                            if (hw_slots[h].client_idx == -1) {
                                hw_slots[h].client_idx = c;
                                hw_slots[h].sub_idx = s;
                                if (g_verbose)
                                    std::printf("Map -> PC %d (Pad %d) took Switch Port %d\n", c+1, s+1, h+1);
                                break;
                            }
                        }
                    }
                }
            }

            // 4. Construct the final report from snapshot
            HIDReport* out_subs[4] = { &r.p1, &r.p2, &r.p3, &r.p4 };
            for (int h = 0; h < 4; ++h) {
                if (hw_slots[h].client_idx != -1) {
                    int c = hw_slots[h].client_idx;
                    int s = hw_slots[h].sub_idx;
                    *out_subs[h] = report_snap[c][s];
                }
            }

            // 5. Send to physical USB gadget drivers efficiently
            bool ok = true;
            static_assert(sizeof(HIDReport) == 8, "HIDReport size mismatch with HID gadget descriptor");
            if (r.p1 != prev.p1) { if(write(fds[0], &r.p1, sizeof(HIDReport)) < 0 && errno != EAGAIN) ok = false; }
            if (r.p2 != prev.p2) { if(write(fds[1], &r.p2, sizeof(HIDReport)) < 0 && errno != EAGAIN) ok = false; }
            if (r.p3 != prev.p3) { if(write(fds[2], &r.p3, sizeof(HIDReport)) < 0 && errno != EAGAIN) ok = false; }
            if (r.p4 != prev.p4) { if(write(fds[3], &r.p4, sizeof(HIDReport)) < 0 && errno != EAGAIN) ok = false; }

            if (!ok) {
                if (!error_shown) { std::puts("Switch disconnected — waiting for reconnect..."); error_shown = true; }
                for(int i=0; i<4; ++i) { close(fds[i]); fds[i] = -1; }
                std::this_thread::sleep_for(ms(1000)); break;
            }
            prev = r;
            ++g_hid_writes;
        }
    }
    
    // Shutdown securely by neutralizing all ports
    MultiReport neutral{}; neutral.reset();
    for(int i=0; i<4; ++i) { 
        if (fds[i] >= 0) { ssize_t _u = write(fds[i], &neutral.p1, sizeof(HIDReport)); (void)_u; close(fds[i]); }
    }
}


// ── Per-IP rate limiter ──────────────────────────────────────────────────────
static std::mutex g_rate_mtx;

// ── Stats thread ──────────────────────────────────────────────────────────────
static void stats_thread() {
    uint64_t last_cleanup = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(ms(5000));

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

static const char INDEX_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover\">\n"
    "    <title>NS Web Control</title>\n"
    "    <style>\n"
    "        body {\n"
    "            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;\n"
    "            background-color: #FFFFFF;\n"
    "            color: #1A1A1A;\n"
    "            padding: 20px;\n"
    "            max-width: 480px;\n"
    "        }\n"
    "        h2 { color: #CC0000; margin-top: 0; font-size: 20px; }\n"
    "        .row { display: flex; align-items: center; margin-bottom: 12px; }\n"
    "        .row label { width: 140px; text-align: right; margin-right: 10px; font-size: 14px; }\n"
    "        input[type=\"text\"], select {\n"
    "            flex: 1; padding: 4px; font-family: 'Consolas', monospace; border: 1px solid #ccc;\n"
    "        }\n"
    "        button {\n"
    "            padding: 6px 16px; font-family: 'Segoe UI'; font-size: 14px;\n"
    "            background: #f0f0f0; border: 1px solid #ccc; cursor: pointer;\n"
    "            border-radius: 4px;\n"
    "            min-height: 30px;\n"
    "        }\n"
    "        button:hover { background: #e0e0e0; }\n"
    "        .btn-group { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 15px; }\n"
    "        hr { border: none; border-top: 2px solid #DDDDDD; margin: 20px 0; }\n"
    "        .status { font-size: 13px; margin-bottom: 4px; }\n"
    "        \n"
    "        #modalOverlay {\n"
    "            display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%;\n"
    "            background: rgba(0,0,0,0.5); justify-content: center; align-items: center;\n"
    "            z-index: 10000;\n"
    "        }\n"
    "        #modalContent {\n"
    "            background: white; padding: 20px; border-radius: 8px; width: 90%; max-width: 440px;\n"
    "            max-height: 80vh; display: flex; flex-direction: column; box-shadow: 0 4px 12px rgba(0,0,0,0.2);\n"
    "        }\n"
    "        #modalContent h3 { margin-top: 0; margin-bottom: 15px; color: #CC0000; flex-shrink: 0; }\n"
    "        \n"
    "        #bindingsScrollArea {\n"
    "            overflow-y: auto; flex-grow: 1; padding-right: 10px;\n"
    "        }\n"
    "        \n"
    "        .bind-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; font-size: 13px; }\n"
    "        .bind-btn { width: 120px; font-family: 'Consolas'; font-size: 12px; transition: background 0.2s; }\n"
    "        .listening { background: #ffeb3b !important; border-color: #fbc02d !important; color: #000; transform: scale(1.02); }\n"
    "        \n"
    "        .modal-footer {\n"
    "            display: flex; justify-content: space-between; margin-top: 15px; padding-top: 15px; border-top: 1px solid #eee; flex-shrink: 0;\n"
    "        }\n"
    "        .footer-group { display: flex; gap: 8px; }\n"
    "        \n"
    "        @media (max-width: 480px) {\n"
    "            .row { flex-direction: column; align-items: flex-start; }\n"
    "            .row label { width: auto; text-align: left; margin-bottom: 5px; }\n"
    "            select, button { padding: 10px; min-height: 44px; }\n"
    "        }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "    <h2>NS Web Control</h2>\n"
    "    <div class=\"row\" id=\"kbModeContainer\">\n"
    "        <label>Keyboard Mode:</label>\n"
    "        <select id=\"kbMode\">\n"
    "            <option value=\"0\">OFF</option>\n"
    "            <option value=\"1\">ON (single)</option>\n"
    "            <option value=\"2\">ON (override)</option>\n"
    "        </select>\n"
    "    </div>\n"
    "    <div class=\"btn-group\">\n"
    "        <button id=\"btnConnect\">Connect</button>\n"
    "        <button id=\"btnBindings\">Bindings...</button>\n"
    "        <button id=\"btnTouchControls\" style=\"display: none; background: #e3f2fd; border-color: #90caf9;\" onclick=\"window.location.href='/mobile'\">Touch Controls</button>\n"
    "        <button id=\"btnEditor\" style=\"display: none; background: #fff9c4; border-color: #ffe082;\" onclick=\"window.location.href='/editor'\">Editor</button>\n"
    "    </div>\n"
    "    <hr>\n"
    "    <div class=\"status\" id=\"statusText\">Ready</div>\n"
    "    <div class=\"status\" id=\"p1Text\">P1: Idle</div>\n"
    "    <div class=\"status\" id=\"p2Text\">P2: Not connected</div>\n"
    "    <div class=\"status\" id=\"p3Text\">P3: Not connected</div>\n"
    "    <div class=\"status\" id=\"p4Text\">P4: Not connected</div>\n"
    "    <div id=\"modalOverlay\">\n"
    "        <div id=\"modalContent\">\n"
    "            <h3>Keyboard Bindings</h3>\n"
    "            <div id=\"bindingsScrollArea\">\n"
    "                <div id=\"bindingsList\"></div>\n"
    "            </div>\n"
    "            <div class=\"modal-footer\">\n"
    "                <div class=\"footer-group\">\n"
    "                    <button id=\"btnSaveBindings\" style=\"background: #e3f2fd; border-color: #90caf9;\">Save</button>\n"
    "                    <button id=\"btnCancelBindings\">Cancel</button>\n"
    "                </div>\n"
    "                <div class=\"footer-group\">\n"
    "                    <button id=\"btnSetupBindings\">Setup Wizard</button>\n"
    "                    <button id=\"btnResetBindings\">Reset</button>\n"
    "                </div>\n"
    "            </div>\n"
    "        </div>\n"
    "    </div>\n"
    "<script>\n"
    "const PROTO_MAGIC = 0x4E535743;\n"
    "const PROTO_VERSION = 4;\n"
    "const SECRET = \"nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY\";\n"
    "const PACKET_SIZE = 68;\n"
    "const PACKET_AUTH_SIZE = 52;\n"
    "const BTN_Y = 1<<0, BTN_B = 1<<1, BTN_A = 1<<2, BTN_X = 1<<3;\n"
    "const BTN_L = 1<<4, BTN_R = 1<<5, BTN_ZL = 1<<6, BTN_ZR = 1<<7;\n"
    "const BTN_MINUS = 1<<8, BTN_PLUS = 1<<9, BTN_LSTICK = 1<<10, BTN_RSTICK = 1<<11;\n"
    "const BTN_HOME = 1<<12, BTN_CAPTURE = 1<<13;\n"
    "const HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3, HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8;\n"
    "let ws = null;\n"
    "let isConnected = false;\n"
    "let loopId = null;\n"
    "let seqCounter = 0;\n"
    "const keysDown = new Set();\n"
    "const defaultBindings = {\n"
    "    'BTN_Y': 'KeyZ', 'BTN_B': 'KeyX', 'BTN_A': 'KeyV', 'BTN_X': 'KeyC',\n"
    "    'BTN_L': 'KeyQ', 'BTN_R': 'KeyE', 'BTN_ZL': 'Digit1', 'BTN_ZR': 'Digit2',\n"
    "    'BTN_MINUS': 'Digit3', 'BTN_PLUS': 'Digit4',\n"
    "    'BTN_LSTICK': 'ShiftLeft', 'BTN_RSTICK': 'ShiftRight',\n"
    "    'BTN_HOME': 'Home', 'BTN_CAPTURE': 'PrintScreen',\n"
    "    'DPAD_UP': 'ArrowUp', 'DPAD_DOWN': 'ArrowDown', 'DPAD_LEFT': 'ArrowLeft', 'DPAD_RIGHT': 'ArrowRight',\n"
    "    'LSTICK_UP': 'KeyW', 'LSTICK_DOWN': 'KeyS', 'LSTICK_LEFT': 'KeyA', 'LSTICK_RIGHT': 'KeyD',\n"
    "    'RSTICK_UP': 'KeyI', 'RSTICK_DOWN': 'KeyK', 'RSTICK_LEFT': 'KeyJ', 'RSTICK_RIGHT': 'KeyL'\n"
    "};\n"
    "let currentBindings = { ...defaultBindings };\n"
    "let preEditBindings = {};\n"
    "window.onload = () => {\n"
    "    const isMobile = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent);\n"
    "    if (isMobile) {\n"
    "        document.getElementById('kbModeContainer').style.display = 'none';\n"
    "        document.getElementById('btnBindings').style.display = 'none';\n"
    "        document.getElementById('btnTouchControls').style.display = 'inline-block';\n"
    "        document.getElementById('btnEditor').style.display = 'inline-block';\n"
    "    }\n"
    "    const savedMode = localStorage.getItem('nswc_mode');\n"
    "    if (savedMode) document.getElementById('kbMode').value = savedMode;\n"
    "    const savedBindings = localStorage.getItem('nswc_bindings');\n"
    "    if (savedBindings) currentBindings = JSON.parse(savedBindings);\n"
    "};\n"
    "document.getElementById('kbMode').onchange = (e) => localStorage.setItem('nswc_mode', e.target.value);\n"
    "window.addEventListener('keydown', (e) => {\n"
    "    if (activeBindKey) { e.preventDefault(); remapKey(e.code); return; }\n"
    "    keysDown.add(e.code);\n"
    "});\n"
    "window.addEventListener('keyup', (e) => keysDown.delete(e.code));\n"
    "function getNeutralState() { return { buttons: 0, hat: HAT_NEUTRAL, lx: 128, ly: 128, rx: 128, ry: 128 }; }\n"
    "function getKeyboardState() {\n"
    "    let buttons = 0, hat = HAT_NEUTRAL, lx = 128, ly = 128, rx = 128, ry = 128;\n"
    "    if (keysDown.has(currentBindings['BTN_Y'])) buttons |= BTN_Y;\n"
    "    if (keysDown.has(currentBindings['BTN_B'])) buttons |= BTN_B;\n"
    "    if (keysDown.has(currentBindings['BTN_A'])) buttons |= BTN_A;\n"
    "    if (keysDown.has(currentBindings['BTN_X'])) buttons |= BTN_X;\n"
    "    if (keysDown.has(currentBindings['BTN_L'])) buttons |= BTN_L;\n"
    "    if (keysDown.has(currentBindings['BTN_R'])) buttons |= BTN_R;\n"
    "    if (keysDown.has(currentBindings['BTN_ZL'])) buttons |= BTN_ZL;\n"
    "    if (keysDown.has(currentBindings['BTN_ZR'])) buttons |= BTN_ZR;\n"
    "    if (keysDown.has(currentBindings['BTN_MINUS'])) buttons |= BTN_MINUS;\n"
    "    if (keysDown.has(currentBindings['BTN_PLUS'])) buttons |= BTN_PLUS;\n"
    "    if (keysDown.has(currentBindings['BTN_LSTICK'])) buttons |= BTN_LSTICK;\n"
    "    if (keysDown.has(currentBindings['BTN_RSTICK'])) buttons |= BTN_RSTICK;\n"
    "    if (keysDown.has(currentBindings['BTN_HOME'])) buttons |= BTN_HOME;\n"
    "    if (keysDown.has(currentBindings['BTN_CAPTURE'])) buttons |= BTN_CAPTURE;\n"
    "    const up = keysDown.has(currentBindings['DPAD_UP']), down = keysDown.has(currentBindings['DPAD_DOWN']);\n"
    "    const left = keysDown.has(currentBindings['DPAD_LEFT']), right = keysDown.has(currentBindings['DPAD_RIGHT']);\n"
    "    if (up && right) hat = HAT_NE; else if (up && left) hat = HAT_NW;\n"
    "    else if (down && right) hat = HAT_SE; else if (down && left) hat = HAT_SW;\n"
    "    else if (up) hat = HAT_N; else if (down) hat = HAT_S;\n"
    "    else if (left) hat = HAT_W; else if (right) hat = HAT_E;\n"
    "    if (keysDown.has(currentBindings['LSTICK_LEFT']) && !keysDown.has(currentBindings['LSTICK_RIGHT'])) lx = 0;\n"
    "    else if (keysDown.has(currentBindings['LSTICK_RIGHT']) && !keysDown.has(currentBindings['LSTICK_LEFT'])) lx = 255;\n"
    "    if (keysDown.has(currentBindings['LSTICK_UP']) && !keysDown.has(currentBindings['LSTICK_DOWN'])) ly = 0;\n"
    "    else if (keysDown.has(currentBindings['LSTICK_DOWN']) && !keysDown.has(currentBindings['LSTICK_UP'])) ly = 255;\n"
    "    if (keysDown.has(currentBindings['RSTICK_LEFT']) && !keysDown.has(currentBindings['RSTICK_RIGHT'])) rx = 0;\n"
    "    else if (keysDown.has(currentBindings['RSTICK_RIGHT']) && !keysDown.has(currentBindings['RSTICK_LEFT'])) rx = 255;\n"
    "    if (keysDown.has(currentBindings['RSTICK_UP']) && !keysDown.has(currentBindings['RSTICK_DOWN'])) ry = 0;\n"
    "    else if (keysDown.has(currentBindings['RSTICK_DOWN']) && !keysDown.has(currentBindings['RSTICK_UP'])) ry = 255;\n"
    "    return { buttons, hat, lx, ly, rx, ry };\n"
    "}\n"
    "function getGamepadState(pad) {\n"
    "    if (!pad) return null;\n"
    "    let buttons = 0, hat = HAT_NEUTRAL, lx = 128, ly = 128, rx = 128, ry = 128;\n"
    "    if (pad.buttons[0]?.pressed) buttons |= BTN_B;\n"
    "    if (pad.buttons[1]?.pressed) buttons |= BTN_A;\n"
    "    if (pad.buttons[2]?.pressed) buttons |= BTN_Y;\n"
    "    if (pad.buttons[3]?.pressed) buttons |= BTN_X;\n"
    "    if (pad.buttons[4]?.pressed) buttons |= BTN_L;\n"
    "    if (pad.buttons[5]?.pressed) buttons |= BTN_R;\n"
    "    if (pad.buttons[6]?.pressed) buttons |= BTN_ZL;\n"
    "    if (pad.buttons[7]?.pressed) buttons |= BTN_ZR;\n"
    "    if (pad.buttons[8]?.pressed) buttons |= BTN_MINUS;\n"
    "    if (pad.buttons[9]?.pressed) buttons |= BTN_PLUS;\n"
    "    if (pad.buttons[10]?.pressed) buttons |= BTN_LSTICK;\n"
    "    if (pad.buttons[11]?.pressed) buttons |= BTN_RSTICK;\n"
    "    if (pad.buttons[16]?.pressed) buttons |= BTN_HOME;\n"
    "    if (pad.buttons[17]?.pressed) buttons |= BTN_CAPTURE;\n"
    "    if ((buttons & BTN_LSTICK) && (buttons & BTN_RSTICK)) buttons |= BTN_HOME;\n"
    "    if ((buttons & BTN_MINUS) && (buttons & BTN_PLUS)) buttons |= BTN_CAPTURE;\n"
    "    const pup = pad.buttons[12]?.pressed, pdown = pad.buttons[13]?.pressed;\n"
    "    const pleft = pad.buttons[14]?.pressed, pright = pad.buttons[15]?.pressed;\n"
    "    if (pup && pright) hat = HAT_NE; else if (pup && pleft) hat = HAT_NW;\n"
    "    else if (pdown && pright) hat = HAT_SE; else if (pdown && pleft) hat = HAT_SW;\n"
    "    else if (pup) hat = HAT_N; else if (pdown) hat = HAT_S;\n"
    "    else if (pleft) hat = HAT_W; else if (pright) hat = HAT_E;\n"
    "    const applyDeadzone = (val) => { if (Math.abs(val) < 0.15) return 128; return Math.round(((val + 1) / 2) * 255); };\n"
    "    if (pad.axes.length >= 4) {\n"
    "        lx = applyDeadzone(pad.axes[0]); ly = applyDeadzone(pad.axes[1]);\n"
    "        rx = applyDeadzone(pad.axes[2]); ry = applyDeadzone(pad.axes[3]);\n"
    "    }\n"
    "    return { buttons, hat, lx, ly, rx, ry };\n"
    "}\n"
    "function mergeStates(s1, s2) {\n"
    "    if (!s1) return s2; if (!s2) return s1;\n"
    "    return { buttons: s1.buttons | s2.buttons, hat: s1.hat !== HAT_NEUTRAL ? s1.hat : s2.hat,\n"
    "        lx: s1.lx !== 128 ? s1.lx : s2.lx, ly: s1.ly !== 128 ? s1.ly : s2.ly,\n"
    "        rx: s1.rx !== 128 ? s1.rx : s2.rx, ry: s1.ry !== 128 ? s1.ry : s2.ry };\n"
    "}\n"
    "function buildAndSendPacket() {\n"
    "    if (!ws || ws.readyState !== WebSocket.OPEN) return;\n"
    "    const rawGamepads = navigator.getGamepads ? navigator.getGamepads() : [];\n"
    "    const activePads = [];\n"
    "    for (let i = 0; i < rawGamepads.length; i++) if (rawGamepads[i]) activePads.push(rawGamepads[i]);\n"
    "    const mode = parseInt(document.getElementById('kbMode').value);\n"
    "    const kbState = getKeyboardState();\n"
    "    let slotStates = [null, null, null, null];\n"
    "    let uiText = [\"\", \"\", \"\", \"\"];\n"
    "    if (mode === 0) {\n"
    "        for (let i = 0; i < 4; i++) {\n"
    "            let gp = getGamepadState(activePads[i]);\n"
    "            slotStates[i] = gp || getNeutralState();\n"
    "            uiText[i] = gp ? \"Connected\" : \"Not connected\";\n"
    "        }\n"
    "    } else if (mode === 1) {\n"
    "        slotStates[0] = kbState; uiText[0] = `Keyboard (Connected)`;\n"
    "        for (let i = 1; i < 4; i++) {\n"
    "            let gp = getGamepadState(activePads[i - 1]);\n"
    "            slotStates[i] = gp || getNeutralState();\n"
    "            uiText[i] = gp ? \"Connected\" : \"Not connected\";\n"
    "        }\n"
    "    } else if (mode === 2) {\n"
    "        let gp0 = getGamepadState(activePads[0]);\n"
    "        slotStates[0] = mergeStates(kbState, gp0 || getNeutralState());\n"
    "        uiText[0] = `${gp0 ? \"Connected\" : \"Not connected\"} \\\\ Keyboard`;\n"
    "        for (let i = 1; i < 4; i++) {\n"
    "            let gp = getGamepadState(activePads[i]);\n"
    "            slotStates[i] = gp || getNeutralState();\n"
    "            uiText[i] = gp ? \"Connected\" : \"Not connected\";\n"
    "        }\n"
    "    }\n"
    "    const buffer = new ArrayBuffer(PACKET_SIZE), view = new DataView(buffer);\n"
    "    view.setUint32(0, PROTO_MAGIC, true); view.setUint8(4, PROTO_VERSION); view.setUint8(5, 0);\n"
    "    view.setUint16(6, 0, true); view.setUint32(8, seqCounter++, true); view.setBigUint64(12, BigInt(Date.now() * 1000), true);\n"
    "    for(let p = 0; p < 4; p++) {\n"
    "        document.getElementById(`p${p+1}Text`).innerText = `P${p+1}: ${uiText[p]}`;\n"
    "        let finalButtons = slotStates[p].buttons;\n"
    "        if (finalButtons & BTN_CAPTURE) finalButtons &= ~(BTN_PLUS | BTN_MINUS);\n"
    "        if (finalButtons & BTN_HOME) finalButtons &= ~(BTN_LSTICK | BTN_RSTICK);\n"
    "        const offset = 20 + (p * 8);\n"
    "        view.setUint16(offset, finalButtons, true);\n"
    "        view.setUint8(offset + 2, slotStates[p].hat);\n"
    "        view.setUint8(offset + 3, slotStates[p].lx); view.setUint8(offset + 4, slotStates[p].ly);\n"
    "        view.setUint8(offset + 5, slotStates[p].rx); view.setUint8(offset + 6, slotStates[p].ry);\n"
    "        view.setUint8(offset + 7, 0);\n"
    "    }\n"
    "    ws.send(buffer);\n"
    "}\n"
    "document.getElementById('btnConnect').onclick = async () => {\n"
    "    if (isConnected) {\n"
    "        clearInterval(loopId); ws.close(); isConnected = false;\n"
    "        document.getElementById('btnConnect').innerText = \"Connect\";\n"
    "        document.getElementById('statusText').innerText = \"Disconnected\";\n"
    "        document.getElementById('kbMode').disabled = false;\n"
    "        return;\n"
    "    }\n"
    "    const wsUrl = window.location.protocol === 'https:' ? `wss://${window.location.host}` : `ws://${window.location.host}`;\n"
    "    ws = new WebSocket(wsUrl); ws.binaryType = \"arraybuffer\";\n"
    "    ws.onopen = () => {\n"
    "        isConnected = true;\n"
    "        document.getElementById('btnConnect').innerText = \"Disconnect\";\n"
    "        document.getElementById('kbMode').disabled = true;\n"
    "        document.getElementById('statusText').innerText = `Connected to Pi Proxy.`;\n"
    "        loopId = setInterval(buildAndSendPacket, 4);\n"
    "    };\n"
    "    ws.onerror = () => alert(\"Failed to connect to proxy!\");\n"
    "    ws.onclose = () => {\n"
    "        isConnected = false; clearInterval(loopId);\n"
    "        document.getElementById('btnConnect').innerText = \"Connect\";\n"
    "        document.getElementById('statusText').innerText = \"Disconnected\";\n"
    "        document.getElementById('kbMode').disabled = false;\n"
    "    }\n"
    "};\n"
    "function formatKeyName(code) {\n"
    "    if (code === 'Unbound') return '';\n"
    "    if (code.startsWith('Key')) return code.replace('Key', '');\n"
    "    if (code.startsWith('Digit')) return code.replace('Digit', '');\n"
    "    if (code.startsWith('Arrow')) return code.replace('Arrow', '');\n"
    "    if (code === 'ShiftLeft') return 'LShift'; if (code === 'ShiftRight') return 'RShift';\n"
    "    if (code === 'ControlLeft') return 'LCtrl'; if (code === 'ControlRight') return 'RCtrl';\n"
    "    if (code === 'AltLeft') return 'LAlt'; if (code === 'AltRight') return 'RAlt';\n"
    "    return code;\n"
    "}\n"
    "let activeBindKey = null; let isSetupMode = false; let setupQueue = [];\n"
    "function renderBindings() {\n"
    "    const list = document.getElementById('bindingsList'); list.innerHTML = '';\n"
    "    for (const [btn, code] of Object.entries(currentBindings)) {\n"
    "        const row = document.createElement('div'); row.className = 'bind-row';\n"
    "        const label = document.createElement('span'); label.innerText = btn.replace('BTN_', '');\n"
    "        const btnChange = document.createElement('button'); btnChange.className = 'bind-btn';\n"
    "        btnChange.innerText = formatKeyName(code); btnChange.id = `btn-${btn}`;\n"
    "        btnChange.onclick = () => {\n"
    "            if (isSetupMode) return;\n"
    "            if (activeBindKey) document.getElementById(`btn-${activeBindKey}`).classList.remove('listening');\n"
    "            activeBindKey = btn; btnChange.innerText = \"---\"; btnChange.classList.add('listening');\n"
    "        };\n"
    "        row.appendChild(label); row.appendChild(btnChange); list.appendChild(row);\n"
    "    }\n"
    "}\n"
    "function startNextSetupBind() {\n"
    "    if (setupQueue.length === 0) {\n"
    "        isSetupMode = false; activeBindKey = null; renderBindings(); return;\n"
    "    }\n"
    "    activeBindKey = setupQueue.shift(); renderBindings();\n"
    "    const targetBtn = document.getElementById(`btn-${activeBindKey}`);\n"
    "    if (targetBtn) { targetBtn.innerText = \"---\"; targetBtn.classList.add('listening'); targetBtn.scrollIntoView({ behavior: 'smooth', block: 'center' }); }\n"
    "}\n"
    "function remapKey(code) {\n"
    "    if (!activeBindKey) return;\n"
    "    if (code === 'Escape') { isSetupMode = false; activeBindKey = null; renderBindings(); return; }\n"
    "    if (isSetupMode) {\n"
    "        for (const existingCode of Object.values(currentBindings)) if (existingCode === code) return;\n"
    "        currentBindings[activeBindKey] = code; startNextSetupBind();\n"
    "    } else {\n"
    "        for (const [existingBtn, existingCode] of Object.entries(currentBindings)) {\n"
    "            if (existingCode === code && existingBtn !== activeBindKey) currentBindings[existingBtn] = 'Unbound';\n"
    "        }\n"
    "        currentBindings[activeBindKey] = code; activeBindKey = null; renderBindings();\n"
    "    }\n"
    "}\n"
    "document.getElementById('btnBindings').onclick = () => {\n"
    "    preEditBindings = { ...currentBindings }; isSetupMode = false; activeBindKey = null;\n"
    "    renderBindings(); document.getElementById('modalOverlay').style.display = 'flex';\n"
    "};\n"
    "document.getElementById('btnSaveBindings').onclick = () => {\n"
    "    localStorage.setItem('nswc_bindings', JSON.stringify(currentBindings));\n"
    "    isSetupMode = false; activeBindKey = null; document.getElementById('modalOverlay').style.display = 'none';\n"
    "};\n"
    "document.getElementById('btnCancelBindings').onclick = () => {\n"
    "    currentBindings = { ...preEditBindings };\n"
    "    isSetupMode = false; activeBindKey = null; document.getElementById('modalOverlay').style.display = 'none';\n"
    "};\n"
    "document.getElementById('btnResetBindings').onclick = () => {\n"
    "    if (isSetupMode) isSetupMode = false;\n"
    "    currentBindings = { ...defaultBindings }; activeBindKey = null; renderBindings();\n"
    "};\n"
    "document.getElementById('btnSetupBindings').onclick = () => {\n"
    "    for (let k in currentBindings) currentBindings[k] = 'Unbound';\n"
    "    setupQueue = Object.keys(currentBindings); isSetupMode = true; startNextSetupBind();\n"
    "};\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static const char MOBILE_STYLE_AND_DOM[] =
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover\">\n"
    "    <style>\n"
    "        html, body { background: #111; color: #fff; margin:0; padding:0; width: 100%; height: 100%; overflow:hidden; touch-action:none; user-select:none; -webkit-user-select:none; font-family:sans-serif; }\n"
    "        #gamepad { position: fixed; top: env(safe-area-inset-top, 0px); right: env(safe-area-inset-right, 0px); bottom: env(safe-area-inset-bottom, 0px); left: env(safe-area-inset-left, 0px); box-sizing: border-box; }\n"
    "        #rotate-msg { display:none; position:absolute; inset:0; background:#000; z-index:9999; justify-content:center; align-items:center; text-align:center; padding:20px; font-size:18px; }\n"
    "        @media (orientation: portrait) { #rotate-msg { display:flex; } #gamepad { display:none; } }\n"
    "        \n"
    "        .edit-item { position: absolute; box-sizing: border-box; }\n"
    "        .btn { background: rgba(255,255,255,0.15); border: 1px solid rgba(255,255,255,0.2); display:flex; justify-content:center; align-items:center; font-weight:bold; color:#ddd; font-size:16px; box-shadow: 0 4px 8px rgba(0,0,0,0.3); }\n"
    "        .btn.active { background: rgba(255,255,255,0.7); color: #000; box-shadow: 0 0 10px rgba(255,255,255,0.5); }\n"
    "        .btn-circle { border-radius: 50%; width: 100%; height: 100%; }\n"
    "        .btn-shoulder { border-radius: 8px; font-size:14px; }\n"
    "        .btn-sys { border-radius: 50%; font-size: 14px; }\n"
    "        \n"
    "        .cross { display: grid; grid-template-columns: 1fr 1fr 1fr; grid-template-rows: 1fr 1fr 1fr; gap: 4px; }\n"
    "        .cross .up { grid-column: 2; grid-row: 1; }\n"
    "        .cross .down { grid-column: 2; grid-row: 3; }\n"
    "        .cross .left { grid-column: 1; grid-row: 2; }\n"
    "        .cross .right { grid-column: 3; grid-row: 2; }\n"
    "        .joystick { background: rgba(255,255,255,0.05); border: 2px solid rgba(255,255,255,0.15); border-radius: 50%; box-shadow: inset 0 0 20px rgba(0,0,0,0.5); aspect-ratio: 1/1; }\n"
    "        .knob { width: 40%; height: 40%; background: rgba(255,255,255,0.3); border-radius: 50%; position: absolute; top: 30%; left: 30%; pointer-events: none; box-shadow: 0 4px 10px rgba(0,0,0,0.4); }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n";

static const char MOBILE_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <title>NS Touch Controls</title>\n"
    "%s" // MOBILE_STYLE_AND_DOM
    "    <style>\n"
    "        #btnConnect { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); padding: 15px 30px; border-radius: 30px; background: rgba(200,0,0,0.8); border:none; color:#fff; font-weight:bold; font-size: 18px; z-index: 100; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }\n"
    "        #btnConnect.connected { background: rgba(0,200,0,0.8); }\n"
    "        #statusDot { display: none; position: absolute; bottom: 20px; right: 20px; width: 15px; height: 15px; background: #0f0; border-radius: 50%; box-shadow: 0 0 10px #0f0; z-index: 100; cursor: pointer; }\n"
    "    </style>\n"
    "    <div id=\"rotate-msg\">Please rotate to landscape mode.</div>\n"
    "    <div id=\"gamepad\">\n"
    "        <div id=\"statusDot\"></div>\n"
    "        <button id=\"btnConnect\">Connect</button>\n"
    "        <div id=\"btn-zl\" class=\"edit-item btn-shoulder btn btn-map\" data-btn=\"64\">ZL</div>\n"
    "        <div id=\"btn-l\" class=\"edit-item btn-shoulder btn btn-map\" data-btn=\"16\">L</div>\n"
    "        <div id=\"btn-zr\" class=\"edit-item btn-shoulder btn btn-map\" data-btn=\"128\">ZR</div>\n"
    "        <div id=\"btn-r\" class=\"edit-item btn-shoulder btn btn-map\" data-btn=\"32\">R</div>\n"
    "        <div id=\"btn-minus\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"256\">-</div>\n"
    "        <div id=\"btn-plus\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"512\">+</div>\n"
    "        <div id=\"btn-capture\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"8192\">O</div>\n"
    "        <div id=\"btn-home\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"4096\">⌂</div>\n"
    "        <div id=\"lstick\" class=\"edit-item joystick\"><div class=\"knob\" id=\"lknob\"></div></div>\n"
    "        <div id=\"btn-ls\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"1024\">LS</div>\n"
    "        <div id=\"dpad\" class=\"edit-item cross\">\n"
    "            <div class=\"btn btn-circle up btn-dpad\" data-dir=\"u\">&#x25B2;&#xFE0E;</div><div class=\"btn btn-circle left btn-dpad\" data-dir=\"l\">&#x25C0;&#xFE0E;</div>\n"
    "            <div class=\"btn btn-circle right btn-dpad\" data-dir=\"r\">&#x25B6;&#xFE0E;</div><div class=\"btn btn-circle down btn-dpad\" data-dir=\"d\">&#x25BC;&#xFE0E;</div>\n"
    "        </div>\n"
    "        <div id=\"abxy\" class=\"edit-item cross\">\n"
    "            <div class=\"btn btn-circle up btn-map\" data-btn=\"8\">X</div><div class=\"btn btn-circle left btn-map\" data-btn=\"1\">Y</div>\n"
    "            <div class=\"btn btn-circle right btn-map\" data-btn=\"4\">A</div><div class=\"btn btn-circle down btn-map\" data-btn=\"2\">B</div>\n"
    "        </div>\n"
    "        <div id=\"rstick\" class=\"edit-item joystick\"><div class=\"knob\" id=\"rknob\"></div></div>\n"
    "        <div id=\"btn-rs\" class=\"edit-item btn-sys btn btn-map\" data-btn=\"2048\">RS</div>\n"
    "    </div>\n"
    "<script>\n"
    "const defLayout = {\n"
    "    'btn-zl': {l:4, t:4, w:14, h:8}, 'btn-l': {l:4, t:14, w:14, h:8},\n"
    "    'btn-zr': {l:82, t:4, w:14, h:8}, 'btn-r': {l:82, t:14, w:14, h:8},\n"
    "    'btn-minus': {l:38, t:5, w:6}, 'btn-plus': {l:56, t:5, w:6},\n"
    "    'btn-capture': {l:42, t:18, w:5}, 'btn-home': {l:53, t:18, w:5},\n"
    "    'lstick': {l:6, t:35, w:16}, 'btn-ls': {l:2, t:65, w:5},\n"
    "    'dpad': {l:22, t:60, w:16},\n"
    "    'abxy': {l:78, t:35, w:16},\n"
    "    'rstick': {l:62, t:60, w:16}, 'btn-rs': {l:85, t:80, w:5}\n"
    "};\n"
    "let layout = JSON.parse(localStorage.getItem('nswc_layout')) || defLayout;\n"
    "function applyLayout() {\n"
    "    for(let id in defLayout) {\n"
    "        let el = document.getElementById(id);\n"
    "        if(!el) continue;\n"
    "        let conf = layout[id] || defLayout[id];\n"
    "        if(conf.hide) { el.style.display = 'none'; }\n"
    "        else {\n"
    "            if(id === 'dpad' || id === 'abxy') el.style.display = 'grid';\n"
    "            else el.style.display = 'flex';\n"
    "            el.style.left = conf.l + '%'; el.style.top = conf.t + '%'; el.style.width = conf.w + '%';\n"
    "            if(conf.h) el.style.height = conf.h + '%';\n"
    "            else { el.style.aspectRatio = '1 / 1'; el.style.height = 'auto'; }\n"
    "        }\n"
    "    }\n"
    "}\n"
    "applyLayout();\n"
    "const PROTO_MAGIC = 0x4E535743, PROTO_VERSION = 4, PACKET_SIZE = 68;\n"
    "let ws = null, loopId = null, seqCounter = 0, isConnected = false, connectTimeout = null;\n"
    "let state = { buttons: 0, hat: 8, lx: 128, ly: 128, rx: 128, ry: 128 };\n"
    "const btnTouches = new Map();\n"
    "document.querySelectorAll('.btn-map').forEach(el => {\n"
    "    const bit = parseInt(el.dataset.btn);\n"
    "    el.addEventListener('touchstart', e => {\n"
    "        e.preventDefault();\n"
    "        const id = e.changedTouches[0].identifier;\n"
    "        if (!btnTouches.has(el)) {\n"
    "            btnTouches.set(el, id);\n"
    "            state.buttons |= bit;\n"
    "            el.classList.add('active');\n"
    "        }\n"
    "    }, {passive:false});\n"
    "    el.addEventListener('touchend', e => {\n"
    "        e.preventDefault();\n"
    "        for (const t of e.changedTouches) {\n"
    "            if (btnTouches.get(el) === t.identifier) {\n"
    "                state.buttons &= ~bit;\n"
    "                el.classList.remove('active');\n"
    "                btnTouches.delete(el);\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "    }, {passive:false});\n"
    "    el.addEventListener('touchcancel', e => {\n"
    "        for (const t of e.changedTouches) {\n"
    "            if (btnTouches.get(el) === t.identifier) {\n"
    "                state.buttons &= ~bit;\n"
    "                el.classList.remove('active');\n"
    "                btnTouches.delete(el);\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "    }, {passive:false});\n"
    "});\n"
    "const dpadTouches = new Map();\n"
    "const dpad = { u:false, d:false, l:false, r:false };\n"
    "function updateHat() {\n"
    "    if (dpad.u && dpad.r) state.hat = 1; else if (dpad.u && dpad.l) state.hat = 7;\n"
    "    else if (dpad.d && dpad.r) state.hat = 3; else if (dpad.d && dpad.l) state.hat = 5;\n"
    "    else if (dpad.u) state.hat = 0; else if (dpad.d) state.hat = 4;\n"
    "    else if (dpad.l) state.hat = 6; else if (dpad.r) state.hat = 2; else state.hat = 8;\n"
    "}\n"
    "document.querySelectorAll('.btn-dpad').forEach(el => {\n"
    "    const dir = el.dataset.dir;\n"
    "    el.addEventListener('touchstart', e => {\n"
    "        e.preventDefault();\n"
    "        const id = e.changedTouches[0].identifier;\n"
    "        if (!dpadTouches.has(el)) {\n"
    "            dpadTouches.set(el, id);\n"
    "            dpad[dir] = true;\n"
    "            el.classList.add('active');\n"
    "            updateHat();\n"
    "        }\n"
    "    }, {passive:false});\n"
    "    el.addEventListener('touchend', e => {\n"
    "        e.preventDefault();\n"
    "        for (const t of e.changedTouches) {\n"
    "            if (dpadTouches.get(el) === t.identifier) {\n"
    "                dpad[dir] = false;\n"
    "                el.classList.remove('active');\n"
    "                dpadTouches.delete(el);\n"
    "                updateHat();\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "    }, {passive:false});\n"
    "    el.addEventListener('touchcancel', e => {\n"
    "        for (const t of e.changedTouches) {\n"
    "            if (dpadTouches.get(el) === t.identifier) {\n"
    "                dpad[dir] = false;\n"
    "                el.classList.remove('active');\n"
    "                dpadTouches.delete(el);\n"
    "                updateHat();\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "    }, {passive:false});\n"
    "});\n"
    "function setupJoystick(baseId, knobId, axisX, axisY) {\n"
    "    const base = document.getElementById(baseId), knob = document.getElementById(knobId);\n"
    "    if(!base) return;\n"
    "    let activeTouch = null;\n"
    "    const reset = () => { state[axisX] = 128; state[axisY] = 128; knob.style.transform = `translate(0px, 0px)`; activeTouch = null; };\n"
    "    base.addEventListener('touchstart', e => { e.preventDefault(); activeTouch = e.changedTouches[0].identifier; updateJoy(e.changedTouches[0]); }, {passive:false});\n"
    "    base.addEventListener('touchmove', e => {\n"
    "        e.preventDefault();\n"
    "        for (let i=0; i<e.changedTouches.length; i++) if (e.changedTouches[i].identifier === activeTouch) updateJoy(e.changedTouches[i]);\n"
    "    }, {passive:false});\n"
    "    base.addEventListener('touchend', e => { for (let i=0; i<e.changedTouches.length; i++) if (e.changedTouches[i].identifier === activeTouch) reset(); }, {passive:false});\n"
    "    base.addEventListener('touchcancel', e => { for (let i=0; i<e.changedTouches.length; i++) if (e.changedTouches[i].identifier === activeTouch) reset(); }, {passive:false});\n"
    "    function updateJoy(t) {\n"
    "        const rect = base.getBoundingClientRect(); const maxDist = rect.width / 2;\n"
    "        let dx = t.clientX - (rect.left + rect.width/2), dy = t.clientY - (rect.top + rect.height/2);\n"
    "        let dist = Math.sqrt(dx*dx + dy*dy);\n"
    "        if (dist > maxDist) { dx = (dx/dist)*maxDist; dy = (dy/dist)*maxDist; }\n"
    "        knob.style.transform = `translate(${dx}px, ${dy}px)`;\n"
    "        state[axisX] = Math.round(((dx / maxDist) + 1) * 127.5);\n"
    "        state[axisY] = Math.round(((dy / maxDist) + 1) * 127.5);\n"
    "    }\n"
    "}\n"
    "setupJoystick('lstick', 'lknob', 'lx', 'ly');\n"
    "setupJoystick('rstick', 'rknob', 'rx', 'ry');\n"
    "function sendPacket() {\n"
    "    if (!ws || ws.readyState !== WebSocket.OPEN) return;\n"
    "    const buffer = new ArrayBuffer(PACKET_SIZE), view = new DataView(buffer);\n"
    "    view.setUint32(0, PROTO_MAGIC, true); view.setUint8(4, PROTO_VERSION); view.setUint8(5, 0);\n"
    "    view.setUint16(6, 0, true); view.setUint32(8, seqCounter++, true); view.setBigUint64(12, BigInt(Date.now()*1000), true);\n"
    "    view.setUint16(20, state.buttons, true); view.setUint8(22, state.hat);\n"
    "    view.setUint8(23, state.lx); view.setUint8(24, state.ly); view.setUint8(25, state.rx); view.setUint8(26, state.ry);\n"
    "    for(let p=1; p<4; p++) {\n"
    "        let off = 20 + (p*8); view.setUint16(off, 0, true); view.setUint8(off+2, 8);\n"
    "        view.setUint8(off+3, 128); view.setUint8(off+4, 128); view.setUint8(off+5, 128); view.setUint8(off+6, 128);\n"
    "    }\n"
    "    ws.send(buffer);\n"
    "}\n"
    "document.getElementById('btnConnect').onclick = () => {\n"
    "    if (isConnected) { ws.close(); return; }\n"
    "    if (document.documentElement.requestFullscreen) { document.documentElement.requestFullscreen().catch(()=>{}); }\n"
    "    const wsUrl = window.location.protocol === 'https:' ? `wss://${window.location.host}` : `ws://${window.location.host}`;\n"
    "    ws = new WebSocket(wsUrl); ws.binaryType = \"arraybuffer\";\n"
    "    ws.onopen = () => {\n"
    "        isConnected = true; const btn = document.getElementById('btnConnect');\n"
    "        btn.innerText = \"Connected\"; btn.classList.add('connected');\n"
    "        connectTimeout = setTimeout(() => { btn.style.display = 'none'; document.getElementById('statusDot').style.display = 'block'; }, 2000);\n"
    "        loopId = setInterval(sendPacket, 4);\n"
    "    };\n"
    "    ws.onerror = () => alert(\"Connection failed!\");\n"
    "    ws.onclose = () => {\n"
    "        isConnected = false; clearInterval(loopId); clearTimeout(connectTimeout);\n"
    "        const btn = document.getElementById('btnConnect');\n"
    "        btn.style.display = 'block'; btn.innerText = \"Connect\"; btn.classList.remove('connected');\n"
    "        document.getElementById('statusDot').style.display = 'none';\n"
    "    };\n"
    "};\n"
    "document.getElementById('statusDot').onclick = () => { if(ws) ws.close(); };\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static const char EDITOR_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <title>Layout Editor</title>\n"
    "%s" // MOBILE_STYLE_AND_DOM
    "    <style>\n"
    "        .overlap { border: 2px solid red !important; box-shadow: 0 0 15px red !important; z-index: 1000; }\n"
    "        #editor-bar {\n"
    "            position: fixed; top: 50%; left: 50%; transform: translate(-50%, -50%);\n"
    "            background: rgba(0,0,0,0.85); border-radius: 12px; display: flex; flex-direction: column;\n"
    "            z-index: 10000; box-shadow: 0 10px 30px rgba(0,0,0,0.8); border: 1px solid #444; width: 300px;\n"
    "            max-height: 90vh; overflow-y: auto;\n"
    "        }\n"
    "        #editor-bar button, #editor-bar select { padding: 10px; font-size: 16px; border-radius: 6px; border: none; font-weight: bold; cursor: pointer; }\n"
    "        #toggleDel { background: #555; color: white; transition: 0.2s; }\n"
    "        #toggleDel.active { background: #e33; }\n"
    "    </style>\n"
    "    <div id=\"rotate-msg\">Please rotate to landscape mode.</div>\n"
    "    <div id=\"gamepad\">\n"
    "        <div id=\"menu-toggle\" onclick=\"toggleMenu()\" style=\"position:absolute; top:50%; left:50%; transform:translate(-50%,-50%); width:50px; height:50px; background:rgba(255,255,255,0.2); border-radius:50%; display:none; justify-content:center; align-items:center; z-index:9999; font-size:24px; box-shadow: 0 0 10px rgba(0,0,0,0.5); border: 1px solid rgba(255,255,255,0.5);\">⚙️</div>\n"
    "        <div id=\"editor-bar\">\n"
    "            <div id=\"eb-header\" style=\"padding: 10px; background: #333; border-radius: 12px 12px 0 0; display: flex; justify-content: space-between; align-items: center; cursor: move;\">\n"
    "                <span style=\"font-weight:bold; font-size:14px; color: white;\">Editor Settings</span>\n"
    "                <button onclick=\"toggleMenu()\" style=\"background:none; border:none; color:white; font-size:22px; padding:10px; margin-right:-5px; cursor:pointer; line-height:1;\">❌</button>\n"
    "            </div>\n"
    "            <div style=\"padding: 15px; display: flex; flex-direction: column; gap: 10px;\">\n"
    "                <div style=\"color:#fff; text-align:center; font-size:14px; margin-bottom:5px;\">Drag to Move, Pinch to Resize</div>\n"
    "                <button onclick=\"saveLayout()\" style=\"background:#0a0; color:#fff;\">Save</button>\n"
    "                <select id=\"addSel\" onchange=\"addBtn()\"><option value=\"\">+ Add Button</option></select>\n"
    "                <button id=\"toggleDel\" onclick=\"toggleDel()\">🗑️ Delete Mode: OFF</button>\n"
    "                <button onclick=\"window.location.href='/mobile'\" style=\"background:#a00; color:#fff;\">Cancel</button>\n"
    "                <button onclick=\"resetLayout()\" style=\"background:#e91e63; color:#fff;\">Reset to Defaults</button>\n"
    "            </div>\n"
    "        </div>\n"
    "        <div id=\"btn-zl\" class=\"edit-item btn-shoulder btn\">ZL</div>\n"
    "        <div id=\"btn-l\" class=\"edit-item btn-shoulder btn\">L</div>\n"
    "        <div id=\"btn-zr\" class=\"edit-item btn-shoulder btn\">ZR</div>\n"
    "        <div id=\"btn-r\" class=\"edit-item btn-shoulder btn\">R</div>\n"
    "        <div id=\"btn-minus\" class=\"edit-item btn-sys btn\">-</div>\n"
    "        <div id=\"btn-plus\" class=\"edit-item btn-sys btn\">+</div>\n"
    "        <div id=\"btn-capture\" class=\"edit-item btn-sys btn\">O</div>\n"
    "        <div id=\"btn-home\" class=\"edit-item btn-sys btn\">⌂</div>\n"
    "        <div id=\"lstick\" class=\"edit-item joystick\"><div class=\"knob\"></div></div>\n"
    "        <div id=\"btn-ls\" class=\"edit-item btn-sys btn\">LS</div>\n"
    "        <div id=\"dpad\" class=\"edit-item cross\">\n"
    "            <div class=\"btn btn-circle up\">&#x25B2;&#xFE0E;</div><div class=\"btn btn-circle left\">&#x25C0;&#xFE0E;</div>\n"
    "            <div class=\"btn btn-circle right\">&#x25B6;&#xFE0E;</div><div class=\"btn btn-circle down\">&#x25BC;&#xFE0E;</div>\n"
    "        </div>\n"
    "        <div id=\"abxy\" class=\"edit-item cross\">\n"
    "            <div class=\"btn btn-circle up\">X</div><div class=\"btn btn-circle left\">Y</div>\n"
    "            <div class=\"btn btn-circle right\">A</div><div class=\"btn btn-circle down\">B</div>\n"
    "        </div>\n"
    "        <div id=\"rstick\" class=\"edit-item joystick\"><div class=\"knob\"></div></div>\n"
    "        <div id=\"btn-rs\" class=\"edit-item btn-sys btn\">RS</div>\n"
    "    </div>\n"
    "<script>\n"
    "const defLayout = {\n"
    "    'btn-zl': {l:4, t:4, w:14, h:8}, 'btn-l': {l:4, t:14, w:14, h:8},\n"
    "    'btn-zr': {l:82, t:4, w:14, h:8}, 'btn-r': {l:82, t:14, w:14, h:8},\n"
    "    'btn-minus': {l:38, t:5, w:6}, 'btn-plus': {l:56, t:5, w:6},\n"
    "    'btn-capture': {l:42, t:18, w:5}, 'btn-home': {l:53, t:18, w:5},\n"
    "    'lstick': {l:6, t:35, w:16}, 'btn-ls': {l:2, t:65, w:5},\n"
    "    'dpad': {l:22, t:60, w:16},\n"
    "    'abxy': {l:78, t:35, w:16},\n"
    "    'rstick': {l:62, t:60, w:16}, 'btn-rs': {l:85, t:80, w:5}\n"
    "};\n"
    "let layout = JSON.parse(localStorage.getItem('nswc_layout')) || defLayout;\n"
    "function applyLayout() {\n"
    "    for(let id in defLayout) {\n"
    "        let el = document.getElementById(id);\n"
    "        if(!el) continue;\n"
    "        let conf = layout[id] || defLayout[id];\n"
    "        if(conf.hide) { el.style.display = 'none'; }\n"
    "        else {\n"
    "            if(id === 'dpad' || id === 'abxy') el.style.display = 'grid';\n"
    "            else el.style.display = 'flex';\n"
    "            el.style.left = conf.l + '%'; el.style.top = conf.t + '%'; el.style.width = conf.w + '%';\n"
    "            if(conf.h) el.style.height = conf.h + '%';\n"
    "            else { el.style.aspectRatio = '1 / 1'; el.style.height = 'auto'; }\n"
    "        }\n"
    "    }\n"
    "}\n"
    "applyLayout();\n"
    "function populateAdd() {\n"
    "    let sel = document.getElementById('addSel'); sel.innerHTML = '<option value=\"\">+ Add Button</option>';\n"
    "    for(let id in defLayout) {\n"
    "        let conf = layout[id] || defLayout[id];\n"
    "        if(conf.hide) { let opt = document.createElement('option'); opt.value = id; opt.innerText = id; sel.appendChild(opt); }\n"
    "    }\n"
    "}\n"
    "populateAdd();\n"
    "function addBtn() {\n"
    "    let val = document.getElementById('addSel').value;\n"
    "    if(!val) return;\n"
    "    if(!layout[val]) layout[val] = {...defLayout[val]};\n"
    "    layout[val].hide = false; layout[val].l = 45; layout[val].t = 15;\n"
    "    applyLayout(); populateAdd(); checkOverlaps();\n"
    "}\n"
    "function checkOverlaps() {\n"
    "    let els = Array.from(document.querySelectorAll('.edit-item')).filter(e => e.style.display !== 'none');\n"
    "    els.forEach(e => e.classList.remove('overlap'));\n"
    "    let hasOverlap = false;\n"
    "    for(let i=0; i<els.length; i++) {\n"
    "        for(let j=i+1; j<els.length; j++) {\n"
    "            let b1 = els[i].getBoundingClientRect(), b2 = els[j].getBoundingClientRect();\n"
    "            let s1x = b1.width * 0.1, s1y = b1.height * 0.1;\n"
    "            let s2x = b2.width * 0.1, s2y = b2.height * 0.1;\n"
    "            if (!(b1.right - s1x < b2.left + s2x || b1.left + s1x > b2.right - s2x || b1.bottom - s1y < b2.top + s2y || b1.top + s1y > b2.bottom - s2y)) {\n"
    "                els[i].classList.add('overlap'); els[j].classList.add('overlap'); hasOverlap = true;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    return hasOverlap;\n"
    "}\n"
    "function toggleMenu() {\n"
    "    let eb = document.getElementById('editor-bar'); let tg = document.getElementById('menu-toggle');\n"
    "    if(eb.style.display === 'none') { eb.style.display = 'flex'; tg.style.display = 'none'; }\n"
    "    else { eb.style.display = 'none'; tg.style.display = 'flex'; }\n"
    "}\n"
    "let eb = document.getElementById('editor-bar');\n"
    "let isDraggingMenu = false, menuDx, menuDy;\n"
    "eb.addEventListener('touchstart', e => {\n"
    "    if (e.target.closest('button, select, input')) return;\n"
    "    isDraggingMenu = true; let rect = eb.getBoundingClientRect();\n"
    "    menuDx = e.touches[0].clientX - (rect.left + rect.width/2);\n"
    "    menuDy = e.touches[0].clientY - (rect.top + rect.height/2);\n"
    "    e.stopPropagation();\n"
    "}, {passive:false});\n"
    "window.addEventListener('touchmove', e => {\n"
    "    if(isDraggingMenu) {\n"
    "        e.preventDefault();\n"
    "        eb.style.left = (e.touches[0].clientX - menuDx) + 'px';\n"
    "        eb.style.top = (e.touches[0].clientY - menuDy) + 'px';\n"
    "    }\n"
    "}, {passive:false});\n"
    "window.addEventListener('touchend', e => { isDraggingMenu = false; });\n"
    "let activeEl = null, mode = 'none', startX, startY, startL, startT, initialDist, startW, startH, deleteMode = false;\n"
    "function toggleDel() {\n"
    "    deleteMode = !deleteMode; const btn = document.getElementById('toggleDel');\n"
    "    if(deleteMode) { btn.classList.add('active'); btn.innerText = \"🗑️ Delete Mode: ON\"; }\n"
    "    else { btn.classList.remove('active'); btn.innerText = \"🗑️ Delete Mode: OFF\"; }\n"
    "}\n"
    "function getDist(t1, t2) { return Math.hypot(t1.clientX - t2.clientX, t1.clientY - t2.clientY); }\n"
    "document.getElementById('gamepad').addEventListener('touchstart', e => {\n"
    "    if(e.target.closest('#editor-bar') || e.target.closest('#menu-toggle')) return;\n"
    "    let el = e.target.closest('.edit-item'); if(!el) return;\n"
    "    e.preventDefault();\n"
    "    if (deleteMode) {\n"
    "        if(!layout[el.id]) layout[el.id] = {...defLayout[el.id]};\n"
    "        layout[el.id].hide = true; applyLayout(); populateAdd(); checkOverlaps(); return;\n"
    "    }\n"
    "    activeEl = el; if(!layout[el.id]) layout[el.id] = {...defLayout[el.id]};\n"
    "    if(e.touches.length === 1) {\n"
    "        mode = 'drag'; startX = e.touches[0].clientX; startY = e.touches[0].clientY;\n"
    "        startL = layout[el.id].l; startT = layout[el.id].t;\n"
    "    } else if(e.touches.length === 2) {\n"
    "        mode = 'pinch'; initialDist = getDist(e.touches[0], e.touches[1]);\n"
    "        startW = layout[el.id].w; startH = layout[el.id].h;\n"
    "    }\n"
    "}, {passive:false});\n"
    "document.getElementById('gamepad').addEventListener('touchmove', e => {\n"
    "    if(!activeEl || deleteMode) return; e.preventDefault();\n"
    "    let pw = window.innerWidth, ph = window.innerHeight, conf = layout[activeEl.id];\n"
    "    if(mode === 'drag' && e.touches.length === 1) {\n"
    "        let dx = e.touches[0].clientX - startX, dy = e.touches[0].clientY - startY;\n"
    "        conf.l = startL + (dx / pw * 100); conf.t = startT + (dy / ph * 100);\n"
    "    } else if(mode === 'pinch' && e.touches.length >= 2) {\n"
    "        let scale = getDist(e.touches[0], e.touches[1]) / initialDist;\n"
    "        conf.w = Math.max(2, startW * scale); if(startH) conf.h = Math.max(2, startH * scale);\n"
    "    }\n"
    "    applyLayout(); checkOverlaps();\n"
    "}, {passive:false});\n"
    "document.getElementById('gamepad').addEventListener('touchend', e => {\n"
    "    if(e.touches.length === 0) { activeEl = null; mode = 'none'; }\n"
    "    else if(e.touches.length === 1 && activeEl) {\n"
    "        mode = 'drag'; startX = e.touches[0].clientX; startY = e.touches[0].clientY;\n"
    "        startL = layout[activeEl.id].l; startT = layout[activeEl.id].t;\n"
    "    }\n"
    "});\n"
    "setTimeout(checkOverlaps, 500);\n"
    "function saveLayout() {\n"
    "    if(checkOverlaps()) { alert('Fix overlapping buttons (red) before saving!'); return; }\n"
    "    localStorage.setItem('nswc_layout', JSON.stringify(layout));\n"
    "    window.location.href = '/mobile';\n"
    "}\n"
    "function resetLayout() {\n"
    "    if(confirm('Are you sure you want to reset all buttons to their default positions and sizes?')) {\n"
    "        layout = JSON.parse(JSON.stringify(defLayout));\n"
    "        applyLayout();\n"
    "        populateAdd();\n"
    "        checkOverlaps();\n"
    "    }\n"
    "}\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";


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

    // Read buffer
    uint8_t buf[8192];
    size_t fill = 0;

    enum State : uint8_t {
        READ_HTTP,   // reading HTTP request headers
        WRITE_RESP,  // writing HTTP / WS upgrade response (POLLOUT)
        WS_ACTIVE,   // WebSocket mode (frames)
        CLOSED
    } state = CLOSED;

                // HTTP headers (null-terminated)
    char http_buf[8192];
    size_t http_len = 0;
    uint64_t connect_time = 0;
    uint32_t ip = 0;

    // WS session
    int      ws_slot = -1;
    uint32_t ws_seq = 0;
    bool     ws_first = true;
    uint64_t ws_last_rx = 0;

    // Async write queue (heap-allocated, freed after write completes or on cleanup)
    uint8_t *wbuf = nullptr;
    size_t   wlen = 0;
    size_t   woff = 0;
    State    after_write = CLOSED;
};


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

    // Control frames (ping / close) – handle before length validation
    if (opcode == 9) { // ping → pong
        if (masked)
            for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];
        uint8_t pong[2] = {0x8A, 0x00};
        ssize_t _u = write(c->fd, pong, 2); (void)_u;
        return total;
    }
    if (opcode == 8) { // close
        uint8_t close_frame[] = {0x88, 0x00};
        ssize_t _u = write(c->fd, close_frame, 2); (void)_u;
        c->state = WebClient::CLOSED;
        return total;
    }
    if (opcode == 0) { // continuation frames not supported (all messages are single-frame at PACKET_SIZE)
        c->state = WebClient::CLOSED;
        return total;
    }
    if (opcode != 2) return total;          // skip non-binary
    if (flen != PACKET_SIZE) {              // invalid binary frame → disconnect
        c->state = WebClient::CLOSED;
        return total;
    }

    // Unmask payload
    if (masked)
        for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];

    // ── Parse packet ──────────────────────────────────────────────────────
    uint32_t magic; memcpy(&magic, payload, 4);
    if (magic != PROTO_MAGIC) return total;
    uint8_t ver; memcpy(&ver, payload + 4, 1);
    if (ver != PROTO_VERSION) return total;
    uint8_t flags; memcpy(&flags, payload + 5, 1);
    bool is_reset = (flags & FLAG_RESET);
    uint32_t seq; memcpy(&seq, payload + 8, 4);

    // Sequence anti-replay (modular comparison handles uint32 wrap)
    if (!c->ws_first && !is_reset && (int32_t)(seq - c->ws_seq) < 0) return total;
    c->ws_first = false;
    c->ws_seq = seq + 1;

    // Decode report starting at byte 20
    MultiReport report;
    memcpy(&report, payload + 20, sizeof(MultiReport));
    uint64_t now = now_us();

    // Local watchdog (no shared state)
    if (c->ws_slot >= 0 && (now - c->ws_last_rx > WATCHDOG_MS * 1000ULL))
        c->ws_slot = -1;

    // Verify existing slot is still active or find a free one
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
                g_clients[i].last_rx_us = now;
                break;
            }
        }
    }
    if (c->ws_slot >= 0) {
        std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
        if (is_reset)
            g_clients[c->ws_slot].report.reset();
        else
            g_clients[c->ws_slot].report = report;
        g_clients[c->ws_slot].last_rx_us = now;
    }
    c->ws_last_rx = now;
    ++g_pkts_rx;

    return total;
}


// ── Base64 encoding ──────────────────────────────────────────────────────────
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
static char* html_format(const char *fmt, const char *arg, size_t *out_len) {
    size_t len = strlen(fmt) + strlen(arg) + 1;
    char *buf = (char*)malloc(len);
    if (!buf) { *out_len = 0; return nullptr; }
    int written = snprintf(buf, len, fmt, arg);
    if (written < 0 || (size_t)written >= len) { free(buf); *out_len = 0; return nullptr; }
    *out_len = written;
    return buf;
}


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


// ── Web Server Thread (single-threaded poll reactor, fully non-blocking) ─────
static void web_server_thread(int web_port, uint16_t udp_port) {
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

    std::printf("[web] HTTP + WebSocket server listening on port %d\n", web_port);

    struct pollfd pfds[1 + MAX_WS_CLIENTS];
    WebClient     clients[MAX_WS_CLIENTS];
    int           n_clients = 0;

    pfds[0].fd = srv; pfds[0].events = POLLIN; pfds[0].revents = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) { pfds[i+1].fd = -1; }

    while (g_running.load(std::memory_order_relaxed)) {
        // Idle WS timeout (30s) and HTTP handshake timeout (5s)
        uint64_t now_ws = now_us();
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
                pfds[i+1].events = (clients[i].state == WebClient::WRITE_RESP) ? POLLOUT : POLLIN;
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

            // ── WRITE_RESP: flush queued response ───────────────────────────
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
                        char *free_body = nullptr;
                        int status = 200;
                        const char *status_str = "OK";

                        if (req_match(c->http_buf, "GET / ") ||
                            req_match(c->http_buf, "GET /index.html ")) {
                            body = INDEX_HTML;
                            body_len = strlen(INDEX_HTML);
                        } else if (req_match(c->http_buf, "GET /mobile ")) {
                            free_body = html_format(MOBILE_HTML, MOBILE_STYLE_AND_DOM, &body_len);
                            body = free_body;
                        } else if (req_match(c->http_buf, "GET /editor ")) {
                            free_body = html_format(EDITOR_HTML, MOBILE_STYLE_AND_DOM, &body_len);
                            body = free_body;
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

                        free(free_body);
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
                    if (g_clients[clients[i].ws_slot].last_rx_us == clients[i].ws_last_rx)
                        g_clients[clients[i].ws_slot].active = false;
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
                if (g_clients[clients[i].ws_slot].last_rx_us == clients[i].ws_last_rx)
                    g_clients[clients[i].ws_slot].active = false;
            }
            close(clients[i].fd);
        }
    }
    close(srv);
    std::printf("[web] server stopped\n");
}


// ══════════════════════════════════════════════════════════════════════════════
// ── UDP receive loop (main thread) ────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
    std::string bind_addr = "0.0.0.0";
    bool        do_upnp   = false;
    int         web_port  = 0; // 0 = disabled

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if      (a == "-p" && i+1 < argc) port      = (uint16_t)std::atoi(argv[++i]);
        else if (a == "-b" && i+1 < argc) bind_addr = argv[++i];
        else if (a == "-v")               g_verbose  = true;
        else if (a == "--upnp")           do_upnp    = true;
        else if (a == "--no-gadget")      g_auto_gadget_setup = false;
        else if (a == "--keep-gadget")    g_teardown_gadget_exit = false;
        else if (a == "-w") {
            if (i+1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
                web_port = std::atoi(argv[++i]);
            else
                web_port = 8080;
        }
        else if (a == "-h") {
            puts("ns-backend  [-p PORT] [-b ADDR] [--upnp] [-w [WEB_PORT]] [-v] [--no-gadget] [--keep-gadget]");
            return 0;
        }
    }

    derive_key(DEFAULT_SECRET, g_hmac_key);
    signal(SIGINT,  on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (g_auto_gadget_setup && !setup_hori_gadget_builtin(true, "startup reset"))
        return 1;

    if (do_upnp) upnp_add_mapping(port);

    // Start web server if requested
    std::thread web_thread;
    if (web_port > 0)
        web_thread = std::thread(web_server_thread, web_port, port);

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int rbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }
    
    std::printf("UDP %s:%u writer=%d Hz\n",
                bind_addr.c_str(), port, WRITER_HZ);

    std::thread wt(writer_thread, WRITER_HZ);
    std::thread st(stats_thread);

    int ep = epoll_create1(0); epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = sock; epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);

    uint8_t udp_rx[UDP_RX_MAX_PACKET_SIZE];
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 4, 200);
        if (n <= 0) continue;

        sockaddr_in sender{};
        socklen_t slen;
        ssize_t bytes;

        // Drain all available packets from the kernel buffer.
        // Accept both the classic HORI Packet and the modern extended UDP
        // packets used by the upgraded cross-platform clients.  Extended gyro
        // and rumble fields are ignored here; only the 8-byte HORI input report
        // from each logical pad is applied.
        while (g_running.load(std::memory_order_relaxed)) {
            slen = sizeof(sender);
            bytes = recvfrom(sock, udp_rx, sizeof(udp_rx), 0, (sockaddr*)&sender, &slen);
            if (bytes <= 0) break; // EAGAIN or error — ring is drained

            bool is_extended_udp = false;
            Packet pkt{};
            ModernExtendedUdpPacketWire ext_pkt{};

            if (bytes == (ssize_t)PACKET_SIZE) {
                memcpy(&pkt, udp_rx, sizeof(pkt));
            } else if (bytes == (ssize_t)EXT_UDP_PACKET_SIZE) {
                memcpy(&ext_pkt, udp_rx, sizeof(ext_pkt));
                is_extended_udp = true;
            } else {
                if (g_verbose) std::printf("[udp] unexpected packet size=%zd, dropped\n", bytes);
                continue;
            }

        // ── 1. Per-IP rate limiter ────────────────────────────────────────────────
        uint32_t src_ip = sender.sin_addr.s_addr;
        if (!rate_allow(src_ip)) {
            if (g_verbose) puts("rate limit exceeded, dropped");
            continue;
        }

        // ── 2. Magic + version check ──────────────────────────────────────────────
        if (is_extended_udp) {
            if (!modern_extended_udp_packet_ok(ext_pkt)) {
                if (g_verbose) puts("bad extended UDP magic/version, dropped");
                continue;
            }
        } else if (!packet_ok(pkt)) {
            if (g_verbose) puts("bad magic/version, dropped");
            continue;
        }

        // ── 3. Find Client Session or Pin new IP ──────────────────────────────────
        int client_idx = -1;
        uint64_t now = now_us();

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            std::lock_guard<std::mutex> lk(g_mtx[i]);
            if (g_clients[i].active &&
                g_clients[i].addr.sin_addr.s_addr == src_ip &&
                g_clients[i].addr.sin_port == sender.sin_port) {
                client_idx = i;
                break;
            }
        }

        // If not found, assign to a free/timed-out slot
        if (client_idx == -1) {
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                std::lock_guard<std::mutex> lk(g_mtx[i]);
                if (!g_clients[i].active || (now - g_clients[i].last_rx_us > WATCHDOG_MS * 1000ULL)) {
                    client_idx = i;
                    g_clients[i].active = true;
                    g_clients[i].addr = sender;
                    g_clients[i].first_pkt = true;
                    g_clients[i].expected_seq = 0;
                    g_clients[i].report.reset();
                    g_clients[i].last_rx_us = now;
                    if (g_verbose) std::printf("New PC accepted into Server Slot %d/4\n", i+1);
                    break;
                }
            }
        }

        // If all 4 slots are taken by active PCs, drop the packet
        if (client_idx == -1) {
            if (g_verbose) puts("server is full (4 PCs already active), dropped");
            continue;
        }

        // ── 4. HMAC authentication ────────────────────────────────────────────────
        int hmac_ok = 0;
        if (is_extended_udp) {
            hmac_ok = hmac_verify(g_hmac_key, 32,
                                  reinterpret_cast<const uint8_t*>(&ext_pkt),
                                  EXT_UDP_PACKET_AUTH_SIZE,
                                  ext_pkt.hmac, HMAC_TAG_SIZE);
        } else {
            hmac_ok = hmac_verify(g_hmac_key, 32,
                                  reinterpret_cast<const uint8_t*>(&pkt),
                                  PACKET_AUTH_SIZE,
                                  pkt.hmac, HMAC_TAG_SIZE);
        }
        if (hmac_ok != 0) {
            if (g_verbose) puts("bad HMAC, dropped");
            continue;
        }

        // ── 5. Re-validate + Sequence counter + Apply to shared state ─────────────
        {
            std::lock_guard<std::mutex> lk(g_mtx[client_idx]);

            // Re-validate: writer may have deactivated the slot during HMAC
            if (!g_clients[client_idx].active) continue;

            uint8_t flags = is_extended_udp ? ext_pkt.flags : pkt.flags;
            uint32_t seq = is_extended_udp ? ext_pkt.seq : pkt.seq;
            bool is_reset = (flags & FLAG_RESET);
            bool sequence_jump = (g_clients[client_idx].expected_seq > seq) && ((g_clients[client_idx].expected_seq - seq) > 100);

            if (!g_clients[client_idx].first_pkt && seq < g_clients[client_idx].expected_seq && !is_reset && !sequence_jump) {
                if (g_verbose)
                    std::printf("PC %d out-of-order seq=%u, dropped\n", client_idx+1, seq);
                continue;
            }
            g_clients[client_idx].first_pkt = false;
            g_clients[client_idx].expected_seq = seq + 1;

            if (is_reset) {
                g_clients[client_idx].report.reset();
            } else if (is_extended_udp) {
                modern_extended_to_legacy_multi(ext_pkt.report, g_clients[client_idx].report);
            } else {
                g_clients[client_idx].report = pkt.report;
            }
            g_clients[client_idx].last_rx_us = now_us();
        }
        ++g_pkts_rx;
        } // drain loop
    } // epoll loop

    puts("[backend] shutting down");
    upnp_remove_mapping(port);
    close(ep); close(sock);
    wt.join(); st.join();
    if (web_thread.joinable()) web_thread.join();
    if (g_auto_gadget_setup && g_teardown_gadget_exit)
        teardown_gadget();
    return 0;
}
