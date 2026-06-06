#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  protocol.hpp  –  Shared between frontend (PC) and backend (Pi)
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <cstring>
#include <chrono>

// Portable packing: GCC/Clang use __attribute__, MSVC uses #pragma pack
#ifdef _MSC_VER
  #define NS_PACKED_ATTR
  __pragma(pack(push, 1))
#else
  #define NS_PACKED_ATTR __attribute__((packed))
#endif

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
    FLAG_NONE       = 0x00,
    FLAG_RESET      = 0x01,  // Backend should zero all inputs immediately
    FLAG_AUTOFIRE   = 0x02,  // Autofire mask is currently active
    FLAG_SINGLE_PAD = 0x04,  // Web/mobile packet should only claim pad 1
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
} NS_PACKED_ATTR;

// 4-Player Composite Report
struct MultiReport {
    HIDReport p1, p2, p3, p4;

    void reset() noexcept {
        p1.reset(); p2.reset(); p3.reset(); p4.reset();
    }
} NS_PACKED_ATTR;




// Optional browser/WebSocket-only motion payload. The legacy UDP Packet below is
// intentionally unchanged, so existing UDP clients keep sending only buttons and
// sticks with PROTO_VERSION 4.
struct MotionReport {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;

    void reset() noexcept {
        ax = ay = az = 0;
        gx = gy = gz = 0;
    }
} NS_PACKED_ATTR;

struct ExtendedHIDReport {
    HIDReport    input;
    MotionReport motion;
    uint8_t      has_motion;
    uint8_t      reserved[3];

    void reset() noexcept {
        input.reset();
        motion.reset();
        has_motion = 0;
        reserved[0] = reserved[1] = reserved[2] = 0;
    }
} NS_PACKED_ATTR;

struct ExtendedMultiReport {
    ExtendedHIDReport p1, p2, p3, p4;

    void reset() noexcept {
        p1.reset(); p2.reset(); p3.reset(); p4.reset();
    }
} NS_PACKED_ATTR;

static constexpr uint8_t  WEB_PROTO_VERSION = 5; // WebSocket-only extended input + motion
static constexpr uint32_t RUMBLE_MAGIC      = 0x4E535652u; // 'NSVR'
static constexpr std::size_t WEB_PACKET_SIZE = 20 + sizeof(ExtendedMultiReport);

struct RumblePacket {
    uint32_t magic;       // RUMBLE_MAGIC
    uint8_t  subpad;      // 0..3, browser pad index inside the WS client
    uint8_t  low_freq;    // 0..255
    uint8_t  high_freq;   // 0..255
    uint8_t  duration_10ms;
} NS_PACKED_ATTR;

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
} NS_PACKED_ATTR;

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

#ifdef _MSC_VER
__pragma(pack(pop))
#endif