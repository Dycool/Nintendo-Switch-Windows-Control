// test_probe_dual.cpp — Standalone Dual Pro Controller USB test probe
// Build: g++ -O2 -std=c++17 -o test_probe test_probe_dual_instant.cpp -lpthread
// Run:   sudo ./test_probe [-v]
//
// This version exposes TWO HID interfaces in the same USB gadget:
//   /dev/hidg0 = virtual Pro Controller #1
//   /dev/hidg1 = virtual Pro Controller #2
//
// Test mode:
//   /dev/hidg0 = virtual Pro Controller #1: connects and sends neutral reports only.
//   /dev/hidg1 = virtual Pro Controller #2: sends reports at 250 Hz with ultra-short
//                 alternating R pulses, so there are no long button holds.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <ctime>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/poll.h>

using Clock = std::chrono::steady_clock;
using us = std::chrono::microseconds;
using ms = std::chrono::milliseconds;

// ── Configuration ───────────────────────────────────────────────────────────
static constexpr const char* GADGET_DIR  = "/sys/kernel/config/usb_gadget/ns_probe";
static constexpr const char* ORIG_GADGET = "/sys/kernel/config/usb_gadget/ns_ctrl";
static constexpr int         WRITER_HZ   = 250;
static constexpr size_t      REPORT_SIZE = 64;
static constexpr int         CONTROLLER_COUNT = 2;
static constexpr int         ACTIVE_INPUT_CONTROLLER = 1; // 0 = hidg0, 1 = hidg1
static constexpr bool        RATE_LOGS = true;

static std::atomic<bool> g_running{true};
static bool g_verbose = false;

// ── Pro Controller Protocol Constants ───────────────────────────────────────
static constexpr uint8_t RID_INPUT_STANDARD = 0x30;
static constexpr uint8_t RID_INPUT_SUBCMD   = 0x21;
static constexpr uint8_t RID_OUTPUT_RUMBLE  = 0x10;
static constexpr uint8_t RID_OUTPUT_CMD     = 0x01;

static constexpr uint8_t CMD_BT_MANUAL_PAIRING   = 0x01;
static constexpr uint8_t CMD_GET_DEVICE_INFO     = 0x02;
static constexpr uint8_t CMD_SET_DATA_FORMAT     = 0x03;
static constexpr uint8_t CMD_TRIGGER_BUTTONS     = 0x04;
static constexpr uint8_t CMD_SET_SHIP_MODE       = 0x08;
static constexpr uint8_t CMD_SPI_FLASH_READ      = 0x10;
static constexpr uint8_t CMD_SPI_FLASH_WRITE     = 0x11;
static constexpr uint8_t CMD_SET_NFC_IR_CONFIG   = 0x21;
static constexpr uint8_t CMD_SET_PLAYER_LIGHTS   = 0x30;
static constexpr uint8_t CMD_ENABLE_IMU          = 0x40;
static constexpr uint8_t CMD_SET_IMU_SENS        = 0x41;
static constexpr uint8_t CMD_ENABLE_VIBRATION    = 0x48;

#define PACKED __attribute__((packed))

// ── Pro Controller Report Layouts ──────────────────────────────────────────
struct PACKED InputReport30 {
    uint8_t id;
    uint8_t timer;
    uint8_t conn_info;
    uint8_t buttons[3];
    uint8_t left_stick[3];
    uint8_t right_stick[3];
    uint8_t vibrator;
    int16_t accel_x_0, accel_y_0, accel_z_0;
    int16_t gyro_x_0,  gyro_y_0,  gyro_z_0;
    int16_t accel_x_1, accel_y_1, accel_z_1;
    int16_t gyro_x_1,  gyro_y_1,  gyro_z_1;
    int16_t accel_x_2, accel_y_2, accel_z_2;
    int16_t gyro_x_2,  gyro_y_2,  gyro_z_2;
    uint8_t vendor_rest[15];
};
static_assert(sizeof(InputReport30) == 64, "InputReport30 must be 64 bytes");

