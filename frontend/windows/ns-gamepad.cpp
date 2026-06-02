// Minimize Windows.h includes to reduce compilation time and avoid namespace pollution
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>           // Windows API core
#include <winsock2.h>          // Winsock2 for UDP socket operations (must come before ws2tcpip.h)
#include <ws2tcpip.h>          // Additional TCP/IP functionality (IPv4/IPv6 support)
#include <xinput.h>            // XInput API for Xbox 360/One controller support

#include <iostream>            // Standard I/O for console output
#include <chrono>              // High-resolution timing
#include <cstdint>             // Fixed-width integer types
#include <thread>              // Threading support
#include <algorithm>           // Standard algorithms (clamp)
#include "sha256.h"            // HMAC-SHA256 for packet authentication

namespace ns {

// Protocol constants for communication with Raspberry Pi backend
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;  // 'NSWC' magic number for packet validation
static constexpr uint8_t  PROTO_VERSION = 1;             // Protocol version for compatibility checking
static constexpr uint16_t DEFAULT_PORT  = 7331;          // UDP port for sending gamepad data
static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";

/// Nintendo Switch Pro Controller button bitmask layout
/// Bits correspond to specific buttons, allowing multiple buttons to be represented in a single uint16_t
enum Button : uint16_t {
    BTN_Y=1<<0, BTN_B=1<<1, BTN_A=1<<2, BTN_X=1<<3,      // Face buttons
    BTN_L=1<<4, BTN_R=1<<5, BTN_ZL=1<<6, BTN_ZR=1<<7,    // Shoulder buttons
    BTN_MINUS=1<<8, BTN_PLUS=1<<9, BTN_LSTICK=1<<10, BTN_RSTICK=1<<11,  // Special buttons
    BTN_HOME=1<<12, BTN_CAPTURE=1<<13,                    // System buttons
};

/// D-pad (hat switch) direction values
/// Represents the 8 cardinal directions plus neutral state
enum Hat : uint8_t {
    HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3,
    HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8,
};

/// Packed 8-byte HID input report structure
/// Layout must exactly match the USB gadget HID descriptor on the Raspberry Pi backend
#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0;   // Bitmask of pressed buttons (Button enum values)
    uint8_t  hat     = HAT_NEUTRAL;  // D-pad direction (Hat enum value)
    uint8_t  lx      = 128; // Left stick X axis (0-255, 128 = center)
    uint8_t  ly      = 128; // Left stick Y axis (0-255, 128 = center)
    uint8_t  rx      = 128; // Right stick X axis (0-255, 128 = center)
    uint8_t  ry      = 128; // Right stick Y axis (0-255, 128 = center)
    uint8_t  vendor  = 0;   // Vendor-specific data (unused)
    
    /// Reset all fields to default neutral state
    void reset() noexcept { buttons = 0; hat = HAT_NEUTRAL; lx = ly = rx = ry = 128; vendor = 0; }
};

/// Packet flag bits for controlling backend behavior
enum Flags : uint8_t { 
    FLAG_NONE=0x00,         // No special flags
    FLAG_RESET=0x01,        // Reset all inputs to neutral
    FLAG_AUTOFIRE=0x02      // Enable autofire for specified buttons
};

/// HMAC authentication tag (truncated HMAC-SHA256)
static constexpr size_t HMAC_TAG_SIZE = 16;

/// UDP wire packet structure sent from PC frontend to Raspberry Pi backend (~44 bytes with HMAC)
struct Packet {
    uint32_t  magic;         // PROTO_MAGIC for validation
    uint8_t   version;       // PROTO_VERSION for compatibility
    uint8_t   flags;         // Flags bitmask (Flags enum)
    uint16_t  autofire_mask; // Button bitmask for autofire (valid when FLAG_AUTOFIRE is set)
    uint32_t  seq;           // Monotonic sequence counter for packet ordering
    uint64_t  ts_us;         // Sender's steady_clock timestamp in microseconds (for latency calculation)
    HIDReport report;        // The actual gamepad input report
    uint8_t   hmac[HMAC_TAG_SIZE];  // HMAC-SHA256 tag (all-zero when no secret)
};
#pragma pack(pop)

