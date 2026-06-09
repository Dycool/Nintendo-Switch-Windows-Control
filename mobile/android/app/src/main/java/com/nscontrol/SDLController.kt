package com.nscontrol

object SDLController {
    @Volatile private var libraryLoaded = false
    @Volatile private var initialized = false
    @Volatile private var permanentlyUnavailable = false
    @Volatile private var lastError: String = ""

    fun isAvailable(): Boolean = !permanentlyUnavailable
    fun isReady(): Boolean = initialized
    fun errorText(): String = lastError

    private fun ensureLibraryLoaded(): Boolean {
        if (libraryLoaded) return true
        if (permanentlyUnavailable) return false
        return try {
            System.loadLibrary("nsprotocol")
            libraryLoaded = true
            true
        } catch (t: Throwable) {
            lastError = t.javaClass.simpleName + ": " + (t.message ?: "native load failed")
            permanentlyUnavailable = true
            false
        }
    }

    fun init(): Boolean {
        if (initialized) return true
        if (!ensureLibraryLoaded()) return false
        return try {
            initialized = nativeInit()
            if (!initialized && lastError.isEmpty()) lastError = "SDL nativeInit returned false"
            initialized
        } catch (t: Throwable) {
            lastError = t.javaClass.simpleName + ": " + (t.message ?: "native init failed")
            permanentlyUnavailable = true
            initialized = false
            false
        }
    }

    fun quit() {
        if (!initialized) return
        try { nativeQuit() } catch (_: Throwable) {}
        initialized = false
    }

    fun poll() {
        if (!initialized) return
        try { nativePoll() } catch (_: Throwable) {}
    }

    fun padConnected(slot: Int): Boolean = if (initialized) safeBool { nativePadConnected(slot) } else false
    fun padButtons(slot: Int): Int = if (initialized) safeInt { nativePadButtons(slot) } else 0
    fun padDpadUp(slot: Int): Boolean = if (initialized) safeBool { nativePadDpadUp(slot) } else false
    fun padDpadDown(slot: Int): Boolean = if (initialized) safeBool { nativePadDpadDown(slot) } else false
    fun padDpadLeft(slot: Int): Boolean = if (initialized) safeBool { nativePadDpadLeft(slot) } else false
    fun padDpadRight(slot: Int): Boolean = if (initialized) safeBool { nativePadDpadRight(slot) } else false
    fun padLX(slot: Int): Float = if (initialized) safeFloat { nativePadLX(slot) } else 0.0f
    fun padLY(slot: Int): Float = if (initialized) safeFloat { nativePadLY(slot) } else 0.0f
    fun padRX(slot: Int): Float = if (initialized) safeFloat { nativePadRX(slot) } else 0.0f
    fun padRY(slot: Int): Float = if (initialized) safeFloat { nativePadRY(slot) } else 0.0f

    fun padHasMotion(slot: Int): Boolean = if (initialized) safeBool { nativePadHasMotion(slot) } else false
    fun padMotion(slot: Int): ShortArray? = if (initialized) safeShortArray { nativePadMotion(slot) } else null

    fun padRumble(slot: Int, low: Int, high: Int, durationMs: Int = 50) {
        if (initialized) safeUnit { nativePadRumble(slot, low, high, durationMs) }
    }

    fun padStopRumble(slot: Int) {
        if (initialized) safeUnit { nativePadStopRumble(slot) }
    }

    fun phoneSensorsOpen(): Boolean = if (initialized) safeBool { nativePhoneSensorsOpen() } else false
    fun phoneSensorsClose() { if (initialized) safeUnit { nativePhoneSensorsClose() } }
    fun phoneSensorsRead(): ShortArray? = if (initialized) safeShortArray { nativePhoneSensorsRead() } else null

    fun phoneHapticOpen(): Boolean = if (initialized) safeBool { nativePhoneHapticOpen() } else false
    fun phoneHapticClose() { if (initialized) safeUnit { nativePhoneHapticClose() } }
    fun phoneHapticRumble(low: Int, high: Int) { if (initialized) safeUnit { nativePhoneHapticRumble(low, high) } }

    private inline fun safeUnit(block: () -> Unit) {
        try { block() } catch (t: Throwable) { noteRuntimeError(t) }
    }

    private inline fun safeBool(block: () -> Boolean): Boolean {
        return try { block() } catch (t: Throwable) { noteRuntimeError(t); false }
    }

    private inline fun safeInt(block: () -> Int): Int {
        return try { block() } catch (t: Throwable) { noteRuntimeError(t); 0 }
    }

    private inline fun safeFloat(block: () -> Float): Float {
        return try { block() } catch (t: Throwable) { noteRuntimeError(t); 0.0f }
    }

    private inline fun safeShortArray(block: () -> ShortArray?): ShortArray? {
        return try { block() } catch (t: Throwable) { noteRuntimeError(t); null }
    }

    private fun noteRuntimeError(t: Throwable) {
        lastError = t.javaClass.simpleName + ": " + (t.message ?: "native call failed")
    }

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
