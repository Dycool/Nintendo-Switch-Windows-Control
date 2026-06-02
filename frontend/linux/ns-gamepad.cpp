#include <iostream>
#include <chrono>
#include <cstdint>
#include <thread>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <atomic>

// Linux specific headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/joystick.h>
#include <sys/resource.h>
#include "sha256.h"

// Undefine conflicting button macros from linux/joystick.h to avoid naming conflicts
// with the custom Button enum defined in the ns namespace
#undef BTN_A
#undef BTN_B
#undef BTN_X
#undef BTN_Y
#undef BTN_L
#undef BTN_R
#undef BTN_TL
#undef BTN_TR
#undef BTN_SELECT
#undef BTN_START
#undef BTN_MODE
#undef BTN_THUMBL
#undef BTN_THUMBR

namespace ns {

// Protocol constants for communication with Raspberry Pi backend
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;  // 'NSWC' magic number for packet validation
static constexpr uint8_t  PROTO_VERSION = 1;             // Protocol version for compatibility checking
static constexpr uint16_t DEFAULT_PORT  = 7331;          // UDP port for sending gamepad data

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
    uint8_t   hmac[HMAC_TAG_SIZE];
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

/// Thread-safe gamepad state container using atomic operations
/// Allows main thread and sender thread to share input state without explicit locking
struct GamepadState {
    std::atomic<bool> buttons[16];     // Array of 16 button states (atomic for thread safety)
    std::atomic<int16_t> axes[8];      // Array of 8 analog axes (typically 2 sticks = 4 axes)

    GamepadState() {
        // Initialize all buttons as unpressed
        for(int i=0; i<16; ++i) buttons[i].store(false, std::memory_order_relaxed);
        // Initialize all axes to neutral (0)
        for(int i=0; i<8; ++i) axes[i].store(0, std::memory_order_relaxed);
    }

    /// Atomically copy current gamepad state to output arrays (relaxed memory ordering for performance)
    void copy_to(bool* dest_buttons, int16_t* dest_axes) const {
        for(int i=0; i<16; ++i) dest_buttons[i] = buttons[i].load(std::memory_order_relaxed);
        for(int i=0; i<8; ++i) dest_axes[i] = axes[i].load(std::memory_order_relaxed);
    }
};

/// Convert raw analog stick value to normalized 0-255 range with deadzone applied
/// @param val Raw axis value from joystick (-32768 to 32767)
/// @param invert If true, inverts the output (255 - scaled)
/// @param deadzone Threshold below which values are considered neutral (default 8000 for ~24% deadzone)
/// @return Normalized axis value (0-255, 128 = center/neutral)
uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    // If within deadzone, return center position
    if (val > -deadzone && val < deadzone) return 128;

    int scaled;
    if (val >= deadzone) {
        // Positive direction: scale from deadzone to max value (32767)
        scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    } else {
        // Negative direction: scale from deadzone to min value (-32768)
        scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    }
    
    // Clamp to valid range and optionally invert
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

/// Convert Linux joystick state to Switch Pro Controller HID report format
/// Maps Linux device input indices to Switch button/axis conventions
/// @param pad GamepadState containing Linux joystick input data
/// @return HIDReport formatted for USB transmission to Raspberry Pi
ns::HIDReport map_linux_js_to_switch(const GamepadState& pad) {
    ns::HIDReport r;
    r.reset();

    // Copy atomic values to local arrays for safe access during mapping
    bool b[16];
    int16_t a[8];
    pad.copy_to(b, a);

    // Map face buttons (indices 0-3 typically correspond to B, A, Y, X on most Linux gamepads)
    if (b[0]) r.buttons |= ns::BTN_B; 
    if (b[1]) r.buttons |= ns::BTN_A; 
    if (b[2]) r.buttons |= ns::BTN_Y; 
    if (b[3]) r.buttons |= ns::BTN_X; 

    // Map shoulder buttons (indices 4-5)
    if (b[4]) r.buttons |= ns::BTN_L; 
    if (b[5]) r.buttons |= ns::BTN_R; 
    
    // Map trigger buttons via analog axes (axes 2 and 5 on many gamepads)
    if (a[2] > 0) r.buttons |= ns::BTN_ZL;
    if (a[5] > 0) r.buttons |= ns::BTN_ZR;

    // Map menu/select buttons (indices 6-7)
    if (b[6]) r.buttons |= ns::BTN_MINUS;  
    if (b[7]) r.buttons |= ns::BTN_PLUS;   
    
    // Map stick button presses (indices 9-10)
    if (b[9]) r.buttons |= ns::BTN_LSTICK; 
    if (b[10])r.buttons |= ns::BTN_RSTICK; 

    // Special: Both sticks pressed simultaneously = HOME button
    if (b[9] && b[10]) {
        r.buttons |= ns::BTN_HOME;
        r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);  // Remove individual stick presses
    }

