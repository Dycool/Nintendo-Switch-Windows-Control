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

static std::mutex    g_mtx;
static ClientSession g_clients[MAX_CLIENTS];

// Diagnostics
static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};

// ── Signal ────────────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }


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
            std::puts("[backend] 4x /dev/hidg* opened — Ready for up to 4 PCs");
        was_connected = true;

        auto next = Clock::now() + tick;
        MultiReport prev{}; prev.p1.buttons = 0xFFFF; // Force first write
        bool error_shown = false;

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now(); next = std::max(next + tick, now + tick);

            MultiReport r;
            r.reset(); // Base neutral state

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                uint64_t now_stamp = now_us();

                // 1. Clear timed-out clients (Watchdog)
                for (int c = 0; c < MAX_CLIENTS; ++c) {
                    if (g_clients[c].active && (now_stamp - g_clients[c].last_rx_us > WATCHDOG_MS * 1000ULL)) {
                        g_clients[c].active = false;
                        if (g_verbose) std::printf("[backend] PC %d timed out and was disconnected.\n", c+1);
                    }
                }

                // 2. Free hardware slots mapped to inactive clients
                for (int h = 0; h < 4; ++h) {
                    if (hw_slots[h].client_idx != -1) {
                        if (!g_clients[hw_slots[h].client_idx].active) {
                            hw_slots[h].client_idx = -1;
                            hw_slots[h].sub_idx = -1;
                        }
                    }
                }

                // 3. Auto-assign unmapped active inputs to free hardware slots
                for (int c = 0; c < MAX_CLIENTS; ++c) {
                    if (!g_clients[c].active) continue;
                    
                    HIDReport* subs[4] = { &g_clients[c].report.p1, &g_clients[c].report.p2, 
                                           &g_clients[c].report.p3, &g_clients[c].report.p4 };
                    
                    for (int s = 0; s < 4; ++s) {
                        bool mapped = false;
                        for (int h = 0; h < 4; ++h) {
                            if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                                mapped = true; break;
                            }
                        }
                        
                        // If player pressed a button and doesn't have a physical port yet
                        if (!mapped && !is_neutral(*subs[s])) {
                            for (int h = 0; h < 4; ++h) {
                                if (hw_slots[h].client_idx == -1) {
                                    hw_slots[h].client_idx = c;
                                    hw_slots[h].sub_idx = s;
                                    if (g_verbose) 
                                        std::printf("[backend] Map -> PC %d (Pad %d) took Switch Port %d\n", c+1, s+1, h+1);
                                    break;
                                }
                            }
                        }
                    }
                }

                // 4. Construct the final mixed 4-player report
                HIDReport* out_subs[4] = { &r.p1, &r.p2, &r.p3, &r.p4 };
                for (int h = 0; h < 4; ++h) {
                    if (hw_slots[h].client_idx != -1) {
                        int c = hw_slots[h].client_idx;
                        int s = hw_slots[h].sub_idx;
                        HIDReport* src_subs[4] = { &g_clients[c].report.p1, &g_clients[c].report.p2, 
                                                   &g_clients[c].report.p3, &g_clients[c].report.p4 };
                        *out_subs[h] = *src_subs[s];
                    }
                }
            }

            // 5. Send to physical USB gadget drivers efficiently
            bool ok = true;
            if (r.p1 != prev.p1) { if(write(fds[0], &r.p1, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p2 != prev.p2) { if(write(fds[1], &r.p2, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p3 != prev.p3) { if(write(fds[2], &r.p3, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p4 != prev.p4) { if(write(fds[3], &r.p4, 8) < 0 && errno != EAGAIN) ok = false; }

            if (!ok) {
                if (!error_shown) { std::puts("[backend] Switch disconnected — waiting for reconnect..."); error_shown = true; }
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
        if (fds[i] >= 0) { (void)write(fds[i], &neutral.p1, 8); close(fds[i]); }
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
static bool rate_allow(uint32_t ip) {
    uint64_t now = now_us();
    uint32_t idx = ip % RATE_TABLE;
    RateSlot &s = g_rate_table[idx];
    if (s.ip != ip) {
        s.ip = ip; s.count = 1; s.window_start = now; return true;
    }
    if (now - s.window_start > RATE_WINDOW_US) {
        s.count = 1; s.window_start = now; return true;
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
    if (g_upnp_active) return false; 

    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
    if (!devlist) return false;
    
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
    freeUPNPDevlist(devlist);
    
    if (igd != 1 && igd != 2) return false;
    
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    
    int r = UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype,
                                port_str, port_str, g_upnp_lan_addr, "ns-backend", "UDP", nullptr, "0");
    if (r != 0) { FreeUPNPUrls(&g_upnp_urls); return false; }
    
    g_upnp_active = true;
    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, external_ip) == 0) {
        std::printf("[backend] UPnP: UDP port %u successfully forwarded!\n", port);
        std::printf("[backend] UPnP: Tell your clients to connect to -> %s:%u\n", external_ip, port);
    }
    return true;
}

static void upnp_remove_mapping(uint16_t port) {
    if (!g_upnp_active) return;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str, "UDP", nullptr);
    std::puts("[backend] UPnP: port mapping removed cleanly");
    FreeUPNPUrls(&g_upnp_urls); g_upnp_active = false;
}
#else
static bool upnp_add_mapping(uint16_t) { return false; }
static void upnp_remove_mapping(uint16_t) {}
#endif


// ── UDP receive loop (main thread) ────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
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

    derive_key(DEFAULT_SECRET, g_hmac_key);
    signal(SIGINT,  on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (do_upnp) upnp_add_mapping(port);

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int rbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }
    
    std::printf("[backend] UDP %s:%u  Mode=Multi-Client Router (Max 4 PCs)  writer=%d Hz  HMAC=always\n",
                bind_addr.c_str(), port, WRITER_HZ);

    std::thread wt(writer_thread, WRITER_HZ);
    std::thread st(stats_thread);

    int ep = epoll_create1(0); epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = sock; epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);

    Packet pkt{};
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 4, 200 /*ms timeout*/);
        if (n <= 0) continue;

        sockaddr_in sender{};
        socklen_t slen = sizeof(sender);
        ssize_t bytes = recvfrom(sock, &pkt, sizeof(pkt), 0, (sockaddr*)&sender, &slen);

        if (bytes != (ssize_t)PACKET_SIZE) continue;

        // ── 1. Per-IP rate limiter ────────────────────────────────────────────────
        uint32_t src_ip = sender.sin_addr.s_addr;
        if (!rate_allow(src_ip)) {
            if (g_verbose) puts("[backend] rate limit exceeded, dropped");
            continue;
        }

        // ── 2. Magic + version check ──────────────────────────────────────────────
        if (!packet_ok(pkt)) {
            if (g_verbose) puts("[backend] bad magic/version, dropped");
            continue;
        }

        // ── 3. Find Client Session or Pin new IP ──────────────────────────────────
        int client_idx = -1;
        uint64_t now = now_us();
        
        for (int i = 0; i < MAX_CLIENTS; ++i) {
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
                if (!g_clients[i].active || (now - g_clients[i].last_rx_us > WATCHDOG_MS * 1000ULL)) {
                    client_idx = i;
                    g_clients[i].active = true;
                    g_clients[i].addr = sender;
                    g_clients[i].first_pkt = true;
                    g_clients[i].report.reset();
                    if (g_verbose) std::printf("[backend] New PC accepted into Server Slot %d/4\n", i+1);
                    break;
                }
            }
        }

        // If all 4 slots are taken by active PCs, drop the packet
        if (client_idx == -1) {
            if (g_verbose) puts("[backend] server is full (4 PCs already active), dropped");
            continue;
        }

        // ── 4. HMAC authentication ────────────────────────────────────────────────
        if (hmac_verify(g_hmac_key, 32, (const uint8_t *)&pkt, PACKET_AUTH_SIZE, pkt.hmac, HMAC_TAG_SIZE) != 0) {
            if (g_verbose) puts("[backend] bad HMAC, dropped");
            continue;
        }

        // ── 5. Sequence counter (Anti-Replay) ─────────────────────────────────────
        bool is_reset = (pkt.flags & FLAG_RESET);
        bool sequence_jump = (g_clients[client_idx].expected_seq > pkt.seq) && ((g_clients[client_idx].expected_seq - pkt.seq) > 100);

        if (!g_clients[client_idx].first_pkt && pkt.seq < g_clients[client_idx].expected_seq && !is_reset && !sequence_jump) {
            if (g_verbose)
                std::printf("[backend] PC %d out-of-order seq=%u, dropped\n", client_idx+1, pkt.seq);
            continue;
        }
        g_clients[client_idx].first_pkt = false;
        g_clients[client_idx].expected_seq = pkt.seq + 1;

        // ── 6. Apply to shared state ──────────────────────────────────────────────
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (is_reset) {
                g_clients[client_idx].report.reset();
            } else {
                g_clients[client_idx].report = pkt.report;
            }
            g_clients[client_idx].last_rx_us = now_us();
        }
        ++g_pkts_rx;
    }

    puts("[backend] shutting down");
    upnp_remove_mapping(port);
    close(ep); close(sock);
    wt.join(); st.join();
    return 0;
}