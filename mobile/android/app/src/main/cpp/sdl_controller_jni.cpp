#include <jni.h>
#include <stdint.h>

#include "sdl_controller.h"
#include "ns_protocol.h"

static JavaVM* g_jvm = NULL;

// ─── JNI_OnLoad ────────────────────────────────────────────

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

// ─── SDL lifecycle ─────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativeInit(JNIEnv*, jclass) {
#ifdef __ANDROID__
    // SDL3 on Android needs a JavaVM to be set before SDL_Init() for JNI access.
    // Android_InitJNI is defined in SDL3's src/core/android/SDL_android.c and
    // linked from the SDL3 static library.
    extern void Android_InitJNI(JavaVM* vm, JNIEnv* env);
    if (g_jvm) {
        JNIEnv* env = NULL;
        g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
        if (env) Android_InitJNI(g_jvm, env);
    }
#endif
    return sdl_controller_init() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativeQuit(JNIEnv*, jclass) {
    sdl_controller_quit();
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePoll(JNIEnv*, jclass) {
    sdl_controller_poll();
}

// ─── Gamepad state ─────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadConnected(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_connected(slot) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_nscontrol_SDLController_nativePadButtons(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).buttons;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadDpadUp(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).dpad_up ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadDpadDown(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).dpad_down ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadDpadLeft(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).dpad_left ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadDpadRight(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).dpad_right ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_nscontrol_SDLController_nativePadLX(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).lx;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_nscontrol_SDLController_nativePadLY(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).ly;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_nscontrol_SDLController_nativePadRX(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).rx;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_nscontrol_SDLController_nativePadRY(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_input(slot).ry;
}

// ─── Controller motion ─────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePadHasMotion(JNIEnv*, jclass, jint slot) {
    return sdl_controller_pad_motion(slot).has_motion ? JNI_TRUE : JNI_FALSE;
}

// Returns motion as [ax, ay, az, gx, gy, gz]
extern "C" JNIEXPORT jshortArray JNICALL
Java_com_nscontrol_SDLController_nativePadMotion(JNIEnv* env, jclass, jint slot) {
    SdlPadMotion m = sdl_controller_pad_motion(slot);
    jshort arr[6] = { (jshort)m.ax, (jshort)m.ay, (jshort)m.az,
                      (jshort)m.gx, (jshort)m.gy, (jshort)m.gz };
    jshortArray out = env->NewShortArray(6);
    if (out) env->SetShortArrayRegion(out, 0, 6, arr);
    return out;
}

// ─── Controller rumble ─────────────────────────────────────

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePadRumble(JNIEnv*, jclass,
                                                  jint slot, jint low, jint high, jint duration_ms) {
    sdl_controller_pad_rumble(slot, (uint8_t)low, (uint8_t)high, (uint32_t)duration_ms);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePadStopRumble(JNIEnv*, jclass, jint slot) {
    sdl_controller_pad_stop_rumble(slot);
}

// ─── Phone sensors ─────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePhoneSensorsOpen(JNIEnv*, jclass) {
    return sdl_controller_phone_sensors_open() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePhoneSensorsClose(JNIEnv*, jclass) {
    sdl_controller_phone_sensors_close();
}

extern "C" JNIEXPORT jshortArray JNICALL
Java_com_nscontrol_SDLController_nativePhoneSensorsRead(JNIEnv* env, jclass) {
    SdlPadMotion m = sdl_controller_phone_sensors_read();
    if (!m.has_motion) return NULL;
    jshort arr[6] = { (jshort)m.ax, (jshort)m.ay, (jshort)m.az,
                      (jshort)m.gx, (jshort)m.gy, (jshort)m.gz };
    jshortArray out = env->NewShortArray(6);
    if (out) env->SetShortArrayRegion(out, 0, 6, arr);
    return out;
}

// ─── Phone haptics ─────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nscontrol_SDLController_nativePhoneHapticOpen(JNIEnv*, jclass) {
    return sdl_controller_phone_haptic_open() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePhoneHapticClose(JNIEnv*, jclass) {
    sdl_controller_phone_haptic_close();
}

extern "C" JNIEXPORT void JNICALL
Java_com_nscontrol_SDLController_nativePhoneHapticRumble(JNIEnv*, jclass,
                                                          jint low, jint high) {
    sdl_controller_phone_haptic_rumble((uint8_t)low, (uint8_t)high);
}
