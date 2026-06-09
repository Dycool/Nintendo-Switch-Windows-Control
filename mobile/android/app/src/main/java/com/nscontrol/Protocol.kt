package com.nscontrol

import kotlin.math.roundToInt

object Protocol {
    // WebSocket/mobile protocol v6 matches ns::ExtendedMultiReport3:
    // 20-byte frame header + 4 pads * 48 bytes. Each pad carries HID + 3 real
    // MotionReport samples (oldest -> newest). This avoids backend-faked IMU samples.
    const val FRAME_SIZE = 212
    const val HID_SIZE = 8
    const val MOTION_SAMPLE_SIZE = 12
    const val MOTION_SAMPLE_COUNT = 3
    const val EXT_PAD_SIZE = 48
    const val PAD_COUNT = 4

    private const val MAGIC = 0x4E535743
    private const val VERSION = 6

    const val RUMBLE_PACKET_SIZE = 8
    const val PRECISION_RUMBLE_PACKET_SIZE = 20
    const val RUMBLE_MAGIC = 0x4E535652 // 'NSVR'
    const val PRECISION_RUMBLE_MAGIC = 0x4E535648 // 'NSVH'

    const val FLAG_RESET = 0x01
    const val FLAG_DISCONNECT = 0x08
    const val FLAG_SINGLE_PAD = 0x04

    private const val PAD_PRESENT = 0x01

    const val BTN_Y       = 1 shl 0
    const val BTN_B       = 1 shl 1
    const val BTN_A       = 1 shl 2
    const val BTN_X       = 1 shl 3
    const val BTN_L       = 1 shl 4
    const val BTN_R       = 1 shl 5
    const val BTN_ZL      = 1 shl 6
    const val BTN_ZR      = 1 shl 7
    const val BTN_MINUS   = 1 shl 8
    const val BTN_PLUS    = 1 shl 9
    const val BTN_LSTICK  = 1 shl 10
    const val BTN_RSTICK  = 1 shl 11
    const val BTN_HOME    = 1 shl 12
    const val BTN_CAPTURE = 1 shl 13

    const val HAT_N = 0
    const val HAT_NE = 1
    const val HAT_E = 2
    const val HAT_SE = 3
    const val HAT_S = 4
    const val HAT_SW = 5
    const val HAT_W = 6
    const val HAT_NW = 7
    const val HAT_NEUTRAL = 8

    const val STANDARD_GRAVITY: Float = 9.80665f

    fun neutralHid(): ByteArray = ByteArray(HID_SIZE).also { writeNeutralHid(it) }
    fun neutralMotion(): ByteArray = ByteArray(MOTION_SAMPLE_SIZE)

    fun controllerHid(
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
    ): ByteArray {
        val hat = hatFromDpad(dpadUp, dpadDown, dpadLeft, dpadRight)
        return hid(buttons, hat, axisToByte(lx), axisToByte(ly), axisToByte(rx), axisToByte(ry), present)
    }

    fun motionFromAndroid(
        accelX: Float,
        accelY: Float,
        accelZ: Float,
        gyroX: Float,
        gyroY: Float,
        gyroZ: Float
    ): ByteArray {
        val accelScale = 4096.0f / STANDARD_GRAVITY
        val gyroScale = 57.29577951308232f * 16.384f
        return motionFromValues(
            clampMotionShort(-accelX * accelScale),
            clampMotionShort(-accelZ * accelScale),
            clampMotionShort( accelY * accelScale),
            gyroDeadzoneShort(clampMotionShort(-gyroX * gyroScale)),
            gyroDeadzoneShort(clampMotionShort(-gyroZ * gyroScale)),
            gyroDeadzoneShort(clampMotionShort( gyroY * gyroScale)),
            hasMotion = true
        )
    }

    fun motionFromValues(
        ax: Short, ay: Short, az: Short,
        gx: Short, gy: Short, gz: Short,
        hasMotion: Boolean
    ): ByteArray = if (!hasMotion) neutralMotion() else ByteArray(MOTION_SAMPLE_SIZE).also { out ->
        writeI16LE(out, 0, ax)
        writeI16LE(out, 2, ay)
        writeI16LE(out, 4, az)
        writeI16LE(out, 6, gx)
        writeI16LE(out, 8, gy)
        writeI16LE(out, 10, gz)
    }

    fun duplicateMotionSamples(sample: ByteArray): Array<ByteArray>? {
        if (sample.size < MOTION_SAMPLE_SIZE) return null
        val s = sample.copyOfRange(0, MOTION_SAMPLE_SIZE)
        return arrayOf(s.copyOf(), s.copyOf(), s.copyOf())
    }

    fun buildFrame(
        seq: Int,
        flags: Int,
        timestampUs: Long,
        pad0Hid: ByteArray?,
        pad0Motion: ByteArray?
    ): ByteArray = initFrame(flags, seq, timestampUs).also { frame ->
        pad0Hid?.let { setFrameHid(frame, 0, it) }
        pad0Motion?.let { setFrameMotion(frame, 0, it) }
    }

