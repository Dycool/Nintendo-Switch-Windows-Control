#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  protocol.hpp  –  Shared between frontend clients and backend servers
//
//  Goals:
//    - Keep legacy 8-byte reports byte-compatible: HIDReport == 8 bytes.
//    - Keep legacy UDP packets byte-compatible: Packet == 68 bytes.
//    - Provide modern optional motion/rumble structs for 64-byte report servers
//      and upgraded UDP/WebSocket clients without breaking old clients.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <chrono>

// Portable packing: GCC/Clang use __attribute__, MSVC uses #pragma pack.
#ifdef _MSC_VER
  #define NS_PACKED_ATTR
  __pragma(pack(push, 1))
#else
  #define NS_PACKED_ATTR __attribute__((packed))
#endif

namespace ns {

// ── Tuning constants ─────────────────────────────────────────────────────────
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u; // 'NSWC'
static constexpr uint8_t  PROTO_VERSION = 4;           // Legacy 4-player UDP packet
static constexpr uint8_t  WEB_PROTO_VERSION = 5;       // Extended input + optional motion
static constexpr uint16_t DEFAULT_PORT  = 7331;
static constexpr int      WATCHDOG_MS   = 1200;
static constexpr int      HORI_UDP_HZ   = 250;
static constexpr int      PRO_UDP_HZ    = 66;
static constexpr int      HORI_UDP_INTERVAL_MS = 4;
static constexpr int      PRO_UDP_INTERVAL_MS  = 15;
static constexpr int      WRITER_HZ     = HORI_UDP_HZ;
static constexpr int      AUTOFIRE_HZ   = 12;

static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
static constexpr std::size_t HMAC_TAG_SIZE = 16;

static constexpr uint32_t RUMBLE_MAGIC = 0x4E535652u; // 'NSVR'
static constexpr uint32_t PRECISION_RUMBLE_MAGIC = 0x4E535648u; // 'NSVH'

// ── Buttons / hats / flags ───────────────────────────────────────────────────
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

enum Hat : uint8_t {
    HAT_N  = 0,
    HAT_NE = 1,
    HAT_E  = 2,
    HAT_SE = 3,
    HAT_S  = 4,
    HAT_SW = 5,
    HAT_W  = 6,
    HAT_NW = 7,
    HAT_NEUTRAL = 8,
};

enum Flags : uint8_t {
    FLAG_NONE       = 0x00,
    FLAG_RESET      = 0x01, // Backend should zero inputs immediately.
    FLAG_AUTOFIRE   = 0x02, // Autofire mask is active.
    FLAG_SINGLE_PAD = 0x04, // Web/mobile packet should only claim pad 1.
};

// Extended clients set this in HIDReport::vendor inside ExtendedHIDReport.
// Server code should clear vendor before writing legacy 8-byte reports.
static constexpr uint8_t EXT_PAD_PRESENT = 0x01;

// ── Legacy input reports ─────────────────────────────────────────────────────
// Exactly 8 bytes. This is the complete HID report written to the old
// legacy 8-byte gadget endpoints.
struct HIDReport {
    uint16_t buttons = 0;
    uint8_t  hat = HAT_NEUTRAL;
    uint8_t  lx = 128;
    uint8_t  ly = 128;
    uint8_t  rx = 128;
    uint8_t  ry = 128;
    uint8_t  vendor = 0;

    void reset() noexcept {
        buttons = 0;
        hat = HAT_NEUTRAL;
        lx = 128; ly = 128; rx = 128; ry = 128;
        vendor = 0;
    }

    bool operator==(const HIDReport& o) const noexcept {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
    bool operator!=(const HIDReport& o) const noexcept { return !(*this == o); }
} NS_PACKED_ATTR;

struct MultiReport {
    HIDReport p1, p2, p3, p4;
    void reset() noexcept { p1.reset(); p2.reset(); p3.reset(); p4.reset(); }
} NS_PACKED_ATTR;

// ── Optional motion / modern reports ─────────────────────────────────────────
struct MotionReport {
    int16_t ax = 0, ay = 0, az = 0;
    int16_t gx = 0, gy = 0, gz = 0;

