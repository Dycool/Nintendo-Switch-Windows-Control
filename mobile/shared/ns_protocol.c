#include <ns_protocol.h>

#include <math.h>
#include <string.h>

static void ns_write_u16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void ns_write_i16le(uint8_t* p, int16_t v) {
    ns_write_u16le(p, (uint16_t)v);
}

static void ns_write_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void ns_write_u64le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
}

float ns_standard_gravity(void) { return 9.80665f; }
float ns_accel_scale_android(void) { return 4096.0f / 9.80665f; }
float ns_accel_scale_apple(void) { return 4096.0f; }
float ns_gyro_scale(void) { return 57.29577951308232f * 16.384f; }

uint8_t ns_axis_to_byte(float v) {
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;
    int out = (int)lroundf((v + 1.0f) * 127.5f);
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return (uint8_t)out;
}

uint8_t ns_hat_from_dpad(int up, int down, int left, int right) {
    if (up && right) return NS_HAT_NE;
    if (up && left) return NS_HAT_NW;
    if (down && right) return NS_HAT_SE;
    if (down && left) return NS_HAT_SW;
    if (up) return NS_HAT_N;
    if (right) return NS_HAT_E;
    if (down) return NS_HAT_S;
    if (left) return NS_HAT_W;
    return NS_HAT_NEUTRAL;
}

int16_t ns_clamp_motion(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lroundf(v);
}

int16_t ns_gyro_deadzone(int16_t v) {
    return (v >= -32 && v <= 32) ? 0 : v;
}

uint16_t ns_normalize_system_shortcuts(uint16_t buttons) {
    bool capture_combo = (buttons & NS_BTN_MINUS) && (buttons & NS_BTN_PLUS);
    bool home_combo = (buttons & NS_BTN_LSTICK) && (buttons & NS_BTN_RSTICK);
    if (capture_combo) {
        buttons |= NS_BTN_CAPTURE;
        buttons &= ~(uint16_t)(NS_BTN_MINUS | NS_BTN_PLUS | NS_BTN_HOME);
        if (home_combo) {
            buttons &= ~(uint16_t)(NS_BTN_LSTICK | NS_BTN_RSTICK);
        }
    } else if (home_combo) {
        buttons |= NS_BTN_HOME;
        buttons &= ~(uint16_t)(NS_BTN_LSTICK | NS_BTN_RSTICK | NS_BTN_CAPTURE);
    }
    return buttons;
}

void ns_hid_write_neutral(uint8_t out_hid[NS_PROTOCOL_HID_SIZE]) {
    if (!out_hid) return;
    memset(out_hid, 0, NS_PROTOCOL_HID_SIZE);
    out_hid[2] = NS_HAT_NEUTRAL;
    out_hid[3] = 128;
    out_hid[4] = 128;
    out_hid[5] = 128;
    out_hid[6] = 128;
}

void ns_hid_write(uint8_t out_hid[NS_PROTOCOL_HID_SIZE],
                  uint16_t buttons,
                  uint8_t hat,
                  uint8_t lx,
                  uint8_t ly,
                  uint8_t rx,
                  uint8_t ry,
                  int present) {
    if (!out_hid) return;
    ns_write_u16le(out_hid + 0, buttons);
    out_hid[2] = hat;
    out_hid[3] = lx;
    out_hid[4] = ly;
    out_hid[5] = rx;
    out_hid[6] = ry;
    out_hid[7] = present ? NS_EXT_PAD_PRESENT : 0;
}

void ns_hid_write_controller(uint8_t out_hid[NS_PROTOCOL_HID_SIZE],
                             uint16_t buttons,
                             int dpad_up,
                             int dpad_down,
                             int dpad_left,
                             int dpad_right,
                             float lx,
                             float ly,
                             float rx,
                             float ry,
                             int present) {
    ns_hid_write(out_hid,
                 buttons,
                 ns_hat_from_dpad(dpad_up, dpad_down, dpad_left, dpad_right),
                 ns_axis_to_byte(lx),
                 ns_axis_to_byte(ly),
                 ns_axis_to_byte(rx),
                 ns_axis_to_byte(ry),
                 present);
}

void ns_motion_write_values(uint8_t out_motion[NS_PROTOCOL_MOTION_SIZE],
                            int16_t ax,
                            int16_t ay,
                            int16_t az,
                            int16_t gx,
                            int16_t gy,
                            int16_t gz,
                            int has_motion) {
    if (!out_motion) return;
    memset(out_motion, 0, NS_PROTOCOL_MOTION_SIZE);
    ns_write_i16le(out_motion + 0, ax);
    ns_write_i16le(out_motion + 2, ay);
    ns_write_i16le(out_motion + 4, az);
    ns_write_i16le(out_motion + 6, gx);
    ns_write_i16le(out_motion + 8, gy);
    ns_write_i16le(out_motion + 10, gz);
    (void)has_motion;
}

