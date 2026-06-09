#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDL_CONTROLLER_MAX_PADS 4

typedef struct {
    uint16_t buttons;
    int dpad_up;
    int dpad_down;
    int dpad_left;
    int dpad_right;
    float lx, ly;
    float rx, ry;
    int connected;
} SdlPadInput;

typedef struct {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
    int has_motion;
} SdlPadMotion;

int sdl_controller_init(void);
void sdl_controller_quit(void);

void sdl_controller_poll(void);

int sdl_controller_pad_connected(int slot);
SdlPadInput sdl_controller_pad_input(int slot);
SdlPadMotion sdl_controller_pad_motion(int slot);
void sdl_controller_pad_rumble(int slot, uint8_t low, uint8_t high, uint32_t duration_ms);
void sdl_controller_pad_stop_rumble(int slot);

int sdl_controller_phone_sensors_open(void);
void sdl_controller_phone_sensors_close(void);
SdlPadMotion sdl_controller_phone_sensors_read(void);

int sdl_controller_phone_haptic_open(void);
void sdl_controller_phone_haptic_close(void);
void sdl_controller_phone_haptic_rumble(uint8_t low, uint8_t high);

#ifdef __cplusplus
}
#endif
