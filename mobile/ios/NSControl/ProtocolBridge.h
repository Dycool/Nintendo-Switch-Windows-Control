// Bridging header — matches server/include/protocol.hpp wire layout
// Xcode: set SWIFT_OBJC_BRIDGING_HEADER to NSControl/ProtocolBridge.h

#pragma once
#include <stdint.h>

#pragma pack(push, 1)

enum {
    kProtoMagic   = 0x4E535743,
    kWebProtoVer  = 5,
    kSinglePad    = 0x04,
    kExtPresent   = 0x01,
};

enum {
    kBtnY      = 1<<0,  kBtnB      = 1<<1,  kBtnA = 1<<2,  kBtnX = 1<<3,
    kBtnL      = 1<<4,  kBtnR      = 1<<5,  kBtnZL = 1<<6, kBtnZR = 1<<7,
    kBtnMinus  = 1<<8,  kBtnPlus   = 1<<9,
    kBtnLStick = 1<<10, kBtnRStick = 1<<11,
    kBtnHome   = 1<<12, kBtnCapture = 1<<13,
};

enum {
    kHatN = 0, kHatNE = 1, kHatE = 2, kHatSE = 3,
    kHatS = 4, kHatSW = 5, kHatW = 6, kHatNW = 7, kHatNeutral = 8,
};

typedef struct {
    uint16_t buttons;
    uint8_t  hat;
    uint8_t  lx, ly, rx, ry;
    uint8_t  vendor;
} HIDReport;

typedef struct {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
} MotionReport;

typedef struct {
    HIDReport  input;
    MotionReport motion;
    uint8_t    has_motion;
    uint8_t    reserved[3];
} ExtendedHIDReport;

#pragma pack(pop)
