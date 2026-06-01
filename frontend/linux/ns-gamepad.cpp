#include <iostream>
#include <chrono>
#include <cstdint>
#include <thread>
#include <algorithm>
#include <cstring>
#include <cmath>

// Linux specific headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/joystick.h>
#include <sys/resource.h>

namespace ns {

static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;
static constexpr uint8_t  PROTO_VERSION = 1;
static constexpr uint16_t DEFAULT_PORT  = 7331;

enum Button : uint16_t {
    BTN_Y=1<<0, BTN_B=1<<1, BTN_A=1<<2, BTN_X=1<<3,
    BTN_L=1<<4, BTN_R=1<<5, BTN_ZL=1<<6, BTN_ZR=1<<7,
    BTN_MINUS=1<<8, BTN_PLUS=1<<9, BTN_LSTICK=1<<10, BTN_RSTICK=1<<11,
    BTN_HOME=1<<12, BTN_CAPTURE=1<<13,
};

enum Hat : uint8_t {
    HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3,
    HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8,
};

#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0;
    uint8_t  hat     = HAT_NEUTRAL;
    uint8_t  lx      = 128;
    uint8_t  ly      = 128;
    uint8_t  rx      = 128;
    uint8_t  ry      = 128;
    uint8_t  vendor  = 0;
    void reset() noexcept { buttons = 0; hat = HAT_NEUTRAL; lx = ly = rx = ry = 128; vendor = 0; }
};

enum Flags : uint8_t { FLAG_NONE=0x00, FLAG_RESET=0x01, FLAG_AUTOFIRE=0x02 };

struct Packet {
    uint32_t  magic;
    uint8_t   version;
    uint8_t   flags;
    uint16_t  autofire_mask;
    uint32_t  seq;
    uint64_t  ts_us;
    HIDReport report;
};
#pragma pack(pop)

static constexpr size_t PACKET_SIZE = sizeof(Packet);

inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace ns

// State struct to hold current gamepad status (Linux Joystick API is event-driven)
struct GamepadState {
    bool buttons[16] = {false};
    int16_t axes[8] = {0};
};

uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;

    int scaled;
    if (val >= deadzone) {
        scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    } else {
        scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    }
    
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

ns::HIDReport map_linux_js_to_switch(const GamepadState& pad) {
    ns::HIDReport r;
    r.reset();

    // Standard Linux xpad driver mapping for Xbox controllers
    if (pad.buttons[0]) r.buttons |= ns::BTN_B; // A
    if (pad.buttons[1]) r.buttons |= ns::BTN_A; // B
    if (pad.buttons[2]) r.buttons |= ns::BTN_Y; // X
    if (pad.buttons[3]) r.buttons |= ns::BTN_X; // Y

    if (pad.buttons[4]) r.buttons |= ns::BTN_L; // LB
    if (pad.buttons[5]) r.buttons |= ns::BTN_R; // RB
    
    // Linux maps LT and RT to axes 2 and 5. Value is -32768 (unpressed) to 32767 (fully pressed)
    if (pad.axes[2] > 0) r.buttons |= ns::BTN_ZL;
    if (pad.axes[5] > 0) r.buttons |= ns::BTN_ZR;

    if (pad.buttons[6]) r.buttons |= ns::BTN_MINUS;  // Back
    if (pad.buttons[7]) r.buttons |= ns::BTN_PLUS;   // Start
    if (pad.buttons[9]) r.buttons |= ns::BTN_LSTICK; // L3
    if (pad.buttons[10])r.buttons |= ns::BTN_RSTICK; // R3

    if (pad.buttons[9] && pad.buttons[10]) {
        r.buttons |= ns::BTN_HOME;
        r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK); 
    }

    if (pad.buttons[6] && pad.buttons[7]) {
        r.buttons |= ns::BTN_CAPTURE;
        r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS); 
    }

    // D-PAD on Linux is usually mapped to axes 6 (X) and 7 (Y)
    bool up    = (pad.axes[7] < -16000);
    bool down  = (pad.axes[7] >  16000);
    bool left  = (pad.axes[6] < -16000);
    bool right = (pad.axes[6] >  16000);

    if (up && right)       r.hat = ns::HAT_NE;
    else if (up && left)   r.hat = ns::HAT_NW;
    else if (down && right)r.hat = ns::HAT_SE;
    else if (down && left) r.hat = ns::HAT_SW;
    else if (up)           r.hat = ns::HAT_N;
    else if (down)         r.hat = ns::HAT_S;
    else if (left)         r.hat = ns::HAT_W;
    else if (right)        r.hat = ns::HAT_E;

    // Thumbsticks are axes 0 (LX), 1 (LY), 3 (RX), 4 (RY)
    r.lx = apply_deadzone(pad.axes[0], false);
    r.ly = apply_deadzone(pad.axes[1], true);
    r.rx = apply_deadzone(pad.axes[3], false);
    r.ry = apply_deadzone(pad.axes[4], true);

    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ./gamepad <RASPBERRY PI IP>\n";
        return 1;
    }

    std::string host = argv[1];
    uint16_t port    = ns::DEFAULT_PORT;

    // Attempt to set high priority (Requires sudo for full effect on Linux)
    setpriority(PRIO_PROCESS, 0, -20);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create socket.\n";
        return 1;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    
    char port_buf[8];
    snprintf(port_buf, sizeof(port_buf), "%u", port);
    if (getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0) {
        std::cerr << "Failed to resolve host.\n";
        close(sock);
        return 1;
    }

    sockaddr_in dest{};
    memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    std::cout << "Started... Press CTRL+C to exit.\n";

    uint32_t seq = 0;
    GamepadState state;
    
    // Open gamepad device in non-blocking mode
    const char* js_dev = "/dev/input/js0";
    int js_fd = open(js_dev, O_RDONLY | O_NONBLOCK);

    while (true) {
        bool connected = false;

        if (js_fd >= 0) {
            js_event event;
            // Read all queued events
            while (read(js_fd, &event, sizeof(event)) > 0) {
                event.type &= ~JS_EVENT_INIT; // Ignore synthetic init flags
                
                if (event.type == JS_EVENT_BUTTON && event.number < 16) {
                    state.buttons[event.number] = event.value;
                } else if (event.type == JS_EVENT_AXIS && event.number < 8) {
                    state.axes[event.number] = event.value;
                }
            }

            // Check if device disconnected
            if (errno != EAGAIN) {
                close(js_fd);
                js_fd = -1;
            } else {
                connected = true;
            }
        } else {
            // Attempt reconnect quietly
            js_fd = open(js_dev, O_RDONLY | O_NONBLOCK);
        }

        ns::Packet pkt{};
        pkt.magic   = ns::PROTO_MAGIC;
        pkt.version = ns::PROTO_VERSION;
        pkt.flags   = ns::FLAG_NONE;
        pkt.seq     = seq++;
        pkt.ts_us   = ns::now_us();

        if (connected) {
            pkt.report = map_linux_js_to_switch(state);
            sendto(sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); 
        } else {
            pkt.report.reset();
            sendto(sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    if (js_fd >= 0) close(js_fd);
    close(sock);
    return 0;
}