    fun initFrame(flags: Int, seq: Int, timestampUs: Long): ByteArray = ByteArray(FRAME_SIZE).also { frame ->
        writeU32LE(frame, 0, MAGIC)
        frame[4] = VERSION.toByte()
        frame[5] = (flags and 0xFF).toByte()
        writeU32LE(frame, 8, seq)
        writeU64LE(frame, 12, timestampUs)
        for (i in 0 until PAD_COUNT) writeNeutralPad(frame, 20 + i * EXT_PAD_SIZE)
    }

    fun setFrameHid(frame: ByteArray, padIndex: Int, hid: ByteArray) {
        if (frame.size < FRAME_SIZE || hid.size < HID_SIZE || padIndex !in 0 until PAD_COUNT) return
        hid.copyInto(frame, 20 + padIndex * EXT_PAD_SIZE, 0, HID_SIZE)
    }

    fun setFrameMotion(frame: ByteArray, padIndex: Int, motion: ByteArray) {
        duplicateMotionSamples(motion)?.let { setFrameMotionSamples(frame, padIndex, it) }
    }

    fun setFrameMotionSamples(frame: ByteArray, padIndex: Int, samples: Array<ByteArray>) {
        if (frame.size < FRAME_SIZE || samples.size < MOTION_SAMPLE_COUNT || padIndex !in 0 until PAD_COUNT) return
        val base = 20 + padIndex * EXT_PAD_SIZE + HID_SIZE
        for (i in 0 until MOTION_SAMPLE_COUNT) {
            val sample = samples[i]
            if (sample.size < MOTION_SAMPLE_SIZE) return
            sample.copyInto(frame, base + i * MOTION_SAMPLE_SIZE, 0, MOTION_SAMPLE_SIZE)
        }
        frame[20 + padIndex * EXT_PAD_SIZE + 44] = 1
    }

    fun extractPad0HidFromWebFrame(src: ByteArray): ByteArray? {
        if (src.size < 20 + HID_SIZE) return null
        return src.copyOfRange(20, 20 + HID_SIZE)
    }

    fun hid(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int, present: Boolean): ByteArray =
        ByteArray(HID_SIZE).also { out ->
            writeU16LE(out, 0, buttons and 0xFFFF)
            out[2] = hat.coerceIn(0, 8).toByte()
            out[3] = lx.coerceIn(0, 255).toByte()
            out[4] = ly.coerceIn(0, 255).toByte()
            out[5] = rx.coerceIn(0, 255).toByte()
            out[6] = ry.coerceIn(0, 255).toByte()
            out[7] = if (present) PAD_PRESENT.toByte() else 0
        }

    private fun writeNeutralHid(out: ByteArray, off: Int = 0) {
        for (i in 0 until HID_SIZE) out[off + i] = 0
        out[off + 2] = HAT_NEUTRAL.toByte()
        out[off + 3] = 128.toByte()
        out[off + 4] = 128.toByte()
        out[off + 5] = 128.toByte()
        out[off + 6] = 128.toByte()
    }

    private fun writeNeutralPad(out: ByteArray, off: Int) {
        for (i in 0 until EXT_PAD_SIZE) out[off + i] = 0
        writeNeutralHid(out, off)
    }

    private fun hatFromDpad(up: Boolean, down: Boolean, left: Boolean, right: Boolean): Int = when {
        up && right -> HAT_NE
        up && left -> HAT_NW
        down && right -> HAT_SE
        down && left -> HAT_SW
        up -> HAT_N
        right -> HAT_E
        down -> HAT_S
        left -> HAT_W
        else -> HAT_NEUTRAL
    }

    private fun axisToByte(vIn: Float): Int {
        val v = vIn.coerceIn(-1.0f, 1.0f)
        return ((v + 1.0f) * 127.5f).roundToInt().coerceIn(0, 255)
    }

    private fun clampMotionShort(v: Float): Short =
        v.roundToInt().coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt()).toShort()

    private fun gyroDeadzoneShort(v: Short): Short = if (kotlin.math.abs(v.toInt()) <= 8) 0 else v

    private fun writeU16LE(out: ByteArray, off: Int, value: Int) {
        out[off] = (value and 0xFF).toByte()
        out[off + 1] = ((value ushr 8) and 0xFF).toByte()
    }

    private fun writeI16LE(out: ByteArray, off: Int, value: Short) = writeU16LE(out, off, value.toInt())

    private fun writeU32LE(out: ByteArray, off: Int, value: Int) {
        out[off] = (value and 0xFF).toByte()
        out[off + 1] = ((value ushr 8) and 0xFF).toByte()
        out[off + 2] = ((value ushr 16) and 0xFF).toByte()
        out[off + 3] = ((value ushr 24) and 0xFF).toByte()
    }

    private fun writeU64LE(out: ByteArray, off: Int, value: Long) {
        for (i in 0 until 8) out[off + i] = ((value ushr (8 * i)) and 0xFF).toByte()
    }
}
