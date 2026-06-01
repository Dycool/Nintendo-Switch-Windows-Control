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

}

struct GamepadState {
    std::atomic<bool> buttons[16];
    std::atomic<int16_t> axes[8];

    GamepadState() {
        for(int i=0; i<16; ++i) buttons[i].store(false, std::memory_order_relaxed);
        for(int i=0; i<8; ++i) axes[i].store(0, std::memory_order_relaxed);
    }

    void copy_to(bool* dest_buttons, int16_t* dest_axes) const {
        for(int i=0; i<16; ++i) dest_buttons[i] = buttons[i].load(std::memory_order_relaxed);
        for(int i=0; i<8; ++i) dest_axes[i] = axes[i].load(std::memory_order_relaxed);
    }
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

    bool b[16];
    int16_t a[8];
    pad.copy_to(b, a);

    if (b[0]) r.buttons |= ns::BTN_B; 
    if (b[1]) r.buttons |= ns::BTN_A; 
    if (b[2]) r.buttons |= ns::BTN_Y; 
    if (b[3]) r.buttons |= ns::BTN_X; 

    if (b[4]) r.buttons |= ns::BTN_L; 
    if (b[5]) r.buttons |= ns::BTN_R; 
    
    if (a[2] > 0) r.buttons |= ns::BTN_ZL;
    if (a[5] > 0) r.buttons |= ns::BTN_ZR;

    if (b[6]) r.buttons |= ns::BTN_MINUS;  
    if (b[7]) r.buttons |= ns::BTN_PLUS;   
    if (b[9]) r.buttons |= ns::BTN_LSTICK; 
    if (b[10])r.buttons |= ns::BTN_RSTICK; 

    if (b[9] && b[10]) {
        r.buttons |= ns::BTN_HOME;
        r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK); 
    }

    if (b[6] && b[7]) {
        r.buttons |= ns::BTN_CAPTURE;
        r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS); 
    }

    bool up    = (a[7] < -16000);
    bool down  = (a[7] >  16000);
    bool left  = (a[6] < -16000);
    bool right = (a[6] >  16000);

    if (up && right)       r.hat = ns::HAT_NE;
    else if (up && left)   r.hat = ns::HAT_NW;
    else if (down && right)r.hat = ns::HAT_SE;
    else if (down && left) r.hat = ns::HAT_SW;
    else if (up)           r.hat = ns::HAT_N;
    else if (down)         r.hat = ns::HAT_S;
    else if (left)         r.hat = ns::HAT_W;
    else if (right)        r.hat = ns::HAT_E;

    r.lx = apply_deadzone(a[0], false);
    r.ly = apply_deadzone(a[1], false);  
    r.rx = apply_deadzone(a[3], false);
    r.ry = apply_deadzone(a[4], false);  

    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <RASPBERRY_PI_IP> [device_path (e.g. /dev/input/js0)]\n";
        return 1;
    }

    std::string host = argv[1];
    std::string device = (argc >= 3) ? argv[2] : "/dev/input/js0";

    int js_fd = open(device.c_str(), O_RDONLY);
    if (js_fd < 0) {
        std::cerr << "Error opening gamepad device: " << device << "\n";
        return 1;
    }
    std::cout << "Gamepad successfully opened at: " << device << "\n";

    setpriority(PRIO_PROCESS, 0, -20);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create UDP socket.\n";
        close(js_fd);
        return 1;
    }

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(ns::DEFAULT_PORT);
    if (inet_pton(AF_INET, host.c_str(), &dest.sin_addr) <= 0) {
        std::cerr << "Invalid IP address: " << host << "\n";
        close(js_fd);
        close(sock);
        return 1;
    }

    std::cout << "Streaming data to " << host << ":" << ns::DEFAULT_PORT << "... Press CTRL+C to exit.\n";

    GamepadState state;
    std::atomic<bool> running{true};

    // Thread for transmitting network reports via high-precision busy-wait polling loop
    std::thread sender_thread([&]() {
        uint32_t packet_seq = 0;
        auto next_tick = std::chrono::steady_clock::now();

        while (running.load(std::memory_order_relaxed)) {
            while (std::chrono::steady_clock::now() < next_tick) {
                std::atomic_thread_fence(std::memory_order_relaxed);
            }

            ns::Packet pkt{};
            pkt.magic         = ns::PROTO_MAGIC;
            pkt.version       = ns::PROTO_VERSION;
            pkt.flags         = ns::FLAG_NONE;
            pkt.autofire_mask = 0;
            pkt.seq           = packet_seq++;
            pkt.ts_us         = ns::now_us();
            pkt.report        = map_linux_js_to_switch(state);

            sendto(sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));

            next_tick += std::chrono::milliseconds(2);
        }
    });

    struct js_event event;
    // Main thread handling incoming OS joystick device event parsing
    while (read(js_fd, &event, sizeof(event)) > 0) {
        uint8_t type = event.type & ~JS_EVENT_INIT;

        if (type == JS_EVENT_BUTTON) {
            if (event.number < 16) {
                state.buttons[event.number].store(event.value, std::memory_order_relaxed);
            }
        } else if (type == JS_EVENT_AXIS) {
            if (event.number < 8) {
                state.axes[event.number].store(event.value, std::memory_order_relaxed);
            }
        }
    }

    running = false;
    if (sender_thread.joinable()) {
        sender_thread.join();
    }

    close(js_fd);
    close(sock);
    return 0;
}
