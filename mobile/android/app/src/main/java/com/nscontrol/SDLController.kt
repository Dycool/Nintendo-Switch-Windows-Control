package com.nscontrol

object SDLController {
    init { System.loadLibrary("nsprotocol") }

    fun init(): Boolean = nativeInit()
    fun quit() = nativeQuit()
    fun poll() = nativePoll()

    fun padConnected(slot: Int): Boolean = nativePadConnected(slot)
    fun padButtons(slot: Int): Int = nativePadButtons(slot)
    fun padDpadUp(slot: Int): Boolean = nativePadDpadUp(slot)
    fun padDpadDown(slot: Int): Boolean = nativePadDpadDown(slot)
    fun padDpadLeft(slot: Int): Boolean = nativePadDpadLeft(slot)
    fun padDpadRight(slot: Int): Boolean = nativePadDpadRight(slot)
    fun padLX(slot: Int): Float = nativePadLX(slot)
    fun padLY(slot: Int): Float = nativePadLY(slot)
    fun padRX(slot: Int): Float = nativePadRX(slot)
    fun padRY(slot: Int): Float = nativePadRY(slot)

    fun padHasMotion(slot: Int): Boolean = nativePadHasMotion(slot)
    fun padMotion(slot: Int): ShortArray? = nativePadMotion(slot)

    fun padRumble(slot: Int, low: Int, high: Int, durationMs: Int = 50) =
        nativePadRumble(slot, low, high, durationMs)
    fun padStopRumble(slot: Int) = nativePadStopRumble(slot)

    fun phoneSensorsOpen(): Boolean = nativePhoneSensorsOpen()
    fun phoneSensorsClose() = nativePhoneSensorsClose()
    fun phoneSensorsRead(): ShortArray? = nativePhoneSensorsRead()

    fun phoneHapticOpen(): Boolean = nativePhoneHapticOpen()
    fun phoneHapticClose() = nativePhoneHapticClose()
    fun phoneHapticRumble(low: Int, high: Int) = nativePhoneHapticRumble(low, high)

    // ─── Native declarations ──────────────────────────────

    private external fun nativeInit(): Boolean
    private external fun nativeQuit()
    private external fun nativePoll()

    private external fun nativePadConnected(slot: Int): Boolean
    private external fun nativePadButtons(slot: Int): Int
    private external fun nativePadDpadUp(slot: Int): Boolean
    private external fun nativePadDpadDown(slot: Int): Boolean
    private external fun nativePadDpadLeft(slot: Int): Boolean
    private external fun nativePadDpadRight(slot: Int): Boolean
    private external fun nativePadLX(slot: Int): Float
    private external fun nativePadLY(slot: Int): Float
    private external fun nativePadRX(slot: Int): Float
    private external fun nativePadRY(slot: Int): Float

    private external fun nativePadHasMotion(slot: Int): Boolean
    private external fun nativePadMotion(slot: Int): ShortArray?

    private external fun nativePadRumble(slot: Int, low: Int, high: Int, durationMs: Int)
    private external fun nativePadStopRumble(slot: Int)

    private external fun nativePhoneSensorsOpen(): Boolean
    private external fun nativePhoneSensorsClose()
    private external fun nativePhoneSensorsRead(): ShortArray?

    private external fun nativePhoneHapticOpen(): Boolean
    private external fun nativePhoneHapticClose()
    private external fun nativePhoneHapticRumble(low: Int, high: Int)
}
