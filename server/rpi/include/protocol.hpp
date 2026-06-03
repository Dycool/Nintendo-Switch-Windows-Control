#pragma once
#include <cstdint>
#include <cstring>
#include <chrono>

namespace ns {

static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;
static constexpr uint8_t  PROTO_VERSION = 2;
static constexpr uint16_t DEFAULT_PORT  = 7331;
static constexpr int      WRITER_HZ     = 250;
static constexpr int      AUTOFIRE_HZ   = 12;

// ── HORI Structs ──
struct HIDReport {
    uint16_t buttons = 0;
    uint8_t  hat     = 8;
    uint8_t  lx      = 128;
    uint8_t  ly      = 128;
    uint8_t  rx      = 128;
    uint8_t  ry      = 128;
    uint8_t  vendor  = 0;

    void reset() noexcept { *this = HIDReport{}; hat = 8; lx = ly = rx = ry = 128; }
    bool operator==(const HIDReport& o) const noexcept { return std::memcmp(this, &o, sizeof(*this)) == 0; }
} __attribute__((packed));

// ── GameCube Structs ──
struct GCController {
    uint8_t status = 0x10, btn1 = 0, btn2 = 0;
    uint8_t stick_x = 128, stick_y = 128, cstick_x = 128, cstick_y = 128;
    uint8_t l_analog = 0, r_analog = 0;
} __attribute__((packed));

struct GCHubReport {
    uint8_t      id = 0x21;
    GCController p1, p2, p3, p4;
    void reset() noexcept { *this = GCHubReport{}; }
} __attribute__((packed));

// ── Wire Packet ──
static constexpr std::size_t HMAC_TAG_SIZE = 16;

struct Packet {
    uint32_t  magic;
    uint8_t   version;
    uint8_t   flags;
    uint16_t  autofire_mask;
    uint32_t  seq;
    uint64_t  ts_us;
    
    struct {
        union {
            HIDReport   hori;
            GCHubReport gc;
        } u;
    } payload;
    
    uint8_t   hmac[HMAC_TAG_SIZE];
} __attribute__((packed));

static constexpr std::size_t PACKET_SIZE = sizeof(Packet);
static constexpr std::size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;

inline uint64_t now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline bool packet_ok(const Packet& p) noexcept { return p.magic == PROTO_MAGIC && p.version == PROTO_VERSION; }

} // namespace ns