    void reset() noexcept {
        ax = ay = az = 0;
        gx = gy = gz = 0;
    }
} NS_PACKED_ATTR;

struct ExtendedHIDReport {
    HIDReport input{};      // 8 bytes; input.vendor bit 0 = EXT_PAD_PRESENT.
    MotionReport motion{};  // 12 bytes.
    uint8_t has_motion = 0;
    uint8_t reserved[3]{};

    void reset() noexcept {
        input.reset();
        motion.reset();
        has_motion = 0;
        reserved[0] = reserved[1] = reserved[2] = 0;
    }
} NS_PACKED_ATTR;

struct ExtendedMultiReport {
    ExtendedHIDReport p1, p2, p3, p4;
    void reset() noexcept { p1.reset(); p2.reset(); p3.reset(); p4.reset(); }
} NS_PACKED_ATTR;

struct RumblePacket {
    uint32_t magic = RUMBLE_MAGIC;
    uint8_t  subpad = 0;        // 0..3 logical pad inside the client.
    uint8_t  low_freq = 0;      // 0..255
    uint8_t  high_freq = 0;     // 0..255
    uint8_t  duration_10ms = 0; // Duration units used by the web/UDP clients.
} NS_PACKED_ATTR;

struct PrecisionRumblePacket {
    uint32_t magic = PRECISION_RUMBLE_MAGIC;
    uint8_t  subpad = 0;        // 0..3 logical pad inside the client.
    uint8_t  low_freq = 0;      // Classic fallback weak/low motor, 0..255.
    uint8_t  high_freq = 0;     // Classic fallback strong/high motor, 0..255.
    uint8_t  duration_10ms = 0; // Classic fallback duration.
    uint8_t  precision[8]{};           // Raw precision rumble bytes: left[4], right[4].
    uint8_t  reserved[4]{};
} NS_PACKED_ATTR;

// ── Legacy UDP packet ────────────────────────────────────────────────────────
struct Packet {
    uint32_t    magic = PROTO_MAGIC;
    uint8_t     version = PROTO_VERSION;
    uint8_t     flags = FLAG_NONE;
    uint16_t    autofire_mask = 0;
    uint32_t    seq = 0;
    uint64_t    ts_us = 0;
    MultiReport report{};
    uint8_t     hmac[HMAC_TAG_SIZE]{};
} NS_PACKED_ATTR;

static constexpr std::size_t PACKET_SIZE      = sizeof(Packet);
static constexpr std::size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;

static constexpr std::size_t EXT_REPORT_SIZE  = sizeof(ExtendedHIDReport);
static constexpr std::size_t EXT_MULTI_SIZE   = sizeof(ExtendedMultiReport);
static constexpr std::size_t WEB_PACKET_SIZE  = 20 + sizeof(ExtendedMultiReport);

// ── Hard wire-layout checks ──────────────────────────────────────────────────
static_assert(sizeof(HIDReport) == 8,
              "HIDReport must stay 8 bytes for legacy gadget reports");
static_assert(sizeof(MultiReport) == 32,
              "MultiReport must be four 8-byte HID reports");
static_assert(sizeof(MotionReport) == 12,
              "MotionReport wire layout changed");
static_assert(sizeof(ExtendedHIDReport) == 24,
              "ExtendedHIDReport wire layout changed");
static_assert(sizeof(ExtendedMultiReport) == 96,
              "ExtendedMultiReport wire layout changed");
static_assert(sizeof(Packet) == 68,
              "Legacy Packet wire layout changed");
static_assert(sizeof(RumblePacket) == 8,
              "RumblePacket wire layout changed");
static_assert(sizeof(PrecisionRumblePacket) == 20,
              "PrecisionRumblePacket wire layout changed");

// ── Utilities ────────────────────────────────────────────────────────────────
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()
    );
}

inline bool packet_ok(const Packet& p) noexcept {
    return p.magic == PROTO_MAGIC && p.version == PROTO_VERSION;
}

} // namespace ns

#ifdef _MSC_VER
__pragma(pack(pop))
#endif
