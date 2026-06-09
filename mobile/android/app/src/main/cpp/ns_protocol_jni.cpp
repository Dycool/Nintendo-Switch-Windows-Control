#include <jni.h>
#include <stdint.h>
#include <string.h>

#include "ns_protocol.h"

static jbyteArray byte_array(JNIEnv* env, const uint8_t* data, size_t size) {
    jbyteArray out = env->NewByteArray((jsize)size);
    if (!out) return nullptr;
    env->SetByteArrayRegion(out, 0, (jsize)size, reinterpret_cast<const jbyte*>(data));
    return out;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnY(JNIEnv*, jclass) { return NS_BTN_Y; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnB(JNIEnv*, jclass) { return NS_BTN_B; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnA(JNIEnv*, jclass) { return NS_BTN_A; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnX(JNIEnv*, jclass) { return NS_BTN_X; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnL(JNIEnv*, jclass) { return NS_BTN_L; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnR(JNIEnv*, jclass) { return NS_BTN_R; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnZL(JNIEnv*, jclass) { return NS_BTN_ZL; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnZR(JNIEnv*, jclass) { return NS_BTN_ZR; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnMinus(JNIEnv*, jclass) { return NS_BTN_MINUS; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnPlus(JNIEnv*, jclass) { return NS_BTN_PLUS; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnLStick(JNIEnv*, jclass) { return NS_BTN_LSTICK; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnRStick(JNIEnv*, jclass) { return NS_BTN_RSTICK; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnHome(JNIEnv*, jclass) { return NS_BTN_HOME; }
extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_Protocol_nativeBtnCapture(JNIEnv*, jclass) { return NS_BTN_CAPTURE; }
extern "C" JNIEXPORT jfloat JNICALL
Java_com_nscontrol_Protocol_nativeStandardGravity(JNIEnv*, jclass) { return ns_standard_gravity(); }

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_nscontrol_Protocol_neutralHid(JNIEnv* env, jclass) {
    uint8_t hid[NS_PROTOCOL_HID_SIZE];
    ns_hid_write_neutral(hid);
    return byte_array(env, hid, sizeof(hid));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_nscontrol_Protocol_controllerHid(JNIEnv* env, jclass,
                                          jint buttons,
                                          jboolean dpad_up,
                                          jboolean dpad_down,
                                          jboolean dpad_left,
                                          jboolean dpad_right,
                                          jfloat lx,
                                          jfloat ly,
                                          jfloat rx,
                                          jfloat ry,
                                          jboolean present) {
    uint8_t hid[NS_PROTOCOL_HID_SIZE];
    ns_hid_write_controller(hid,
                            (uint16_t)buttons,
                            dpad_up == JNI_TRUE,
                            dpad_down == JNI_TRUE,
                            dpad_left == JNI_TRUE,
                            dpad_right == JNI_TRUE,
                            lx,
                            ly,
                            rx,
                            ry,
                            present == JNI_TRUE);
    return byte_array(env, hid, sizeof(hid));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_nscontrol_Protocol_motionFromAndroid(JNIEnv* env, jclass,
                                              jfloat accel_x,
                                              jfloat accel_y,
                                              jfloat accel_z,
                                              jfloat gyro_x,
                                              jfloat gyro_y,
                                              jfloat gyro_z) {
    uint8_t motion[NS_PROTOCOL_MOTION_SIZE];
    ns_motion_from_android(motion, accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z);
    return byte_array(env, motion, sizeof(motion));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_nscontrol_Protocol_buildFrame(JNIEnv* env, jclass,
                                       jint seq,
                                       jint flags,
                                       jlong timestamp_us,
                                       jbyteArray pad0_hid,
                                       jbyteArray pad0_motion) {
    uint8_t frame[NS_PROTOCOL_WEB_FRAME_SIZE];
    ns_web_frame_init(frame, (uint8_t)flags, (uint32_t)seq, (uint64_t)timestamp_us);

    if (pad0_hid && env->GetArrayLength(pad0_hid) >= NS_PROTOCOL_HID_SIZE) {
        jbyte tmp[NS_PROTOCOL_HID_SIZE];
        env->GetByteArrayRegion(pad0_hid, 0, NS_PROTOCOL_HID_SIZE, tmp);
        ns_web_frame_set_hid(frame, 0, reinterpret_cast<const uint8_t*>(tmp));
    }

    if (pad0_motion && env->GetArrayLength(pad0_motion) >= NS_PROTOCOL_MOTION_SIZE) {
        jbyte tmp[NS_PROTOCOL_MOTION_SIZE];
        env->GetByteArrayRegion(pad0_motion, 0, NS_PROTOCOL_MOTION_SIZE, tmp);
        ns_web_frame_set_motion(frame, 0, reinterpret_cast<const uint8_t*>(tmp));
    }

    return byte_array(env, frame, sizeof(frame));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_nscontrol_Protocol_extractPad0HidFromWebFrame(JNIEnv* env, jclass, jbyteArray src) {
    if (!src) return nullptr;
    const jsize n = env->GetArrayLength(src);
    if (n <= 0) return nullptr;
    jbyte* bytes = env->GetByteArrayElements(src, nullptr);
    if (!bytes) return nullptr;

    uint8_t hid[NS_PROTOCOL_HID_SIZE];
    int ok = ns_web_frame_extract_hid(reinterpret_cast<const uint8_t*>(bytes), (size_t)n, 0, hid);
    env->ReleaseByteArrayElements(src, bytes, JNI_ABORT);
    if (!ok) return nullptr;
    return byte_array(env, hid, sizeof(hid));
}
