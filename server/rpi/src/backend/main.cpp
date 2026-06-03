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

// HMAC authentication (key derived from DEFAULT_SECRET at startup)
static uint8_t  g_hmac_key[32];

// IP pinning: once a valid sender is seen, only accept packets from it
// until the watchdog timer releases the lock.
static bool         g_have_auth = false;
static sockaddr_in  g_auth_addr{};

// Per-IP rate limiting (token bucket, 32-entry hash table)
static constexpr uint64_t RATE_WINDOW_US = 1'000'000;  // 1 second
static constexpr uint32_t RATE_MAX_PKT   = 2000;        // max packets/sec per IP
static constexpr int      RATE_TABLE     = 32;

struct RateSlot {
    uint32_t ip;           // IP in network byte order, 0 = empty
    uint32_t count;
    uint64_t window_start; // us
};
static RateSlot g_rate_table[RATE_TABLE];

// ── Shared state (protected by g_mtx) ────────────────────────────────────────
// HIDReport is exactly 8 bytes.  On arm64/x86-64 an aligned 64-bit load/store
// is atomic, but we use a plain mutex for strict correctness.
static std::mutex  g_mtx;
static HIDReport   g_report{};
static uint16_t    g_autofire_mask = 0;

// Timestamp of last received packet (us, or 0 if none yet).
static std::atomic<uint64_t> g_last_rx_us{0};

// Diagnostics
static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};

// ── Signal ────────────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ── HID writer thread ─────────────────────────────────────────────────────────
static void writer_thread(const std::string& dev, int hz) {
    const auto tick = us(1'000'000 / hz);

    // Autofire bookkeeping
    const int af_period = std::max(1, hz / (AUTOFIRE_HZ * 2));
    int       af_tick   = 0;
    uint16_t  af_state  = 0;

    int  fd             = -1;
    bool was_connected  = false;

    while (g_running.load(std::memory_order_relaxed)) {

        // ── Open / reopen hidg0 ───────────────────────────────────────────────
        if (fd < 0) {
            fd = open(dev.c_str(), O_WRONLY);
            if (fd < 0) {
                // Device not ready — wait and retry silently
                std::this_thread::sleep_for(ms(500));
                continue;
            }
            if (g_verbose || was_connected)
                std::puts("[backend] /dev/hidg0 opened — Switch connected");
            was_connected = true;
        }

        auto next = Clock::now() + tick;
        HIDReport prev{};
        prev.buttons = 0xFFFF; // Force first write after (re)connect
        bool error_shown = false;

        // ── Inner loop: write until disconnected ─────────────────────────────
        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            // Watchdog: Trigger if we haven't received a UDP packet in 250ms
            uint64_t last = g_last_rx_us.load(std::memory_order_acquire);
            bool silent = (last != 0) && (now_us() - last > 250'000u);

            HIDReport r;
            uint16_t  af_mask;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (silent) {
                    // EXPLICIT NEUTRAL STATE: 
                    // Do not use reset() as it zeroes values (which presses UP/LEFT)
                    g_report.buttons = 0;   // 0 means no buttons pressed
                    g_report.hat     = 8;   // 8 is the HID standard for a centered D-Pad
                    g_report.lx      = 128; // 128 (0x80) is centered for joysticks
                    g_report.ly      = 128;
                    g_report.rx      = 128;
                    g_report.ry      = 128;
                    g_autofire_mask  = 0;
                }
                r       = g_report;
                af_mask = g_autofire_mask;
            }

            if (silent && last != 0) {
                if (g_verbose) std::puts("[backend] watchdog: connection lost, returning to neutral");
                g_last_rx_us.store(0, std::memory_order_release);
            }

            // Autofire toggle
            if (af_mask) {
                if (++af_tick >= af_period) { af_tick = 0; af_state ^= af_mask; }
                r.buttons = (r.buttons & ~af_mask) | (af_state & af_mask);
            } else {
                af_tick = 0; af_state = 0;
            }

            // Skip if the state hasn't changed to save CPU
            if (r == prev) continue;

            ssize_t n = write(fd, &r, sizeof(r));
            if (n == (ssize_t)sizeof(r)) {
                prev = r;
                ++g_hid_writes;
                error_shown = false;
            } else if (n < 0) {
                if (errno == EAGAIN) continue;
                // ESHUTDOWN / ENOTCONN = Switch disconnected
                if (!error_shown) {
                    std::puts("[backend] Switch disconnected — waiting for reconnect...");
                    error_shown = true;
                }
                close(fd);
                fd = -1;
                std::this_thread::sleep_for(ms(1000));
                break; // back to outer loop to reopen
            }
        }
    }

    if (fd >= 0) {
        // Send a true neutral state before cleanly shutting down the backend
        HIDReport neutral{};
        neutral.buttons = 0; 
        neutral.hat = 8;
        neutral.lx = 128; neutral.ly = 128; neutral.rx = 128; neutral.ry = 128;
        (void)write(fd, &neutral, sizeof(neutral));
        close(fd);
    }
}

