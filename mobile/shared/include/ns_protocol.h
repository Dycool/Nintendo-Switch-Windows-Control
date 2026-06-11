#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NS_PROTO_MAGIC = 0x4E535743u,
    NS_WEB_PROTO_VERSION = 5,
    NS_WEB_PROTO_VERSION_3 = 6,

    NS_FLAG_RESET = 0x01,
    NS_FLAG_DISCONNECT = 0x08,
    NS_FLAG_SINGLE_PAD = 0x04,

    NS_EXT_PAD_PRESENT = 0x01,

    NS_PROTOCOL_HID_SIZE = 8,
    NS_PROTOCOL_MOTION_SIZE = 12,
    NS_PROTOCOL_MOTION_SAMPLE_COUNT = 3,
    NS_PROTOCOL_EXT_PAD_SIZE = 48,
    NS_PROTOCOL_WEB_FRAME_SIZE = 212,
    NS_PROTOCOL_PAD_COUNT = 4,
};

enum {
    NS_BTN_Y       = 1u << 0,
    NS_BTN_B       = 1u << 1,
    NS_BTN_A       = 1u << 2,
    NS_BTN_X       = 1u << 3,
    NS_BTN_L       = 1u << 4,
    NS_BTN_R       = 1u << 5,
    NS_BTN_ZL      = 1u << 6,
    NS_BTN_ZR      = 1u << 7,
    NS_BTN_MINUS   = 1u << 8,
    NS_BTN_PLUS    = 1u << 9,
    NS_BTN_LSTICK  = 1u << 10,
    NS_BTN_RSTICK  = 1u << 11,
    NS_BTN_HOME    = 1u << 12,
    NS_BTN_CAPTURE = 1u << 13,
};

enum {
    NS_HAT_N = 0,
    NS_HAT_NE = 1,
    NS_HAT_E = 2,
    NS_HAT_SE = 3,
    NS_HAT_S = 4,
    NS_HAT_SW = 5,
    NS_HAT_W = 6,
    NS_HAT_NW = 7,
    NS_HAT_NEUTRAL = 8,
};

float ns_standard_gravity(void);
float ns_accel_scale_android(void);
float ns_gyro_scale(void);

uint8_t ns_axis_to_byte(float v);
uint8_t ns_hat_from_dpad(int up, int down, int left, int right);
int16_t ns_clamp_motion(float v);
int16_t ns_gyro_deadzone(int16_t v);
uint16_t ns_normalize_system_shortcuts(uint16_t buttons);

void ns_hid_write_neutral(uint8_t out_hid[NS_PROTOCOL_HID_SIZE]);
void ns_hid_write(uint8_t out_hid[NS_PROTOCOL_HID_SIZE],
                  uint16_t buttons,
                  uint8_t hat,
                  uint8_t lx,
                  uint8_t ly,
                  uint8_t rx,
                  uint8_t ry,
                  int present);
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
                             int present);

void ns_motion_write_values(uint8_t out_motion[NS_PROTOCOL_MOTION_SIZE],
                            int16_t ax,
                            int16_t ay,
                            int16_t az,
                            int16_t gx,
                            int16_t gy,
                            int16_t gz,
                            int has_motion);

// All platforms: accelerometer in m/s², gyroscope in rad/s.
void ns_motion_from_android(uint8_t out_motion[NS_PROTOCOL_MOTION_SIZE],
                            float accel_x,
                            float accel_y,
                            float accel_z,
                            float gyro_x,
                            float gyro_y,
                            float gyro_z);

void ns_pad_write_neutral(uint8_t out_pad[NS_PROTOCOL_EXT_PAD_SIZE]);
void ns_pad_set_hid(uint8_t out_pad[NS_PROTOCOL_EXT_PAD_SIZE],
                    const uint8_t hid[NS_PROTOCOL_HID_SIZE]);
void ns_pad_set_motion(uint8_t out_pad[NS_PROTOCOL_EXT_PAD_SIZE],
                       const uint8_t motion[NS_PROTOCOL_MOTION_SIZE]);
void ns_pad_set_motion_samples(uint8_t out_pad[NS_PROTOCOL_EXT_PAD_SIZE],
                               const uint8_t motion0[NS_PROTOCOL_MOTION_SIZE],
                               const uint8_t motion1[NS_PROTOCOL_MOTION_SIZE],
                               const uint8_t motion2[NS_PROTOCOL_MOTION_SIZE]);

void ns_web_frame_init(uint8_t out_frame[NS_PROTOCOL_WEB_FRAME_SIZE],
                       uint8_t flags,
                       uint32_t seq,
                       uint64_t timestamp_us);
void ns_web_frame_set_pad(uint8_t out_frame[NS_PROTOCOL_WEB_FRAME_SIZE],
                          int pad_index,
                          const uint8_t pad[NS_PROTOCOL_EXT_PAD_SIZE]);
void ns_web_frame_set_hid(uint8_t out_frame[NS_PROTOCOL_WEB_FRAME_SIZE],
                          int pad_index,
                          const uint8_t hid[NS_PROTOCOL_HID_SIZE]);
void ns_web_frame_set_motion(uint8_t out_frame[NS_PROTOCOL_WEB_FRAME_SIZE],
                             int pad_index,
                             const uint8_t motion[NS_PROTOCOL_MOTION_SIZE]);
void ns_web_frame_set_motion_samples(uint8_t out_frame[NS_PROTOCOL_WEB_FRAME_SIZE],
                                     int pad_index,
                                     const uint8_t motion0[NS_PROTOCOL_MOTION_SIZE],
                                     const uint8_t motion1[NS_PROTOCOL_MOTION_SIZE],
                                     const uint8_t motion2[NS_PROTOCOL_MOTION_SIZE]);
int ns_web_frame_extract_hid(const uint8_t* frame,
                             size_t frame_size,
                             int pad_index,
                             uint8_t out_hid[NS_PROTOCOL_HID_SIZE]);

#ifdef __cplusplus
}
#endif
