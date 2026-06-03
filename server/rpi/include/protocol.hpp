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
static constexpr uint8_t  PROTO_VERSION = 2;           // Version 2 (Supports both HORI and GameCube)
static constexpr uint16_t DEFAULT_PORT  = 7331;        // Default UDP Port
static constexpr int      HEARTBEAT_MS  = 50;          // Frontend keep-alive interval (ms)
static constexpr int      WATCHDOG_MS   = 1200;        // Silence timeout -> zero all inputs (ms)
static constexpr int      WRITER_HZ     = 250;         // HID writes per second on backend
static constexpr int      AUTOFIRE_HZ   = 12;          // Autofire toggle rate (Hz)

// ── Enums ─────────────────────────────────────────────────────────────────────
// Switch Pro Controller / HORI button bitmask
enum Button : uint16_t {
    BTN_Y       = 1u <<  0, BTN_B       = 1u <<  1,
    BTN_A       = 1u <<  2, BTN_X       = 1u <<  3,
    BTN_L       = 1u <<  4, BTN_R       = 1u <<  5,
    BTN_ZL      = 1u <<  6, BTN_ZR      = 1u <<  7,
    BTN_MINUS   = 1u <<  8, BTN_PLUS    = 1u <<  9,
    BTN_LSTICK  = 1u << 10, BTN_RSTICK  = 1u << 11,
    BTN_HOME    = 1u << 12, BTN_CAPTURE = 1u << 13,
};

// D-pad HAT switch values (8-way + neutral)
enum Hat : uint8_t {
    HAT_N  = 0, HAT_NE = 1, HAT_E  = 2, HAT_SE = 3,
    HAT_S  = 4, HAT_SW = 5, HAT_W  = 6, HAT_NW = 7,
    HAT_NEUTRAL = 8,
};

// Packet flags
enum Flags : uint8_t {
    FLAG_NONE     = 0x00,
    FLAG_RESET    = 0x01,  // Backend should zero all inputs immediately
    FLAG_AUTOFIRE = 0x02,  // Autofire mask is currently active
};


// ── Structs (Formatted as POD to ensure safe Union usage) ─────────────────────

// Exactly 8-byte HID input report for HORI Pokken mode
struct HIDReport {
    uint16_t buttons;
    uint8_t  hat;
    uint8_t  lx;
    uint8_t  ly;
    uint8_t  rx;
    uint8_t  ry;
    uint8_t  vendor;

    // Manually reset values to neutral state
    void reset() noexcept {
        buttons = 0; hat = HAT_NEUTRAL; 
        lx = 128; ly = 128; rx = 128; ry = 128; vendor = 0;
    }

    bool operator==(const HIDReport& o) const noexcept {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
    bool operator!=(const HIDReport& o) const noexcept { return !(*this == o); }
} __attribute__((packed));

// GameCube controller state (9 bytes)
struct GCController {
    uint8_t status, btn1, btn2;
    uint8_t stick_x, stick_y, cstick_x, cstick_y;
    uint8_t l_analog, r_analog;

    // Manually reset values to neutral state
    void reset() noexcept {
        status = 0x00; btn1 = 0; btn2 = 0;
        stick_x = 128; stick_y = 128; cstick_x = 128; cstick_y = 128;
        l_analog = 0; r_analog = 0;
    }
} __attribute__((packed));

// GameCube Hub report containing 4 controllers (37 bytes total)
struct GCHubReport {
    uint8_t      id;
    GCController p1, p2, p3, p4;

    // Manually reset hub to neutral state
    void reset() noexcept {
        id = 0x21; // Official GameCube adapter ID
        p1.reset(); p2.reset(); p3.reset(); p4.reset();
    }

    bool operator==(const GCHubReport& o) const noexcept {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
    bool operator!=(const GCHubReport& o) const noexcept { return !(*this == o); }
} __attribute__((packed));


// ── UDP Wire Packet ───────────────────────────────────────────────────────────
static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
static constexpr std::size_t HMAC_TAG_SIZE = 16;

// Main packet sent over UDP from PC to Raspberry Pi
struct Packet {
    uint32_t  magic;         // PROTO_MAGIC
    uint8_t   version;       // PROTO_VERSION
    uint8_t   flags;         // Flags bitmask
    uint16_t  autofire_mask; // Buttons to autofire (valid if FLAG_AUTOFIRE is set)
    uint32_t  seq;           // Monotonic sequence counter to prevent out-of-order packets
    uint64_t  ts_us;         // Sender timestamp in microseconds

    // Union grouping the payload modes (Max size: 37 bytes from GCHubReport)
    union {
        HIDReport   hori;
        GCHubReport gc;
    } payload;

    uint8_t   hmac[HMAC_TAG_SIZE]; // Truncated HMAC-SHA256 signature
} __attribute__((packed));

static constexpr std::size_t PACKET_SIZE      = sizeof(Packet);
static constexpr std::size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;

// ── Utilities ─────────────────────────────────────────────────────────────────

// Returns current time in microseconds
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Validates the packet's magic number and protocol version
inline bool packet_ok(const Packet& p) noexcept {
    return p.magic == PROTO_MAGIC && p.version == PROTO_VERSION;
}

} // namespace ns