struct PACKED InputReport21 {
    uint8_t id;
    uint8_t timer;
    uint8_t conn_info;
    uint8_t buttons[3];
    uint8_t left_stick[3];
    uint8_t right_stick[3];
    uint8_t vibrator;
    uint8_t ack;
    uint8_t subcmd_id;
    uint8_t reply_data[49];
};
static_assert(sizeof(InputReport21) == 64, "InputReport21 must be 64 bytes");

// ── Pro Controller HID Report Descriptor ────────────────────────────────────
static const uint8_t PRO_CONTROLLER_HID_DESC[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xa1, 0x01,
    0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0a, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x0a, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0b, 0x29, 0x0e, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0b, 0x01,
    0x00, 0x01, 0x00, 0xa1, 0x00, 0x0b, 0x30, 0x00,
    0x01, 0x00, 0x0b, 0x31, 0x00, 0x01, 0x00, 0x0b,
    0x32, 0x00, 0x01, 0x00, 0x0b, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xc0, 0x0b,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19,
    0x0f, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95,
    0x34, 0x81, 0x03, 0x06, 0x00, 0xff, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3f, 0x81, 0x03,
    0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3f,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08,
    0x95, 0x3f, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3f, 0x91, 0x83, 0x85, 0x80,
    0x09, 0x05, 0x75, 0x08, 0x95, 0x3f, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3f,
    0x91, 0x83, 0xc0
};
static constexpr size_t HID_DESC_LEN = sizeof(PRO_CONTROLLER_HID_DESC);

// ── Per-controller identity ─────────────────────────────────────────────────
// Big-endian-ish MAC order used in the 0x81 init ACK.
static const uint8_t CTRL_MAC_BE[CONTROLLER_COUNT][6] = {
    {0x98, 0xB6, 0xE9, 0x11, 0x22, 0x33},
    {0x98, 0xB6, 0xE9, 0x11, 0x22, 0x34},
};

static const char* CTRL_SERIAL[CONTROLLER_COUNT] = {
    "98B6E9112233",
    "98B6E9112234",
};

// ── Virtual SPI Flash ──────────────────────────────────────────────────────
static constexpr size_t SPI_FLASH_SIZE = 0x10000;
static uint8_t g_spi_flash[CONTROLLER_COUNT][SPI_FLASH_SIZE];
static bool    g_spi_initialized[CONTROLLER_COUNT] = {false, false};

static void init_spi_flash(int ctrl) {
    if (ctrl < 0 || ctrl >= CONTROLLER_COUNT) return;
    if (g_spi_initialized[ctrl]) return;

    uint8_t* flash = g_spi_flash[ctrl];
    memset(flash, 0xFF, SPI_FLASH_SIZE);

    // Serial number area.
    const char* serial = CTRL_SERIAL[ctrl];
    memcpy(flash + 0x6080, serial, strlen(serial) + 1);

    // Some useful SPI bytes around 0x6000. These avoid returning pure 0xFF
    // to common init reads.
    flash[0x6012] = 0x03;
    flash[0x6013] = 0xA0;
    flash[0x601B] = 0x02;

    auto pack12 = [](uint16_t val, uint8_t& b0, uint8_t& b1) {
        b0 = val & 0xFF;
        b1 = (b1 & 0xF0) | ((val >> 8) & 0x0F);
    };

    // Basic neutral stick calibration.
    uint8_t cal_left[9] = {};
    pack12(0x800, cal_left[0], cal_left[1]);
    pack12(0x800, cal_left[1], cal_left[2]);
    pack12(0xF00, cal_left[3], cal_left[4]);
    pack12(0xF00, cal_left[4], cal_left[5]);
    cal_left[6] = 0x18;

    uint8_t cal_right[9] = {};
    pack12(0x800, cal_right[0], cal_right[1]);
    pack12(0x800, cal_right[1], cal_right[2]);
    pack12(0xF00, cal_right[3], cal_right[4]);
    pack12(0xF00, cal_right[4], cal_right[5]);
    cal_right[6] = 0x18;

    memcpy(flash + 0x603D, cal_left, sizeof(cal_left));
    memcpy(flash + 0x6046, cal_right, sizeof(cal_right));

    // Controller color area, read by the Switch at 0x6050.
    // #1 black-ish, #2 slightly different so cached color/identity is not identical.
    if (ctrl == 0) {
        flash[0x6050] = 0x32; flash[0x6051] = 0x32; flash[0x6052] = 0x32;
        flash[0x6053] = 0xFF; flash[0x6054] = 0xFF; flash[0x6055] = 0xFF;
        flash[0x6056] = 0x32; flash[0x6057] = 0x32; flash[0x6058] = 0x32;
        flash[0x6059] = 0xFF; flash[0x605A] = 0xFF; flash[0x605B] = 0xFF;
    } else {
        flash[0x6050] = 0x20; flash[0x6051] = 0x20; flash[0x6052] = 0x20;
        flash[0x6053] = 0x80; flash[0x6054] = 0x80; flash[0x6055] = 0x80;
        flash[0x6056] = 0x20; flash[0x6057] = 0x20; flash[0x6058] = 0x20;
        flash[0x6059] = 0x80; flash[0x605A] = 0x80; flash[0x605B] = 0x80;
    }
    flash[0x605C] = 0x00;

    // Basic IMU calibration.
    struct { int16_t val; uint16_t addr; } imu_cal[] = {
        { 0,      0x6098 }, { 0,      0x609A }, { 0,      0x609C },
        { 0,      0x609E }, { 0,      0x60A0 }, { 0,      0x60A2 },
        { 0x1000, 0x60A4 }, { 0x1000, 0x60A6 }, { 0x1000, 0x60A8 },
        { 0x2000, 0x60AA }, { 0x2000, 0x60AC }, { 0x2000, 0x60AE },
    };
    for (auto& c : imu_cal) {
        flash[c.addr]     = c.val & 0xFF;
        flash[c.addr + 1] = (c.val >> 8) & 0xFF;
    }

    g_spi_initialized[ctrl] = true;
}