void ns_motion_from_android(uint8_t out_motion[NS_PROTOCOL_MOTION_SIZE],
                            float accel_x,
                            float accel_y,
                            float accel_z,
                            float gyro_x,
                            float gyro_y,
                            float gyro_z) {
    const float accel_scale = ns_accel_scale_android();
    const float gyro_scale = ns_gyro_scale();
    ns_motion_write_values(out_motion,
                           ns_clamp_motion(-accel_y * accel_scale),
                           ns_clamp_motion(-accel_z * accel_scale),
                           ns_clamp_motion(-accel_x * accel_scale),
                           ns_gyro_deadzone(ns_clamp_motion(-gyro_y * gyro_scale)),
                           ns_gyro_deadzone(ns_clamp_motion(-gyro_z * gyro_scale)),
                           ns_gyro_deadzone(ns_clamp_motion(-gyro_x * gyro_scale)),
                           1);
}

static struct {
    int input;  // 0=X, 1=Y, 2=Z
    int sign;   // +1 or -1
} gRemap[3] = {{1, 1}, {2, -1}, {0, 1}};  // defaults: (+y, -z, +x)

void ns_set_motion_remap(int ax_input, int ax_sign,
                         int ay_input, int ay_sign,
                         int az_input, int az_sign) {
    gRemap[0].input = ax_input; gRemap[0].sign = ax_sign == -1 ? -1 : 1;
    gRemap[1].input = ay_input; gRemap[1].sign = ay_sign == -1 ? -1 : 1;
    gRemap[2].input = az_input; gRemap[2].sign = az_sign == -1 ? -1 : 1;
}

static float pick_value(float x, float y, float z, int input) {
    switch (input) {
        case 0: return x;
        case 1: return y;
        case 2: return z;
        default: return 0;
    }
}

void ns_motion_from_apple(uint8_t out_motion[NS_PROTOCOL_MOTION_SIZE],
                          float gravity_x,
                          float gravity_y,
                          float gravity_z,
                          float rotation_x,
                          float rotation_y,
                          float rotation_z) {
    const float accel_scale = ns_accel_scale_apple();
    const float gyro_scale = ns_gyro_scale();
    float g[3] = {gravity_x, gravity_y, gravity_z};
    float r[3] = {rotation_x, rotation_y, rotation_z};
    ns_motion_write_values(out_motion,
        ns_clamp_motion(pick_value(g[0], g[1], g[2], gRemap[0].input) * (float)gRemap[0].sign * accel_scale),
        ns_clamp_motion(pick_value(g[0], g[1], g[2], gRemap[1].input) * (float)gRemap[1].sign * accel_scale),
        ns_clamp_motion(pick_value(g[0], g[1], g[2], gRemap[2].input) * (float)gRemap[2].sign * accel_scale),
        ns_gyro_deadzone(ns_clamp_motion(pick_value(r[0], r[1], r[2], gRemap[0].input) * (float)gRemap[0].sign * gyro_scale)),
        ns_gyro_deadzone(ns_clamp_motion(pick_value(r[0], r[1], r[2], gRemap[1].input) * (float)gRemap[1].sign * gyro_scale)),
        ns_gyro_deadzone(ns_clamp_motion(pick_value(r[0], r[1], r[2], gRemap[2].input) * (float)gRemap[2].sign * gyro_scale)),
        1);
}

void ns_pad_write_neutral(uint8_t out_pad[NS_PROTOCOL_EXT_PAD_SIZE]) {
    if (!out_pad) return;
    memset(out_pad, 0, NS_PROTOCOL_EXT_PAD_SIZE);
    ns_hid_write_neutral(out_pad);
}

void ns_pad_set_hid(uint8_t out_pad[NS_PROTOCOL_EXT_PAD_SIZE],
                    const uint8_t hid[NS_PROTOCOL_HID_SIZE]) {
    if (!out_pad || !hid) return;
    memcpy(out_pad, hid, NS_PROTOCOL_HID_SIZE);
}

void ns_pad_set_motion_samples(uint8_t out_pad[NS_PROTOCOL_EXT_PAD_SIZE],
                               const uint8_t motion0[NS_PROTOCOL_MOTION_SIZE],
                               const uint8_t motion1[NS_PROTOCOL_MOTION_SIZE],
                               const uint8_t motion2[NS_PROTOCOL_MOTION_SIZE]) {
    if (!out_pad || !motion0 || !motion1 || !motion2) return;
    memcpy(out_pad + NS_PROTOCOL_HID_SIZE + 0 * NS_PROTOCOL_MOTION_SIZE, motion0, NS_PROTOCOL_MOTION_SIZE);
    memcpy(out_pad + NS_PROTOCOL_HID_SIZE + 1 * NS_PROTOCOL_MOTION_SIZE, motion1, NS_PROTOCOL_MOTION_SIZE);
    memcpy(out_pad + NS_PROTOCOL_HID_SIZE + 2 * NS_PROTOCOL_MOTION_SIZE, motion2, NS_PROTOCOL_MOTION_SIZE);
    out_pad[44] = 1;
}

