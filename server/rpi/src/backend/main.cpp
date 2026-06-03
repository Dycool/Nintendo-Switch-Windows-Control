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

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

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
static bool g_multiplayer = false;

static uint8_t g_hmac_key[32];
static bool g_have_auth = false;
static sockaddr_in g_auth_addr{};

// Rate Limiter
static constexpr uint64_t RATE_WINDOW_US = 1'000'000;
static constexpr uint32_t RATE_MAX_PKT   = 2000;
static constexpr int      RATE_TABLE     = 32;

struct RateSlot { uint32_t ip; uint32_t count; uint64_t window_start; };
static RateSlot g_rate_table[RATE_TABLE];

// ── Shared state ──────────────────────────────────────────────────────────────
static std::mutex  g_mtx;
static HIDReport   g_hori_report{};
static GCHubReport g_gc_report{};
static uint16_t    g_autofire_mask = 0;
static std::atomic<uint64_t> g_last_rx_us{0};

static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};

static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ── Thread 1: HORI Pokken (Single Player) ─────────────────────────────────────
static void hori_thread(const std::string& dev, int hz) {
    const auto tick = us(1'000'000 / hz);
    const int af_period = std::max(1, hz / (AUTOFIRE_HZ * 2));
    int af_tick = 0;
    uint16_t af_state = 0;
    int fd = -1;
    bool was_connected = false;

    while (g_running.load(std::memory_order_relaxed)) {
        if (fd < 0) {
            fd = open(dev.c_str(), O_WRONLY);
            if (fd < 0) { std::this_thread::sleep_for(ms(500)); continue; }
            if (g_verbose || was_connected) std::puts("[backend] /dev/hidg0 opened (HORI Mode)");
            was_connected = true;
        }

        auto next = Clock::now() + tick;
        HIDReport prev{};
        prev.buttons = 0xFFFF; 
        bool error_shown = false;

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            uint64_t last = g_last_rx_us.load(std::memory_order_acquire);
            bool silent = (last != 0) && (now_us() - last > (WATCHDOG_MS * 1000ULL));

            HIDReport r;
            uint16_t af_mask;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (silent) {
                    g_hori_report.buttons = 0; g_hori_report.hat = 8;
                    g_hori_report.lx = 128; g_hori_report.ly = 128;
                    g_hori_report.rx = 128; g_hori_report.ry = 128;
                    g_autofire_mask = 0;
                }
                r = g_hori_report;
                af_mask = g_autofire_mask;
            }

            if (silent && last != 0) {
                if (g_verbose) std::puts("[backend] watchdog: returning to neutral");
                g_last_rx_us.store(0, std::memory_order_release);
            }

            if (af_mask) {
                if (++af_tick >= af_period) { af_tick = 0; af_state ^= af_mask; }
                r.buttons = (r.buttons & ~af_mask) | (af_state & af_mask);
            } else { af_tick = 0; af_state = 0; }

            if (r == prev) continue;

            ssize_t n = write(fd, &r, sizeof(r));
            if (n == (ssize_t)sizeof(r)) {
                prev = r; ++g_hid_writes; error_shown = false;
            } else if (n < 0) {
                if (errno == EAGAIN) continue;
                if (!error_shown) { std::puts("[backend] Switch disconnected..."); error_shown = true; }
                close(fd); fd = -1;
                std::this_thread::sleep_for(ms(1000));
                break; 
            }
        }
    }
    if (fd >= 0) close(fd);
}

// ── Thread 2: GameCube Hub (Multiplayer) ──────────────────────────────────────
static void gc_hub_thread(const std::string& dev) {
    const auto tick = us(1'000'000 / 125); // Strict 8ms for GC
    int fd = -1;
    bool was_connected = false;

    while (g_running.load(std::memory_order_relaxed)) {
        if (fd < 0) {
            fd = open(dev.c_str(), O_RDWR | O_NONBLOCK); 
            if (fd < 0) { std::this_thread::sleep_for(ms(500)); continue; }
            if (g_verbose || was_connected) std::puts("[backend] /dev/hidg0 opened (GC Hub Mode)");
            was_connected = true;
        }

        auto next = Clock::now() + tick;
        GCHubReport prev{};
        prev.id = 0xFF; 
        bool error_shown = false;

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            uint8_t discard[64];
            while (read(fd, discard, sizeof(discard)) > 0) {} 

            uint64_t last = g_last_rx_us.load(std::memory_order_acquire);
            bool silent = (last != 0) && (now_us() - last > (WATCHDOG_MS * 1000ULL));

            GCHubReport r;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (silent) g_gc_report.reset();
                r = g_gc_report;
            }

            if (silent && last != 0) g_last_rx_us.store(0, std::memory_order_release);
            if (r == prev) continue;

            ssize_t n = write(fd, &r, sizeof(r));
            if (n == (ssize_t)sizeof(r)) {
                prev = r; ++g_hid_writes; error_shown = false;
            } else if (n < 0 && errno != EAGAIN) {
                if (!error_shown) { std::puts("[backend] Switch disconnected..."); error_shown = true; }
                close(fd); fd = -1;
                std::this_thread::sleep_for(ms(1000));
                break;
            }
        }
    }
    if (fd >= 0) close(fd);
}

// ── Rate limiter & UPnP ───────────────────────────────────────────────────────
static void stats_thread() {
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(ms(5000));
        if (!g_verbose) continue;
        std::printf("[backend] pkts_rx=%-8llu  hid_writes=%-8llu\n",
            (unsigned long long)g_pkts_rx.load(), (unsigned long long)g_hid_writes.load());
    }
}