// ── Helpers ────────────────────────────────────────────────────────────────
static void fill_neutral_controls(InputReport30& r) {
    r.conn_info = 0x8E;

    // Neutral sticks: packed 12-bit X/Y values, both around 0x800.
    r.left_stick[0]  = 0x00;
    r.left_stick[1]  = 0x08;
    r.left_stick[2]  = 0x80;
    r.right_stick[0] = 0x00;
    r.right_stick[1] = 0x08;
    r.right_stick[2] = 0x80;

    r.vibrator = 0x00;
}

static void fill_neutral_controls(InputReport21& r) {
    r.conn_info = 0x8E;

    r.left_stick[0]  = 0x00;
    r.left_stick[1]  = 0x08;
    r.left_stick[2]  = 0x80;
    r.right_stick[0] = 0x00;
    r.right_stick[1] = 0x08;
    r.right_stick[2] = 0x80;

    r.vibrator = 0x00;
}

static void set_identity_in_0x81(uint8_t* resp_81, int ctrl) {
    const uint8_t* mac = CTRL_MAC_BE[ctrl];

    resp_81[4] = mac[0];
    resp_81[5] = mac[1];
    resp_81[6] = mac[2];
    resp_81[7] = mac[3];
    resp_81[8] = mac[4];
    resp_81[9] = mac[5];
}

// ── Build Device Info ──────────────────────────────────────────────────────
static void build_get_device_info_response(uint8_t* out, int ctrl) {
    memset(out, 0, 36);
    out[0] = 0x03;  // Firmware Major
    out[1] = 0x48;  // Firmware Minor
    out[2] = 0x03;  // Controller Type: 0x03 = Pro Controller
    out[3] = 0x02;  // Hardware model

    // Device info uses the reverse byte order compared to the 0x81 init ACK.
    const uint8_t* mac = CTRL_MAC_BE[ctrl];
    out[4] = mac[5];
    out[5] = mac[4];
    out[6] = mac[3];
    out[7] = mac[2];
    out[8] = mac[1];
    out[9] = mac[0];

    out[10] = 0x03; // Connection info
    out[11] = 0x01; // SPI color flag
}

struct ControllerRuntime {
    int fd = -1;
    int ctrl = 0;
    uint8_t timer = 0;
    bool full_report_enabled = false;
    bool pending_subcmd_reply = false;
    InputReport21 pending_reply = {};
};

