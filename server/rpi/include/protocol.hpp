#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  protocol.hpp  –  Shared between frontend (PC) and backend (Pi)
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <cstring>
#include <chrono>

namespace ns {

// ── Tuning Constants ──────────────────────────────────────────────────────────
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u; // 'NSWC' Magic Number
static constexpr uint8_t  PROTO_VERSION = 4;           // Version 4 (Strict 4-Player Hub)
static constexpr uint16_t DEFAULT_PORT  = 7331;        // Default UDP Port
static constexpr int      WATCHDOG_MS   = 1200;        // Silence timeout -> zero all inputs (ms)
static constexpr int      WRITER_HZ     = 250;         // HID writes per second on backend
static constexpr int      AUTOFIRE_HZ   = 12;          // Autofire toggle rate (Hz)

// ── Enums ─────────────────────────────────────────────────────────────────────
enum Button : uint16_t {
    BTN_Y       = 1u <<  0, BTN_B       = 1u <<  1,
    BTN_A       = 1u <<  2, BTN_X       = 1u <<  3,
    BTN_L       = 1u <<  4, BTN_R       = 1u <<  5,
    BTN_ZL      = 1u <<  6, BTN_ZR      = 1u <<  7,
    BTN_MINUS   = 1u <<  8, BTN_PLUS    = 1u <<  9,
    BTN_LSTICK  = 1u << 10, BTN_RSTICK  = 1u << 11,
    BTN_HOME    = 1u << 12, BTN_CAPTURE = 1u << 13,
};

enum Hat : uint8_t {
    HAT_N  = 0, HAT_NE = 1, HAT_E  = 2, HAT_SE = 3,
    HAT_S  = 4, HAT_SW = 5, HAT_W  = 6, HAT_NW = 7,
    HAT_NEUTRAL = 8,
};

enum Flags : uint8_t {
    FLAG_NONE     = 0x00,
    FLAG_RESET    = 0x01,  // Backend should zero all inputs immediately
    FLAG_AUTOFIRE = 0x02,  // Autofire mask is currently active
};


// ── Structs ───────────────────────────────────────────────────────────────────

// Exactly 8-byte HID input report for HORI Pokken mode
struct HIDReport {
    uint16_t buttons;
    uint8_t  hat;
    uint8_t  lx, ly, rx, ry, vendor;

    void reset() noexcept {
        buttons = 0; hat = HAT_NEUTRAL; 
        lx = 128; ly = 128; rx = 128; ry = 128; vendor = 0;
    }

    bool operator==(const HIDReport& o) const noexcept { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    bool operator!=(const HIDReport& o) const noexcept { return !(*this == o); }
} __attribute__((packed));

// 4-Player Composite Report
struct MultiReport {
    HIDReport p1, p2, p3, p4;

    void reset() noexcept {
        p1.reset(); p2.reset(); p3.reset(); p4.reset();
    }
} __attribute__((packed));


// ── UDP Wire Packet ───────────────────────────────────────────────────────────
static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
static constexpr std::size_t HMAC_TAG_SIZE = 16;

struct Packet {
    uint32_t    magic;         // PROTO_MAGIC
    uint8_t     version;       // PROTO_VERSION
    uint8_t     flags;         // Flags bitmask
    uint16_t    autofire_mask; // Buttons to autofire
    uint32_t    seq;           // Monotonic sequence counter
    uint64_t    ts_us;         // Sender timestamp
    MultiReport report;        // Contains states for all 4 controllers
    uint8_t     hmac[HMAC_TAG_SIZE]; // Signature
} __attribute__((packed));

static constexpr std::size_t PACKET_SIZE      = sizeof(Packet);
static constexpr std::size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;

// ── Utilities ─────────────────────────────────────────────────────────────────
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

inline bool packet_ok(const Packet& p) noexcept {
    return p.magic == PROTO_MAGIC && p.version == PROTO_VERSION;
}

} // namespace ns