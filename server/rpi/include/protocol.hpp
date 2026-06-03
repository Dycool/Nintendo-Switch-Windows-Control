#pragma once
#include <cstdint>
#include <cstring>
#include <chrono>

namespace ns {

static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u; // 'NSWC'
static constexpr uint8_t  PROTO_VERSION = 2;           // Bumped for unified protocol
static constexpr uint16_t DEFAULT_PORT  = 7331;
static constexpr int      HEARTBEAT_MS  = 50;
static constexpr int      WATCHDOG_MS   = 1200;
static constexpr int      WRITER_HZ     = 250;
static constexpr int      AUTOFIRE_HZ   = 12;

// ── 1. HORI Controller Structs ──
enum Button : uint16_t {
    BTN_Y       = 1u <<  0, BTN_B       = 1u <<  1, BTN_A       = 1u <<  2, BTN_X       = 1u <<  3,
    BTN_L       = 1u <<  4, BTN_R       = 1u <<  5, BTN_ZL      = 1u <<  6, BTN_ZR      = 1u <<  7,
    BTN_MINUS   = 1u <<  8, BTN_PLUS    = 1u <<  9, BTN_LSTICK  = 1u << 10, BTN_RSTICK  = 1u << 11,
    BTN_HOME    = 1u << 12, BTN_CAPTURE = 1u << 13,
};

enum Hat : uint8_t {
    HAT_N = 0, HAT_NE = 1, HAT_E = 2, HAT_SE = 3,
    HAT_S = 4, HAT_SW = 5, HAT_W = 6, HAT_NW = 7, HAT_NEUTRAL = 8,
};

struct HIDReport {
    uint16_t buttons = 0;
    uint8_t  hat     = HAT_NEUTRAL;
    uint8_t  lx      = 128;
    uint8_t  ly      = 128;
    uint8_t  rx      = 128;
    uint8_t  ry      = 128;
    uint8_t  vendor  = 0;

    void reset() noexcept { *this = HIDReport{}; }
    bool operator==(const HIDReport& o) const noexcept { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    bool operator!=(const HIDReport& o) const noexcept { return !(*this == o); }
} __attribute__((packed));

static_assert(sizeof(HIDReport) == 8, "HIDReport must be 8 bytes");

// ── 2. GameCube Structs ──
struct GCController {
    uint8_t status = 0x10, btn1 = 0, btn2 = 0;
    uint8_t stick_x = 128, stick_y = 128, cstick_x = 128, cstick_y = 128;
    uint8_t l_analog = 0, r_analog = 0;
} __attribute__((packed));

struct GCHubReport {
    uint8_t      id = 0x21;
    GCController p1, p2, p3, p4;

    void reset() noexcept { *this = GCHubReport{}; }
    bool operator==(const GCHubReport& o) const noexcept { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    bool operator!=(const GCHubReport& o) const noexcept { return !(*this == o); }
} __attribute__((packed));

static_assert(sizeof(GCHubReport) == 37, "GCHubReport must be 37 bytes");

// ── 3. Wire Packet ──
enum Flags : uint8_t { FLAG_NONE = 0x00, FLAG_RESET = 0x01, FLAG_AUTOFIRE = 0x02 };

static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
static constexpr std::size_t HMAC_TAG_SIZE = 16;

struct Packet {
    uint32_t  magic;
    uint8_t   version;
    uint8_t   flags;
    uint16_t  autofire_mask;
    uint32_t  seq;
    uint64_t  ts_us;
    
    // Dynamically size the payload for UDP transit
    union {
        HIDReport   hori;
        GCHubReport gc;
    } payload;
    
    uint8_t   hmac[HMAC_TAG_SIZE];
} __attribute__((packed));

static constexpr std::size_t PACKET_SIZE = sizeof(Packet);
static constexpr std::size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;

inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}
inline bool packet_ok(const Packet& p) noexcept { return p.magic == PROTO_MAGIC && p.version == PROTO_VERSION; }

} // namespace ns