// ── Create the subcommand reply packet ─────────────────────────────────────
static int handle_subcommand(ControllerRuntime& rt, uint8_t subcmd, const uint8_t* cmd_data, size_t cmd_len, InputReport21* reply) {
    memset(reply->reply_data, 0, sizeof(reply->reply_data));

    reply->ack = 0x80;
    reply->subcmd_id = subcmd;

    switch (subcmd) {
    case CMD_BT_MANUAL_PAIRING: {
        // Minimal pairing-style ACK. This prevents the repeated ack=0x00 loop.
        reply->ack = 0x81;
        reply->reply_data[0] = 0x03;
        reply->reply_data[1] = 0x01;
        return 2;
    }

    case CMD_GET_DEVICE_INFO: {
        uint8_t info[36];
        build_get_device_info_response(info, rt.ctrl);
        reply->ack = 0x82;
        memcpy(reply->reply_data, info, 36);
        return 36;
    }

    case CMD_SET_DATA_FORMAT: {
        // Usually 0x30 / 0x31 / 0x3F. When this arrives, allow input reports.
        uint8_t mode = cmd_len > 0 ? cmd_data[0] : 0x30;
        rt.full_report_enabled = true;
        reply->ack = 0x80;
        reply->reply_data[0] = mode;
        return 1;
    }

    case CMD_TRIGGER_BUTTONS: {
        reply->ack = 0x80;
        return 0;
    }

    case CMD_SET_SHIP_MODE: {
        reply->ack = 0x80;
        return 0;
    }

    case CMD_SPI_FLASH_READ: {
        if (cmd_len < 5) {
            reply->ack = 0x00;
            return 0;
        }

        uint32_t addr =
            ((uint32_t)cmd_data[0]) |
            ((uint32_t)cmd_data[1] << 8) |
            ((uint32_t)cmd_data[2] << 16) |
            ((uint32_t)cmd_data[3] << 24);

        uint8_t size = cmd_data[4];
        if (size > 44) size = 44;

        reply->ack = 0x90;
        reply->reply_data[0] = cmd_data[0];
        reply->reply_data[1] = cmd_data[1];
        reply->reply_data[2] = cmd_data[2];
        reply->reply_data[3] = cmd_data[3];
        reply->reply_data[4] = size;

        uint8_t* flash = g_spi_flash[rt.ctrl];

        if (addr < SPI_FLASH_SIZE) {
            size_t to_copy = std::min((size_t)size, (size_t)(SPI_FLASH_SIZE - addr));
            memcpy(reply->reply_data + 5, flash + addr, to_copy);
            if (to_copy < size) memset(reply->reply_data + 5 + to_copy, 0xFF, size - to_copy);
        } else {
            memset(reply->reply_data + 5, 0xFF, size);
        }

        if (g_verbose) {
            printf("[probe%d] Lendo SPI Flash addr=0x%04X size=%d\n", rt.ctrl + 1, addr, size);
        }
        return 5 + size;
    }

    case CMD_SPI_FLASH_WRITE: {
        reply->ack = 0x80;
        return 0;
    }

    case CMD_SET_NFC_IR_CONFIG: {
        // Working single-controller behavior: 0xA0 0x21 + small MCU payload.
        reply->ack = 0xA0;
        reply->subcmd_id = 0x21;

        memset(reply->reply_data, 0x00, sizeof(reply->reply_data));

        reply->reply_data[0] = 0x01;
        reply->reply_data[1] = 0x00;
        reply->reply_data[2] = 0xFF;
        reply->reply_data[3] = 0x00;
        reply->reply_data[4] = 0x03;
        reply->reply_data[5] = 0x00;
        reply->reply_data[6] = 0x05;
        reply->reply_data[7] = 0x01;

        return 8;
    }

    case CMD_SET_PLAYER_LIGHTS: {
        reply->ack = 0x80;
        reply->reply_data[0] = cmd_len > 0 ? cmd_data[0] : 0;
        return 1;
    }

    case CMD_ENABLE_IMU: {
        reply->ack = 0x80;
        reply->reply_data[0] = cmd_len > 0 ? cmd_data[0] : 0;
        return 1;
    }

    case CMD_SET_IMU_SENS: {
        reply->ack = 0x80;
        return 0;
    }

    case CMD_ENABLE_VIBRATION: {
        reply->ack = 0x80;
        reply->reply_data[0] = cmd_len > 0 ? cmd_data[0] : 0;
        return 1;
    }

    default:
        reply->ack = 0x00;
        return 0;
    }
}