    // Special: MINUS + PLUS simultaneously = CAPTURE button
    if (b[6] && b[7]) {
        r.buttons |= ns::BTN_CAPTURE;
        r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);     // Remove individual menu presses
    }

    // Map D-pad via analog axes (axes 6-7 typically for Linux gamepads)
    // Threshold of 16000 out of 32767 range detects cardinal directions
    bool up    = (a[7] < -16000);
    bool down  = (a[7] >  16000);
    bool left  = (a[6] < -16000);
    bool right = (a[6] >  16000);

    // Set hat value based on combination of directional inputs
    if (up && right)       r.hat = ns::HAT_NE;
    else if (up && left)   r.hat = ns::HAT_NW;
    else if (down && right)r.hat = ns::HAT_SE;
    else if (down && left) r.hat = ns::HAT_SW;
    else if (up)           r.hat = ns::HAT_N;
    else if (down)         r.hat = ns::HAT_S;
    else if (left)         r.hat = ns::HAT_W;
    else if (right)        r.hat = ns::HAT_E;

    // Map analog sticks (axes 0-1 for left stick, 3-4 for right stick) with deadzone
    r.lx = apply_deadzone(a[0], false);
    r.ly = apply_deadzone(a[1], false);  
    r.rx = apply_deadzone(a[3], false);
    r.ry = apply_deadzone(a[4], false);  

    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP> [device_path]\n";
        return 1;
    }
    std::string host   = argv[1];
    std::string device = (argc >= 3) ? argv[2] : "/dev/input/js0";

    // Derive HMAC key from compiled-in default secret (always active)
    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    // Open Linux joystick device for reading raw input events
    int js_fd = open(device.c_str(), O_RDONLY);
    if (js_fd < 0) {
        std::cerr << "Error opening gamepad device: " << device << "\n";
        return 1;
    }
    std::cout << "Gamepad successfully opened at: " << device << "\n";

    // Set process priority to maximum (for low-latency input handling)
    // -20 is the highest priority (requires appropriate permissions)
    setpriority(PRIO_PROCESS, 0, -20);

    // Create UDP socket for sending packets to Raspberry Pi
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create UDP socket.\n";
        close(js_fd);
        return 1;
    }

    // Configure destination address (Raspberry Pi backend)
    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(ns::DEFAULT_PORT);
    
    // Convert IP address string to binary form
    if (inet_pton(AF_INET, host.c_str(), &dest.sin_addr) <= 0) {
        std::cerr << "Invalid IP address: " << host << "\n";
        close(js_fd);
        close(sock);
        return 1;
    }

    std::cout << "Streaming data to " << host << ":" << ns::DEFAULT_PORT << "... Press CTRL+C to exit.\n";

    // Shared gamepad state between main thread (reader) and sender thread (transmitter)
    GamepadState state;
    std::atomic<bool> running{true};

    // Spawn dedicated thread for transmitting UDP packets at fixed rate (2ms interval = 500 Hz)
    // This thread runs independently from the main event reading loop
    std::thread sender_thread([&]() {
        uint32_t packet_seq = 0;
        auto next_tick = std::chrono::steady_clock::now();

        while (running.load(std::memory_order_relaxed)) {
            // Busy-wait until next transmission time (high precision, no sleep overhead)
            // Uses atomic fence for minimal overhead
            while (std::chrono::steady_clock::now() < next_tick) {
                std::atomic_thread_fence(std::memory_order_relaxed);
            }

            // Construct UDP packet with current gamepad state
            ns::Packet pkt{};
            pkt.magic         = ns::PROTO_MAGIC;
            pkt.version       = ns::PROTO_VERSION;
            pkt.flags         = ns::FLAG_NONE;
            pkt.autofire_mask = 0;
            pkt.seq           = packet_seq++;
            pkt.ts_us         = ns::now_us();                    // Current timestamp for latency measurement
            pkt.report        = map_linux_js_to_switch(state);   // Convert state to Switch format
            {
                uint8_t full_hmac[32];
                hmac_sha256(hmac_key, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
                memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
            }

            // Send packet to Raspberry Pi via UDP
            sendto(sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));

            // Schedule next transmission in 2ms
            next_tick += std::chrono::milliseconds(2);
        }
    });

    // Main event loop: read joystick input events from Linux device
    // Processes both button presses and analog stick movements
    struct js_event event;
    while (read(js_fd, &event, sizeof(event)) > 0) {
        // Mask off the init flag to get the actual event type
        // JS_EVENT_INIT is set on first read to reflect current state
        uint8_t type = event.type & ~JS_EVENT_INIT;

        if (type == JS_EVENT_BUTTON) {
            // Button event: store button state (0=released, 1=pressed)
            if (event.number < 16) {
                state.buttons[event.number].store(event.value, std::memory_order_relaxed);
            }
        } else if (type == JS_EVENT_AXIS) {
            // Analog axis event: store axis value (-32768 to 32767)
            if (event.number < 8) {
                state.axes[event.number].store(event.value, std::memory_order_relaxed);
            }
        }
    }

    // Graceful shutdown: signal sender thread to stop
    running = false;
    if (sender_thread.joinable()) {
        sender_thread.join();  // Wait for sender thread to complete
    }

    // Clean up resources
    close(js_fd);
    close(sock);
    return 0;
}