// ── Stats thread ──────────────────────────────────────────────────────────────
static void stats_thread() {
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(ms(5000));
        if (!g_verbose) continue;
        std::printf("[backend] pkts_rx=%-8llu  hid_writes=%-8llu\n",
            (unsigned long long)g_pkts_rx.load(),
            (unsigned long long)g_hid_writes.load());
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
// Returns true if this packet is within the rate limit for its source IP.
static bool rate_allow(uint32_t ip) {
    uint64_t now = now_us();
    uint32_t idx = ip % RATE_TABLE;
    RateSlot &s = g_rate_table[idx];
    if (s.ip != ip) {
        s.ip = ip;
        s.count = 1;
        s.window_start = now;
        return true;
    }
    if (now - s.window_start > RATE_WINDOW_US) {
        s.count = 1;
        s.window_start = now;
        return true;
    }
    s.count++;
    return s.count <= RATE_MAX_PKT;
}

#ifdef USE_UPNP
// ── UPnP port forwarding ──
static bool g_upnp_active = false;
static UPNPUrls g_upnp_urls{};
static IGDdatas g_upnp_data{};
static char g_upnp_lan_addr[64]{};

static bool upnp_add_mapping(uint16_t port) {
    // Prevent re-initialization if UPnP is already active.
    // If you need to forward multiple ports, you should separate the 
    // IGD discovery phase from the AddPortMapping phase.
    if (g_upnp_active) {
        std::fprintf(stderr, "[backend] UPnP: Already active. Remove mapping first.\n");
        return false; 
    }

    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
    if (!devlist) {
        std::fprintf(stderr, "[backend] UPnP: no IGD (router) found\n");
        return false;
    }
    
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
    freeUPNPDevlist(devlist);
    
    if (igd != 1 && igd != 2) {
        std::fprintf(stderr, "[backend] UPnP: no valid IGD (code %d)\n", igd);
        return false;
    }
    
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    
    // Consider changing "0" to something like "3600" (1 hour) for safety
    int r = UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype,
                                port_str, port_str, g_upnp_lan_addr, "ns-backend", "UDP", nullptr, "0");
    if (r != 0) {
        std::fprintf(stderr, "[backend] UPnP: AddPortMapping failed: %s (code %d)\n", strupnperror(r), r);
        FreeUPNPUrls(&g_upnp_urls); // FIX: Prevent memory leak on mapping failure
        return false;
    }
    
    g_upnp_active = true;

    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, external_ip) == 0) {
        std::printf("[backend] UPnP: UDP port %u successfully forwarded!\n", port);
        std::printf("[backend] UPnP: Tell your client to connect to -> %s:%u\n", external_ip, port);
    } else {
        std::printf("[backend] UPnP: UDP port %u forwarded to LAN IP %s (Could not fetch public IP)\n", port, g_upnp_lan_addr);
    }
    
    return true;
}

static void upnp_remove_mapping(uint16_t port) {
    if (!g_upnp_active) return;
    
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str, "UDP", nullptr);
    
    std::puts("[backend] UPnP: port mapping removed cleanly");
    FreeUPNPUrls(&g_upnp_urls);
    g_upnp_active = false;
}
#else
static bool upnp_add_mapping(uint16_t) { return false; }
static void upnp_remove_mapping(uint16_t) {}
#endif

