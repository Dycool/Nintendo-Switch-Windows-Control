package com.nscontrol

object Protocol {
    init { System.loadLibrary("nsprotocol") }

    const val FRAME_SIZE = 116
    const val HID_SIZE = 8
    const val MOTION_SIZE = 16
    const val PAD_COUNT = 4
    const val FLAG_RESET = 0x01
    const val FLAG_SINGLE_PAD = 0x04

    val BTN_Y: Int = nativeBtnY()
    val BTN_B: Int = nativeBtnB()
    val BTN_A: Int = nativeBtnA()
    val BTN_X: Int = nativeBtnX()
    val BTN_L: Int = nativeBtnL()
    val BTN_R: Int = nativeBtnR()
    val BTN_ZL: Int = nativeBtnZL()
    val BTN_ZR: Int = nativeBtnZR()
    val BTN_MINUS: Int = nativeBtnMinus()
    val BTN_PLUS: Int = nativeBtnPlus()
    val BTN_LSTICK: Int = nativeBtnLStick()
    val BTN_RSTICK: Int = nativeBtnRStick()
    val BTN_HOME: Int = nativeBtnHome()
    val BTN_CAPTURE: Int = nativeBtnCapture()
    val STANDARD_GRAVITY: Float = nativeStandardGravity()

    private external fun nativeBtnY(): Int
    private external fun nativeBtnB(): Int
    private external fun nativeBtnA(): Int
    private external fun nativeBtnX(): Int
    private external fun nativeBtnL(): Int
    private external fun nativeBtnR(): Int
    private external fun nativeBtnZL(): Int
    private external fun nativeBtnZR(): Int
    private external fun nativeBtnMinus(): Int
    private external fun nativeBtnPlus(): Int
    private external fun nativeBtnLStick(): Int
    private external fun nativeBtnRStick(): Int
    private external fun nativeBtnHome(): Int
    private external fun nativeBtnCapture(): Int
    private external fun nativeStandardGravity(): Float

    external fun neutralHid(): ByteArray
    external fun neutralMotion(): ByteArray

    external fun controllerHid(
        buttons: Int,
        dpadUp: Boolean,
        dpadDown: Boolean,
        dpadLeft: Boolean,
        dpadRight: Boolean,
        lx: Float,
        ly: Float,
        rx: Float,
        ry: Float,
        present: Boolean = true
    ): ByteArray

    external fun motionFromAndroid(
        accelX: Float,
        accelY: Float,
        accelZ: Float,
        gyroX: Float,
        gyroY: Float,
        gyroZ: Float
    ): ByteArray

    external fun motionFromValues(
        ax: Short, ay: Short, az: Short,
        gx: Short, gy: Short, gz: Short,
        hasMotion: Boolean
    ): ByteArray

    external fun buildFrame(
        seq: Int,
        flags: Int,
        timestampUs: Long,
        pad0Hid: ByteArray?,
        pad0Motion: ByteArray?
    ): ByteArray

    external fun initFrame(flags: Int, seq: Int, timestampUs: Long): ByteArray
    external fun setFrameHid(frame: ByteArray, padIndex: Int, hid: ByteArray)
    external fun setFrameMotion(frame: ByteArray, padIndex: Int, motion: ByteArray)

    external fun extractPad0HidFromWebFrame(src: ByteArray): ByteArray?
}