// ── USB Gadget Setup ───────────────────────────────────────────────────────
static bool setup_gadget() {
    auto write_str = [](const char* path, const char* val) -> bool {
        FILE* f = fopen(path, "w");
        if (!f) {
            if (g_verbose) perror(path);
            return false;
        }
        fprintf(f, "%s", val);
        fclose(f);
        return true;
    };

    auto write_bin = [](const char* path, const uint8_t* data, size_t len) -> bool {
        FILE* f = fopen(path, "wb");
        if (!f) {
            if (g_verbose) perror(path);
            return false;
        }
        fwrite(data, 1, len, f);
        fclose(f);
        return true;
    };

    struct stat probe_st;
    if (stat(GADGET_DIR, &probe_st) == 0 && S_ISDIR(probe_st.st_mode)) {
        system("echo '' > /sys/kernel/config/usb_gadget/ns_probe/UDC 2>/dev/null");
        system("rm -rf /sys/kernel/config/usb_gadget/ns_probe/configs/c.1/hid.usb* 2>/dev/null");
        system("rmdir /sys/kernel/config/usb_gadget/ns_probe/configs/c.1/strings/0x409 2>/dev/null");
        system("rmdir /sys/kernel/config/usb_gadget/ns_probe/configs/c.1 2>/dev/null");
        system("rmdir /sys/kernel/config/usb_gadget/ns_probe/functions/hid.usb* 2>/dev/null");
        system("rmdir /sys/kernel/config/usb_gadget/ns_probe/strings/0x409 2>/dev/null");
        system("rmdir /sys/kernel/config/usb_gadget/ns_probe 2>/dev/null");
    }
    std::this_thread::sleep_for(ms(300));

    mkdir(GADGET_DIR, 0755);
    mkdir((std::string(GADGET_DIR) + "/strings/0x409").c_str(), 0755);
    mkdir((std::string(GADGET_DIR) + "/configs/c.1").c_str(), 0755);
    mkdir((std::string(GADGET_DIR) + "/configs/c.1/strings/0x409").c_str(), 0755);

    write_str((std::string(GADGET_DIR) + "/bcdUSB").c_str(),       "0x0200");
    write_str((std::string(GADGET_DIR) + "/bcdDevice").c_str(),    "0x0200");
    write_str((std::string(GADGET_DIR) + "/idVendor").c_str(),     "0x057e");
    write_str((std::string(GADGET_DIR) + "/idProduct").c_str(),    "0x2009");
    write_str((std::string(GADGET_DIR) + "/bDeviceClass").c_str(), "0x00");
    write_str((std::string(GADGET_DIR) + "/bDeviceSubClass").c_str(), "0x00");
    write_str((std::string(GADGET_DIR) + "/bDeviceProtocol").c_str(), "0x00");

    // USB device-level serial is shared by the composite device.
    // Per-controller identity is handled in HID replies and SPI.
    write_str((std::string(GADGET_DIR) + "/strings/0x409/serialnumber").c_str(), "98B6E9112233");
    write_str((std::string(GADGET_DIR) + "/strings/0x409/manufacturer").c_str(), "Nintendo Co., Ltd.");
    write_str((std::string(GADGET_DIR) + "/strings/0x409/product").c_str(),      "Pro Controller");

    write_str((std::string(GADGET_DIR) + "/configs/c.1/MaxPower").c_str(), "500");
    write_str((std::string(GADGET_DIR) + "/configs/c.1/strings/0x409/configuration").c_str(), "Dual Probe Config");

    for (int i = 0; i < CONTROLLER_COUNT; i++) {
        std::string func_name = "hid.usb" + std::to_string(i);
        std::string func_path = std::string(GADGET_DIR) + "/functions/" + func_name;
        mkdir(func_path.c_str(), 0755);

        write_str((func_path + "/protocol").c_str(), "0");
        write_str((func_path + "/subclass").c_str(), "0");
        write_str((func_path + "/report_length").c_str(), "64");
        write_bin((func_path + "/report_desc").c_str(), PRO_CONTROLLER_HID_DESC, HID_DESC_LEN);

        std::string link_path = std::string(GADGET_DIR) + "/configs/c.1/" + func_name;
        symlink(func_path.c_str(), link_path.c_str());
    }

    system("ls /sys/class/udc/ 2>/dev/null | head -1 > /tmp/probe_udc.txt 2>/dev/null");
    FILE* f = fopen("/tmp/probe_udc.txt", "r");
    char udc[128] = {};
    if (f && fgets(udc, sizeof(udc), f)) {
        size_t len = strlen(udc);
        while (len > 0 && (udc[len - 1] == '\n' || udc[len - 1] == ' ')) udc[--len] = '\0';
        write_str((std::string(GADGET_DIR) + "/UDC").c_str(), udc);
        fclose(f);
    }

    std::this_thread::sleep_for(ms(500));
    system("chmod 666 /dev/hidg* 2>/dev/null");
    return true;
}