/// Cached packet size for efficiency (avoids repeated sizeof calculations)
static constexpr size_t PACKET_SIZE      = sizeof(Packet);
static constexpr size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;

/// Get current time in microseconds using steady_clock (high precision, monotonic)
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

}

/// Convert raw XInput analog stick value to normalized 0-255 range with deadzone applied
/// @param val Raw axis value from XInput gamepad (typically -32768 to 32767 for SHORT type)
/// @param invert If true, inverts the output (255 - scaled). Used for Y-axis (down = positive)
/// @param deadzone Threshold below which values are considered neutral (default 8000 for ~24% deadzone)
/// @return Normalized axis value (0-255, 128 = center/neutral)
uint8_t apply_deadzone(SHORT val, bool invert = false, int deadzone = 8000) {
    // If within deadzone, return center position
    if (val > -deadzone && val < deadzone) return 128;

    int scaled;
    if (val >= deadzone) {
        // Positive direction: scale from deadzone to max value (32767)
        scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    } else {
        // Negative direction: scale from deadzone to min value (-32768)
        scaled = 128 - ((abs(val) - deadzone) * 128) / (32768 - deadzone);
    }
    
    // Clamp to valid range and optionally invert
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

/// Convert XInput gamepad state to Switch Pro Controller HID report format
/// Maps Xbox/XInput button layout to Nintendo Switch Pro Controller conventions
/// Note: Xbox and Switch button layouts differ (A/B and X/Y are swapped)
/// @param pad XINPUT_GAMEPAD structure containing current controller state
/// @return HIDReport formatted for USB transmission to Raspberry Pi
ns::HIDReport map_xinput_to_switch(const XINPUT_GAMEPAD& pad) {
    ns::HIDReport r;
    r.reset();

    // Map face buttons (note: Xbox layout differs from Switch)
    // Xbox A -> Switch B, Xbox B -> Switch A, etc.
    if (pad.wButtons & XINPUT_GAMEPAD_A) r.buttons |= ns::BTN_B; 
    if (pad.wButtons & XINPUT_GAMEPAD_B) r.buttons |= ns::BTN_A;
    if (pad.wButtons & XINPUT_GAMEPAD_X) r.buttons |= ns::BTN_Y;
    if (pad.wButtons & XINPUT_GAMEPAD_Y) r.buttons |= ns::BTN_X;

    // Map shoulder buttons
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  r.buttons |= ns::BTN_L;
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) r.buttons |= ns::BTN_R;
    
    // Map trigger buttons (analog values: 0-255)
    // Threshold of 128 represents ~50% trigger press
    if (pad.bLeftTrigger > 128)  r.buttons |= ns::BTN_ZL;
    if (pad.bRightTrigger > 128) r.buttons |= ns::BTN_ZR;

    // Map menu/select buttons
    if (pad.wButtons & XINPUT_GAMEPAD_BACK)  r.buttons |= ns::BTN_MINUS;
    if (pad.wButtons & XINPUT_GAMEPAD_START) r.buttons |= ns::BTN_PLUS;
    
    // Map stick button presses (thumbstick clicks)
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB)  r.buttons |= ns::BTN_LSTICK;
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) r.buttons |= ns::BTN_RSTICK;

    // Special: Both sticks pressed simultaneously = HOME button
    if ((pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) && (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)) {
        r.buttons |= ns::BTN_HOME;
        r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);  // Remove individual stick presses
    }

    // Special: BACK + START simultaneously = CAPTURE button
    if ((pad.wButtons & XINPUT_GAMEPAD_BACK) && (pad.wButtons & XINPUT_GAMEPAD_START)) {
        r.buttons |= ns::BTN_CAPTURE;
        r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);     // Remove individual menu presses
    }

    // Map D-pad (hat switch) directions
    bool up    = (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP);
    bool down  = (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
    bool left  = (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
    bool right = (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);

    // Set hat value based on combination of directional inputs
    if (up && right)       r.hat = ns::HAT_NE;
    else if (up && left)   r.hat = ns::HAT_NW;
    else if (down && right)r.hat = ns::HAT_SE;
    else if (down && left) r.hat = ns::HAT_SW;
    else if (up)           r.hat = ns::HAT_N;
    else if (down)         r.hat = ns::HAT_S;
    else if (left)         r.hat = ns::HAT_W;
    else if (right)        r.hat = ns::HAT_E;

    // Map analog sticks with deadzone
    // Note: Y-axis is inverted (true) because XInput's positive Y is up, but normalized form expects positive = down
    r.lx = apply_deadzone(pad.sThumbLX, false);     // Left stick X (not inverted)
    r.ly = apply_deadzone(pad.sThumbLY, true);      // Left stick Y (inverted)
    r.rx = apply_deadzone(pad.sThumbRX, false);     // Right stick X (not inverted)
    r.ry = apply_deadzone(pad.sThumbRY, true);      // Right stick Y (inverted)

    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP>\n";
        return 1;
    }
    std::string host = argv[1];
    uint16_t port    = ns::DEFAULT_PORT;

    // Derive HMAC key from compiled-in default secret (always active)
    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    // Elevate process and thread priority for real-time input polling
    // HIGH_PRIORITY_CLASS ensures the process gets more CPU time
    // THREAD_PRIORITY_TIME_CRITICAL ensures minimal latency for input reading
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Initialize Winsock2 library (required for socket operations on Windows)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);  // Request Winsock 2.2

    // Create UDP socket for sending packets to Raspberry Pi
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    // Set up address info hints for DNS resolution
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;      // IPv4
    hints.ai_socktype = SOCK_DGRAM; // UDP

    // Convert port number to string for getaddrinfo
    char port_buf[8];
    snprintf(port_buf, sizeof(port_buf), "%u", port);
    
    // Resolve hostname to IP address
    getaddrinfo(host.c_str(), port_buf, &hints, &res);
    
    // Copy resolved address to destination socket address structure
    sockaddr_in dest{};
    memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);  // Free DNS lookup results

    std::cout << "Started... Press CTRL+C to exit.\n";

    uint32_t seq = 0;              // Packet sequence counter
    XINPUT_STATE state;            // XInput controller state structure

    // Main polling loop: continuously read XInput state and send to Raspberry Pi
    while (true) {
        // Clear state structure
        ZeroMemory(&state, sizeof(XINPUT_STATE));
        
        // Attempt to read XInput gamepad state (controller index 0)
        if (XInputGetState(0, &state) == ERROR_SUCCESS) {
            
            // Convert XInput format to Switch HID format
            ns::HIDReport report = map_xinput_to_switch(state.Gamepad);

            // Construct UDP packet
            ns::Packet pkt{};
            pkt.magic         = ns::PROTO_MAGIC;
            pkt.version       = ns::PROTO_VERSION;
            pkt.flags         = ns::FLAG_NONE;
            pkt.seq           = seq++;
            pkt.ts_us         = ns::now_us();
            pkt.report        = report;
            {
                uint8_t full_hmac[32];
                hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
            }

            // Send packet to Raspberry Pi
            sendto(sock, (const char*)&pkt, (int)ns::PACKET_SIZE, 0, (const sockaddr*)&dest, sizeof(dest));
            
            // Maintain consistent transmission rate (~500 Hz at 2ms interval)
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); 
            
        } else {
            // Controller not connected: send neutral state packet to signal disconnection
            ns::Packet pkt{};
            pkt.magic         = ns::PROTO_MAGIC;
            pkt.version       = ns::PROTO_VERSION;
            pkt.flags         = ns::FLAG_NONE;
            pkt.seq           = seq++;
            pkt.ts_us         = ns::now_us();
            pkt.report.reset();  // All inputs to neutral/zero
            {
                uint8_t full_hmac[32];
                hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
            }

            // Send disconnection packet
            sendto(sock, (const char*)&pkt, (int)ns::PACKET_SIZE, 0, (const sockaddr*)&dest, sizeof(dest));
            
            // Use longer sleep when disconnected to avoid spamming connection attempts
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    // Cleanup (unreachable in normal operation due to infinite loop, but good practice)
    closesocket(sock);
    WSACleanup();  // Shut down Winsock2
    return 0;
}