void ns_pad_set_motion(uint8_t out_pad[NS_PROTOCOL_EXT_PAD_SIZE],
                       const uint8_t motion[NS_PROTOCOL_MOTION_SIZE]) {
    if (!out_pad || !motion) return;
    ns_pad_set_motion_samples(out_pad, motion, motion, motion);
}

void ns_web_frame_init(uint8_t out_frame[NS_PROTOCOL_WEB_FRAME_SIZE],
                       uint8_t flags,
                       uint32_t seq,
                       uint64_t timestamp_us) {
    if (!out_frame) return;
    memset(out_frame, 0, NS_PROTOCOL_WEB_FRAME_SIZE);
    ns_write_u32le(out_frame + 0, NS_PROTO_MAGIC);
    out_frame[4] = NS_WEB_PROTO_VERSION_3;
    out_frame[5] = flags;
    out_frame[6] = 0;
    out_frame[7] = 0;
    ns_write_u32le(out_frame + 8, seq);
    ns_write_u64le(out_frame + 12, timestamp_us);
    for (int i = 0; i < NS_PROTOCOL_PAD_COUNT; ++i) {
        ns_pad_write_neutral(out_frame + 20 + (i * NS_PROTOCOL_EXT_PAD_SIZE));
    }
}

void ns_web_frame_set_pad(uint8_t out_frame[NS_PROTOCOL_WEB_FRAME_SIZE],
                          int pad_index,
                          const uint8_t pad[NS_PROTOCOL_EXT_PAD_SIZE]) {
    if (!out_frame || !pad || pad_index < 0 || pad_index >= NS_PROTOCOL_PAD_COUNT) return;
    memcpy(out_frame + 20 + (pad_index * NS_PROTOCOL_EXT_PAD_SIZE), pad, NS_PROTOCOL_EXT_PAD_SIZE);
}

void ns_web_frame_set_hid(uint8_t out_frame[NS_PROTOCOL_WEB_FRAME_SIZE],
                          int pad_index,
                          const uint8_t hid[NS_PROTOCOL_HID_SIZE]) {
    if (!out_frame || !hid || pad_index < 0 || pad_index >= NS_PROTOCOL_PAD_COUNT) return;
    memcpy(out_frame + 20 + (pad_index * NS_PROTOCOL_EXT_PAD_SIZE), hid, NS_PROTOCOL_HID_SIZE);
}

void ns_web_frame_set_motion_samples(uint8_t out_frame[NS_PROTOCOL_WEB_FRAME_SIZE],
                                     int pad_index,
                                     const uint8_t motion0[NS_PROTOCOL_MOTION_SIZE],
                                     const uint8_t motion1[NS_PROTOCOL_MOTION_SIZE],
                                     const uint8_t motion2[NS_PROTOCOL_MOTION_SIZE]) {
    if (!out_frame || !motion0 || !motion1 || !motion2 || pad_index < 0 || pad_index >= NS_PROTOCOL_PAD_COUNT) return;
    uint8_t* pad = out_frame + 20 + (pad_index * NS_PROTOCOL_EXT_PAD_SIZE);
    ns_pad_set_motion_samples(pad, motion0, motion1, motion2);
}

void ns_web_frame_set_motion(uint8_t out_frame[NS_PROTOCOL_WEB_FRAME_SIZE],
                             int pad_index,
                             const uint8_t motion[NS_PROTOCOL_MOTION_SIZE]) {
    if (!out_frame || !motion || pad_index < 0 || pad_index >= NS_PROTOCOL_PAD_COUNT) return;
    ns_web_frame_set_motion_samples(out_frame, pad_index, motion, motion, motion);
}

int ns_web_frame_extract_hid(const uint8_t* frame,
                             size_t frame_size,
                             int pad_index,
                             uint8_t out_hid[NS_PROTOCOL_HID_SIZE]) {
    if (!frame || !out_hid || pad_index < 0 || pad_index >= NS_PROTOCOL_PAD_COUNT) return 0;
    const size_t off = 20u + ((size_t)pad_index * (size_t)NS_PROTOCOL_EXT_PAD_SIZE);
    if (frame_size < off + NS_PROTOCOL_HID_SIZE) return 0;
    memcpy(out_hid, frame + off, NS_PROTOCOL_HID_SIZE);
    return 1;
}
