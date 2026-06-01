#include "../../include/protocol.hpp"

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

using namespace ns;
using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

// ── Global flags ──────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static bool g_verbose = false;

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
        prev.buttons = 0xFF; // force first write after (re)connect
        bool error_shown = false;

        // ── Inner loop: write until disconnected ─────────────────────────────
        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            // Watchdog
            uint64_t last = g_last_rx_us.load(std::memory_order_acquire);
            bool silent = (last != 0) && (now_us() - last > (uint64_t)WATCHDOG_MS * 1000u);

            HIDReport r;
            uint16_t  af_mask;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (silent) {
                    g_report.reset();
                    g_autofire_mask = 0;
                }
                r       = g_report;
                af_mask = g_autofire_mask;
            }

            if (silent && last != 0) {
                if (g_verbose) std::puts("[backend] watchdog: zeroing inputs");
                g_last_rx_us.store(0, std::memory_order_release);
            }

            // Autofire toggle
            if (af_mask) {
                if (++af_tick >= af_period) { af_tick = 0; af_state ^= af_mask; }
                r.buttons = (r.buttons & ~af_mask) | (af_state & af_mask);
            } else {
                af_tick = 0; af_state = 0;
            }

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
        HIDReport zero{};
        write(fd, &zero, sizeof(zero));
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

// ── UDP receive loop (main thread) ────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
    std::string device    = "/dev/hidg0";
    int         rate_hz   = WRITER_HZ;
    std::string bind_addr = "0.0.0.0";

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if      (a == "-p" && i+1 < argc) port      = (uint16_t)std::atoi(argv[++i]);
        else if (a == "-d" && i+1 < argc) device    = argv[++i];
        else if (a == "-r" && i+1 < argc) rate_hz   = std::atoi(argv[++i]);
        else if (a == "-b" && i+1 < argc) bind_addr = argv[++i];
        else if (a == "-v")               g_verbose  = true;
        else if (a == "-h") {
            puts("ns-backend  [-p PORT] [-d /dev/hidg0] [-r HZ] [-b ADDR] [-v]");
            return 0;
        }
    }
    rate_hz = std::clamp(rate_hz, 1, 1000);

    // Warn if HID device missing (common on first boot before setup_gadget.sh)
    struct stat dev_st{};
    if (stat(device.c_str(), &dev_st) != 0)
        std::fprintf(stderr, "[backend] WARNING: %s not found. "
                     "Did you run setup_gadget.sh?\n", device.c_str());

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

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
    std::printf("[backend] UDP %s:%u  device=%s  writer=%d Hz\n",
                bind_addr.c_str(), port, device.c_str(), rate_hz);

    // ── Threads ───────────────────────────────────────────────────────────────
    std::thread wt(writer_thread, device, rate_hz);
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
        if (!packet_ok(pkt)) {
            if (g_verbose) puts("[backend] bad magic/version, dropped");
            continue;
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
                g_report.reset();
                g_autofire_mask = 0;
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
    close(ep);
    close(sock);
    wt.join();
    st.join();
    return 0;
}