// ── Main probe loop per controller ─────────────────────────────────────────
static void probe_loop(int fd, int ctrl) {
    ControllerRuntime rt;
    rt.fd = fd;
    rt.ctrl = ctrl;
    rt.timer = 0;
    rt.full_report_enabled = false;
    rt.pending_subcmd_reply = false;
    memset(&rt.pending_reply, 0, sizeof(rt.pending_reply));

    auto tick = us(1'000'000 / WRITER_HZ);
    auto next = Clock::now() + tick;
    struct pollfd pfd = {fd, POLLIN, 0};

    InputReport30 neutral_in = {};
    neutral_in.id = RID_INPUT_STANDARD;
    fill_neutral_controls(neutral_in);

    uint8_t write_buf[REPORT_SIZE];
    uint8_t read_buf[REPORT_SIZE];

    uint64_t standard_reports_this_second = 0;
    auto last_rate_log = Clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next);
        next = Clock::now() + tick;

        if (rt.pending_subcmd_reply) {
            rt.pending_reply.id = RID_INPUT_SUBCMD;
            rt.pending_reply.timer = rt.timer++;
            fill_neutral_controls(rt.pending_reply);

            memcpy(write_buf, &rt.pending_reply, sizeof(InputReport21));
            ssize_t w = write(fd, write_buf, REPORT_SIZE);
            (void)w;

            rt.pending_subcmd_reply = false;
        } else if (rt.full_report_enabled) {
            InputReport30 std_in = neutral_in;
            std_in.timer = rt.timer++;

            // Controller #1 is intentionally passive: it sends neutral reports
            // only, with no buttons and no stick movement. This keeps it connected
            // without generating user input.
            //
            // Controller #2 is the active 250 Hz test controller. It still sends
            // one report every tick, but its visible input is an ultra-short pulse:
            // R is pressed for exactly one 4 ms frame, then released for the next
            // 4 ms frame. This avoids long holds while proving that the report
            // stream can update at 250 Hz.
            if (ctrl == ACTIVE_INPUT_CONTROLLER) {
                if ((rt.timer & 0x01) == 0) {
                    std_in.buttons[0] |= 0x40; // R, 1-frame pulse at 250 Hz
                }
            }

            memcpy(write_buf, &std_in, sizeof(InputReport30));
            ssize_t w = write(fd, write_buf, REPORT_SIZE);
            (void)w;

            standard_reports_this_second++;
            auto now_log = Clock::now();
            if (g_verbose && RATE_LOGS && now_log - last_rate_log >= ms(1000)) {
                printf("[probe%d] standard reports/sec=%llu mode=%s\n",
                       ctrl + 1,
                       (unsigned long long)standard_reports_this_second,
                       ctrl == ACTIVE_INPUT_CONTROLLER ? "active-250hz" : "neutral-only");
                standard_reports_this_second = 0;
                last_rate_log = now_log;
            }
        }

        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            ssize_t r = read(fd, read_buf, REPORT_SIZE);
            if (r > 0) {
                uint8_t id = read_buf[0];

                if (id == RID_OUTPUT_CMD) {
                    uint8_t subcmd_id = read_buf[10];
                    size_t subcmd_data_len = r > 11 ? std::min((size_t)53, (size_t)(r - 11)) : 0;

                    memset(&rt.pending_reply, 0, sizeof(rt.pending_reply));
                    int reply_len = handle_subcommand(
                        rt,
                        subcmd_id,
                        subcmd_data_len > 0 ? read_buf + 11 : nullptr,
                        subcmd_data_len,
                        &rt.pending_reply
                    );

                    // Even zero-length successful ACKs should be sent.
                    rt.pending_subcmd_reply = (reply_len >= 0);

                    if (g_verbose) {
                        printf("[probe%d] Handled 0x01 subcmd 0x%02X reply=0x%02X 0x%02X\n",
                               ctrl + 1,
                               subcmd_id,
                               rt.pending_reply.ack,
                               rt.pending_reply.subcmd_id);
                    }
                } else if (id == 0x80) {
                    uint8_t resp_81[REPORT_SIZE] = {};
                    resp_81[0] = 0x81;
                    resp_81[1] = read_buf[1];
                    resp_81[2] = 0x00;
                    resp_81[3] = 0x03;

                    set_identity_in_0x81(resp_81, ctrl);

                    // 0x80 0x04 means Enable USB HID Joystick Report.
                    // Start sending input reports after this too.
                    if (read_buf[1] == 0x04) {
                        rt.full_report_enabled = true;
                    }

                    // 0x80 0x05 means Disable USB HID Joystick Report.
                    if (read_buf[1] == 0x05) {
                        rt.full_report_enabled = false;
                    }

                    ssize_t w = write(fd, resp_81, REPORT_SIZE);
                    (void)w;

                    if (g_verbose) {
                        printf("[probe%d] Sent 0x81 init ACK for cmd 0x%02X\n", ctrl + 1, read_buf[1]);
                    }
                } else if (id == RID_OUTPUT_RUMBLE) {
                    // Rumble-only packet. Safe to ignore in this probe.
                } else {
                    if (g_verbose) {
                        printf("[probe%d] Unknown output report id 0x%02X len=%zd\n", ctrl + 1, id, r);
                    }
                }
            }
        }
    }
}