static bool rate_allow(uint32_t ip) {
    uint64_t now = now_us();
    uint32_t idx = ip % RATE_TABLE;
    RateSlot &s = g_rate_table[idx];
    if (s.ip != ip || now - s.window_start > RATE_WINDOW_US) {
        s.ip = ip; s.count = 1; s.window_start = now; return true;
    }
    return ++s.count <= RATE_MAX_PKT;
}

#ifdef USE_UPNP
static bool g_upnp_active = false;
static UPNPUrls g_upnp_urls{};
static IGDdatas g_upnp_data{};
static char g_upnp_lan_addr[64]{};

static bool upnp_add_mapping(uint16_t port) {
    if (g_upnp_active) return false; 
    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
    if (!devlist) return false;
    
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
    freeUPNPDevlist(devlist);
    if (igd != 1 && igd != 2) return false;
    
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);
    if (UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str, port_str, g_upnp_lan_addr, "ns-backend", "UDP", nullptr, "0") != 0) {
        FreeUPNPUrls(&g_upnp_urls); return false;
    }
    g_upnp_active = true;
    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, external_ip) == 0) {
        std::printf("[backend] UPnP: UDP port %u forwarded! Client IP: %s:%u\n", port, external_ip, port);
    }
    return true;
}

static void upnp_remove_mapping(uint16_t port) {
    if (!g_upnp_active) return;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str, "UDP", nullptr);
    FreeUPNPUrls(&g_upnp_urls); g_upnp_active = false;
}
#else
static bool upnp_add_mapping(uint16_t) { return false; }
static void upnp_remove_mapping(uint16_t) {}
#endif

// ── Main UDP Loop ─────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t port = DEFAULT_PORT;
    std::string device = "/dev/hidg0";
    std::string bind_addr = "0.0.0.0";
    bool do_upnp = false;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if      (a == "-p" && i+1 < argc) port = (uint16_t)std::atoi(argv[++i]);
        else if (a == "-b" && i+1 < argc) bind_addr = argv[++i];
        else if (a == "-m")               g_multiplayer = true;
        else if (a == "-v")               g_verbose = true;
        else if (a == "--upnp")           do_upnp = true;
    }

    derive_key(DEFAULT_SECRET, g_hmac_key);

    struct stat dev_st{};
    if (stat(device.c_str(), &dev_st) != 0)
        std::fprintf(stderr, "[backend] WARNING: %s not found. Did you run setup_gadget.sh?\n", device.c_str());

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (do_upnp) upnp_add_mapping(port);

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int yes = 1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int rbuf = 256 * 1024; setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    bind(sock, (sockaddr*)&addr, sizeof(addr));

    std::printf("[backend] Mode: %s | UDP %s:%u | HMAC=always\n", 
                g_multiplayer ? "GameCube 4-Player Hub" : "HORI 1-Player", bind_addr.c_str(), port);

    std::thread wt([&]() {
        if (g_multiplayer) gc_hub_thread(device);
        else               hori_thread(device, WRITER_HZ);
    });
    
    std::thread st(stats_thread);

    int ep = epoll_create1(0);
    epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = sock;
    epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);

    Packet pkt{};
    uint32_t expected_seq = 0;
    bool first_pkt = true;
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        if (epoll_wait(ep, evs, 4, 200) <= 0) continue;

        sockaddr_in sender{}; socklen_t slen = sizeof(sender);
        if (recvfrom(sock, &pkt, sizeof(pkt), 0, (sockaddr*)&sender, &slen) != (ssize_t)PACKET_SIZE) continue;

        uint32_t src_ip = sender.sin_addr.s_addr;
        if (!rate_allow(src_ip) || !packet_ok(pkt)) continue;

        if (g_have_auth && (src_ip != g_auth_addr.sin_addr.s_addr || sender.sin_port != g_auth_addr.sin_port)) {
            if (g_last_rx_us.load(std::memory_order_acquire) != 0) continue;
            g_have_auth = false;
        }

        if (hmac_verify(g_hmac_key, 32, (const uint8_t *)&pkt, PACKET_AUTH_SIZE, pkt.hmac, HMAC_TAG_SIZE) != 0) continue;

        if (!g_have_auth) { g_auth_addr = sender; g_have_auth = true; }

        bool is_reset = (pkt.flags & FLAG_RESET);
        int32_t seq_diff = (int32_t)(pkt.seq - expected_seq);
        bool is_old = seq_diff < 0;
        bool sequence_jump = seq_diff > 100;
        
        if (!first_pkt && is_old && !is_reset && !sequence_jump) continue;
        
        first_pkt = false; expected_seq = pkt.seq + 1;

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (is_reset) {
                if (g_multiplayer) g_gc_report.reset();
                else {
                    g_hori_report.buttons = 0; g_hori_report.hat = 8;
                    g_hori_report.lx = 128; g_hori_report.ly = 128;
                    g_hori_report.rx = 128; g_hori_report.ry = 128;
                }
                g_autofire_mask = 0;
            } else {
                if (g_multiplayer) g_gc_report = pkt.payload.gc;
                else { g_hori_report = pkt.payload.hori; g_autofire_mask = (pkt.flags & FLAG_AUTOFIRE) ? pkt.autofire_mask : 0; }
            }
        }
        g_last_rx_us.store(now_us(), std::memory_order_release);
        ++g_pkts_rx;
    }

    upnp_remove_mapping(port);
    close(ep); close(sock);
    wt.join(); st.join();
    return 0;
}