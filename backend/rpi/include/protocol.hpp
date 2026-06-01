#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  protocol.hpp  –  shared between frontend (PC) and backend (Pi)
//
//  Wire: single UDP datagram (~22 bytes) sent on every state change AND
//        as a keep-alive at least once every HEARTBEAT_MS milliseconds.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <cstring>
#include <chrono>

namespace ns {

// ── Tuning ────────────────────────────────────────────────────────────────────
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u; // 'NSWC'
static constexpr uint8_t  PROTO_VERSION = 1;
static constexpr uint16_t DEFAULT_PORT  = 7331;
static constexpr int      HEARTBEAT_MS  = 50;    // frontend keep-alive interval
static constexpr int      WATCHDOG_MS   = 1200;  // silence -> zero all inputs
static constexpr int      WRITER_HZ     = 250;   // HID writes/second on backend
static constexpr int      AUTOFIRE_HZ   = 12;    // autofire toggle rate

// ── Switch Pro Controller button bitmask ──────────────────────────────────────
// Bit positions match the HID descriptor written by setup_gadget.sh.
enum Button : uint16_t {
    BTN_Y       = 1u <<  0,
    BTN_B       = 1u <<  1,
    BTN_A       = 1u <<  2,
    BTN_X       = 1u <<  3,
    BTN_L       = 1u <<  4,
    BTN_R       = 1u <<  5,
    BTN_ZL      = 1u <<  6,
    BTN_ZR      = 1u <<  7,
    BTN_MINUS   = 1u <<  8,
    BTN_PLUS    = 1u <<  9,
    BTN_LSTICK  = 1u << 10,
    BTN_RSTICK  = 1u << 11,
    BTN_HOME    = 1u << 12,
    BTN_CAPTURE = 1u << 13,
};

// ── D-pad HAT switch values ───────────────────────────────────────────────────
enum Hat : uint8_t {
    HAT_N  = 0, HAT_NE = 1, HAT_E  = 2, HAT_SE = 3,
    HAT_S  = 4, HAT_SW = 5, HAT_W  = 6, HAT_NW = 7,
    HAT_NEUTRAL = 8,
};

// ── 8-byte HID input report ───────────────────────────────────────────────────
// Layout MUST exactly match the USB gadget HID descriptor (setup_gadget.sh).
struct HIDReport {
    uint16_t buttons = 0;
    uint8_t  hat     = HAT_NEUTRAL;
    uint8_t  lx      = 128;   // 128 = centre
    uint8_t  ly      = 128;
    uint8_t  rx      = 128;
    uint8_t  ry      = 128;
    uint8_t  vendor  = 0;

    void reset() noexcept { *this = HIDReport{}; }

    bool operator==(const HIDReport& o) const noexcept {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
    bool operator!=(const HIDReport& o) const noexcept { return !(*this == o); }
} __attribute__((packed));

static_assert(sizeof(HIDReport) == 8, "HIDReport must be 8 bytes");

// ── Packet flags ──────────────────────────────────────────────────────────────
enum Flags : uint8_t {
    FLAG_NONE     = 0x00,
    FLAG_RESET    = 0x01,  // backend should zero all inputs
    FLAG_AUTOFIRE = 0x02,  // autofire_mask is active
};

// ── UDP wire packet (frontend -> backend) ─────────────────────────────────────
struct Packet {
    uint32_t  magic;         // PROTO_MAGIC
    uint8_t   version;       // PROTO_VERSION
    uint8_t   flags;         // Flags bitmask
    uint16_t  autofire_mask; // buttons to autofire (valid when FLAG_AUTOFIRE)
    uint32_t  seq;           // monotonic sequence counter
    uint64_t  ts_us;         // sender steady_clock microseconds (for latency stats)
    HIDReport report;
} __attribute__((packed));

static constexpr std::size_t PACKET_SIZE = sizeof(Packet);

// ── Utilities ─────────────────────────────────────────────────────────────────
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

inline bool packet_ok(const Packet& p) noexcept {
    return p.magic == PROTO_MAGIC && p.version == PROTO_VERSION;
}

} // namespace ns