static int open_hidg_with_wait(int index) {
    std::string path = "/dev/hidg" + std::to_string(index);

    for (int attempt = 0; attempt < 40; attempt++) {
        int fd = open(path.c_str(), O_RDWR);
        if (fd >= 0) return fd;
        std::this_thread::sleep_for(ms(50));
    }

    perror(path.c_str());
    return -1;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "-v") g_verbose = true;
    }

    for (int i = 0; i < CONTROLLER_COUNT; i++) {
        init_spi_flash(i);
    }

    system("killall ns-backend 2>/dev/null");
    std::this_thread::sleep_for(ms(500));

    setup_gadget();

    int fds[CONTROLLER_COUNT];
    for (int i = 0; i < CONTROLLER_COUNT; i++) {
        fds[i] = open_hidg_with_wait(i);
        if (fds[i] < 0) {
            for (int j = 0; j < i; j++) close(fds[j]);
            system("echo '' > /sys/kernel/config/usb_gadget/ns_probe/UDC 2>/dev/null");
            return 1;
        }
    }

    auto on_sig = [](int) { g_running = false; };
    signal(SIGINT, on_sig);

    printf("NS-PC-Control - Dual Probe Passive/Active 250Hz Running (%d controllers)\n", CONTROLLER_COUNT);
    printf("[probe] hidg0/probe1 = neutral only, hidg1/probe2 = active 250 Hz R pulse\n");

    std::vector<std::thread> threads;
    for (int i = 0; i < CONTROLLER_COUNT; i++) {
        threads.emplace_back([i, &fds]() {
            probe_loop(fds[i], i);
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    for (int i = 0; i < CONTROLLER_COUNT; i++) {
        close(fds[i]);
    }

    system("echo '' > /sys/kernel/config/usb_gadget/ns_probe/UDC 2>/dev/null");
    return 0;
}