// ── UDP receive loop (main thread) ────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
    std::string device    = "/dev/hidg0";
    std::string bind_addr = "0.0.0.0";
    bool        do_upnp   = false;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if      (a == "-p" && i+1 < argc) port      = (uint16_t)std::atoi(argv[++i]);
        else if (a == "-b" && i+1 < argc) bind_addr = argv[++i];
        else if (a == "-v")               g_verbose  = true;
        else if (a == "--upnp")           do_upnp    = true;
        else if (a == "-h") {
            puts("ns-backend  [-p PORT] [-b ADDR] [--upnp] [-v]");
            return 0;
        }
    }

    // Derive HMAC key from the compiled-in default secret
    derive_key(DEFAULT_SECRET, g_hmac_key);

    // Warn if HID device missing (common on first boot before setup_gadget.sh)
    struct stat dev_st{};
    if (stat(device.c_str(), &dev_st) != 0)
        std::fprintf(stderr, "[backend] WARNING: %s not found. "
                     "Did you run setup_gadget.sh?\n", device.c_str());

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    // ── UPnP port forwarding ──
    if (do_upnp) upnp_add_mapping(port);

    // ── Create UDP socket ─────────────────────────────────────────────────────
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // Large receive buffer absorbs bursts without dropping packets
    int rbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(sock); return 1;
    }
    std::printf("[backend] UDP %s:%u  device=%s  writer=%d Hz  HMAC=always\n",
                bind_addr.c_str(), port, device.c_str(), WRITER_HZ);

    // ── Threads ───────────────────────────────────────────────────────────────
    std::thread wt(writer_thread, device, WRITER_HZ);
    std::thread st(stats_thread);

    // ── epoll ─────────────────────────────────────────────────────────────────
    int ep = epoll_create1(0);
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = sock;
    epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);

    Packet pkt{};
    uint32_t expected_seq = 0;
    bool first_pkt = true;
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 4, 200 /*ms timeout*/);
        if (n <= 0) continue;

        sockaddr_in sender{};
        socklen_t slen = sizeof(sender);
        ssize_t bytes = recvfrom(sock, &pkt, sizeof(pkt), 0,
                                 (sockaddr*)&sender, &slen);

        if (bytes != (ssize_t)PACKET_SIZE) continue;

        // ── 1. Per-IP rate limiter (trivial, drops flood before any real work) ──
        uint32_t src_ip = sender.sin_addr.s_addr;
        if (!rate_allow(src_ip)) {
            if (g_verbose) puts("[backend] rate limit exceeded, dropped");
            continue;
        }

        // ── 2. Magic + version check (very fast) ────────────────────────────────
        if (!packet_ok(pkt)) {
            if (g_verbose) puts("[backend] bad magic/version, dropped");
            continue;
        }

        // ── 3. IP pinning (fast — prevents non-pinned attackers from reaching HMAC)
        if (g_have_auth) {
            bool same_ip   = src_ip == g_auth_addr.sin_addr.s_addr;
            bool same_port = sender.sin_port == g_auth_addr.sin_port;
            if (!same_ip || !same_port) {
                uint64_t last = g_last_rx_us.load(std::memory_order_acquire);
                if (last != 0) {
                    if (g_verbose) puts("[backend] wrong sender IP, dropped");
                    continue;
                }
                if (g_verbose) puts("[backend] new sender IP accepted (old client timed out)");
                g_have_auth = false;
            }
        }

        // ── 4. HMAC authentication (always active) ───────────────────────────────
        if (hmac_verify(g_hmac_key, 32,
                        (const uint8_t *)&pkt, PACKET_AUTH_SIZE,
                        pkt.hmac, HMAC_TAG_SIZE) != 0) {
            if (g_verbose) puts("[backend] bad HMAC, dropped");
            continue;
        }

        // ── 5. Pin the sender (if not already pinned) ────────────────────────────
        if (!g_have_auth) {
            g_auth_addr = sender;
            g_have_auth = true;
        }

        bool is_reset = (pkt.flags & FLAG_RESET);
        bool sequence_jump = (expected_seq > pkt.seq) && ((expected_seq - pkt.seq) > 100);

        // Discard reordered packets (allow first packet through, and frontend restarts)
        if (!first_pkt && pkt.seq < expected_seq && !is_reset && !sequence_jump) {
            if (g_verbose)
                std::printf("[backend] out-of-order seq=%u (expected >=%u), dropped\n",
                            pkt.seq, expected_seq);
            continue;
        }
        first_pkt    = false;
        expected_seq = pkt.seq + 1;

        // Apply to shared state
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (pkt.flags & FLAG_RESET) {
                // EXPLICIT NEUTRAL STATE (Switch standard)
                g_report.buttons = 0;
                g_report.hat     = 8;
                g_report.lx      = 128;
                g_report.ly      = 128;
                g_report.rx      = 128;
                g_report.ry      = 128;
                g_autofire_mask  = 0;
                if (g_verbose) puts("[backend] reset received");
            } else {
                g_report = pkt.report;
                g_autofire_mask = (pkt.flags & FLAG_AUTOFIRE) ? pkt.autofire_mask : 0;
            }
        }
        g_last_rx_us.store(now_us(), std::memory_order_release);
        ++g_pkts_rx;

        if (g_verbose) {
            // Note: ts_us uses steady_clock which differs per machine,
            // so we just show seq and button state (not cross-machine latency)
            std::printf("[backend] seq=%-6u  btns=%04X hat=%u L(%u,%u) R(%u,%u)\n",
                        pkt.seq,
                        pkt.report.buttons, pkt.report.hat,
                        pkt.report.lx, pkt.report.ly,
                        pkt.report.rx, pkt.report.ry);
        }
    }

    puts("[backend] shutting down");
    upnp_remove_mapping(port);
    close(ep);
    close(sock);
    wt.join();
    st.join();
    return 0;
}
