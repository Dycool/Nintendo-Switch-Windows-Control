package com.nscontrol

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import android.view.Surface
import android.view.View
import android.view.WindowManager
import android.webkit.JavascriptInterface
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import okio.ByteString.Companion.toByteString
import org.json.JSONArray
import java.net.URI
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.roundToInt

class MainActivity : AppCompatActivity() {
    companion object { private const val TAG = "NSMobile" }
    private lateinit var connectView: View
    private lateinit var webView: WebView
    private lateinit var hostInput: EditText
    private lateinit var statusText: TextView

    private var host = ""
    private var connected = false
    @Volatile private var controlClientActive = false
    @Volatile private var sending = false
    private var ws: WebSocket? = null
    private val client = OkHttpClient.Builder().build()
    private val seq = AtomicInteger(0)
    private val senderToken = AtomicInteger(0)
    private val sendLock = Any()

    private lateinit var sensorManager: SensorManager
    private var accelSensor: Sensor? = null
    private var gravitySensor: Sensor? = null
    private var gyroSensor: Sensor? = null
    private var vibrator: Vibrator? = null
    @Volatile private var phoneSensorsActive = false
    private val phoneSensorLock = Any()
    private val latestPhoneAccel = FloatArray(3)
    private val latestPhoneGravity = FloatArray(3)
    private val latestPhoneGyro = FloatArray(3)
    private var hasLatestPhoneAccel = false
    private var hasLatestPhoneGravity = false
    private var hasLatestPhoneGyro = false
    private val latestMotionSamples = Array(Protocol.MOTION_SAMPLE_COUNT) { ByteArray(Protocol.MOTION_SAMPLE_SIZE) }
    private var latestMotionSampleCount = 0

    @Volatile private var touchHid: ByteArray? = null
    @Volatile private var touchFrame: ByteArray? = null
    @Volatile private var lastTouchFrameMs: Long = 0
    @Volatile private var lastBridgeFrameParseMs: Long = 0

    private enum class Page { MAIN_MENU, TOUCH_CONTROLS, EDITOR }
    private var currentPage = Page.MAIN_MENU
    private val pageStack = mutableListOf<Page>()

    private var rumbleLow = 0
    private var rumbleHigh = 0
    private var rumbleUntilMs = 0L
    private var rumbleLastSetMs = 0L

    private val phoneSensorListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            synchronized(phoneSensorLock) {
                when (event.sensor.type) {
                    Sensor.TYPE_GRAVITY -> {
                        for (i in 0 until minOf(3, event.values.size)) latestPhoneGravity[i] = event.values[i]
                        hasLatestPhoneGravity = true
                    }
                    Sensor.TYPE_ACCELEROMETER -> {
                        for (i in 0 until minOf(3, event.values.size)) latestPhoneAccel[i] = event.values[i]
                        hasLatestPhoneAccel = true
                    }
                    Sensor.TYPE_GYROSCOPE -> {
                        for (i in 0 until minOf(3, event.values.size)) latestPhoneGyro[i] = event.values[i]
                        hasLatestPhoneGyro = true
                        pushMotionSampleLocked()
                    }
                }
            }
        }
        override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        connectView = layoutInflater.inflate(R.layout.connect, null)
        webView = WebView(this)
        hostInput = connectView.findViewById(R.id.hostInput)
        statusText = connectView.findViewById(R.id.statusText)
        connectView.findViewById<Button>(R.id.connectBtn).setOnClickListener { onConnect() }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                when {
                    connected && currentPage != Page.MAIN_MENU -> goBack()
                    connected -> disconnect()
                    else -> {
                        isEnabled = false
                        onBackPressedDispatcher.onBackPressed()
                    }
                }
            }
        })

        setContentView(connectView)
        getPreferences(MODE_PRIVATE).getString("host", "")?.let {
            hostInput.setText(it)
            hostInput.setSelection(it.length)
        }

        sensorManager = getSystemService(Context.SENSOR_SERVICE) as SensorManager
        gravitySensor = sensorManager.getDefaultSensor(Sensor.TYPE_GRAVITY)
        accelSensor = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        gyroSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        vibrator = if (Build.VERSION.SDK_INT >= 31) {
            (getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as VibratorManager).defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
        }
    }

    private fun onConnect() {
        host = hostInput.text.toString().trim()
        if (host.isEmpty()) return
        getPreferences(MODE_PRIVATE).edit().putString("host", host).apply()
        setupWebView()
        connected = true
        currentPage = Page.MAIN_MENU
        pageStack.clear()
        setContentView(webView)
        loadUrl(pageUrl(Page.MAIN_MENU))
        statusText.text = "Loaded"
    }

    // WebSocket to the Raspberry Pi backend. Only Touch Controls owns a session in this native-mobile v1.
    private fun connectWs(): Boolean {
        return try {
            val wsUrl = normalizeWsUrl(host)
            val req = Request.Builder().url(wsUrl).build()
            ws = client.newWebSocket(req, object : WebSocketListener() {
                override fun onOpen(w: WebSocket, r: Response) {
                    runOnUiThread {
                        if (!controlClientActive || ws !== w) return@runOnUiThread
                        statusText.text = "Connected"
                        startSending()
                    }
                }

                override fun onClosed(w: WebSocket, code: Int, reason: String) {
                    runOnUiThread { handleWsClosed(w, "Disconnected") }
                }

                override fun onFailure(w: WebSocket, t: Throwable, r: Response?) {
                    runOnUiThread { handleWsClosed(w, "Connection failed") }
                }

                override fun onMessage(w: WebSocket, bytes: ByteString) {
                    // Server -> mobile rumble is the classic 8-byte ns::RumblePacket:
                    // magic 'NSVR', subpad, low_freq, high_freq, duration_10ms.
                    // Do not wait for/use PrecisionRumblePacket here; the current ns-client
                    // path consumes classic NSVR packets too, and the server sends classic rumble.
                    if (bytes.size != Protocol.RUMBLE_PACKET_SIZE) return
                    val magic = readU32LE(bytes, 0)
                    if (magic != Protocol.RUMBLE_MAGIC) return
                    val subpad = bytes[4].toInt() and 0xFF
                    val low = bytes[5].toInt() and 0xFF
                    val high = bytes[6].toInt() and 0xFF
                    val duration10Ms = bytes[7].toInt() and 0xFF
                    Log.d(TAG, "rumble packet subpad=$subpad low=$low high=$high duration10ms=$duration10Ms")
                    runOnUiThread { routeRumble(subpad, low, high, duration10Ms) }
                }
            })
            true
        } catch (_: Throwable) {
            runOnUiThread { statusText.text = "Invalid server address" }
            false
        }
    }

    private fun handleWsClosed(socket: WebSocket, text: String) {
        if (ws !== socket) return
        statusText.text = text
        sending = false
        controlClientActive = false
        touchHid = null
        touchFrame = null
        lastTouchFrameMs = 0
        ws = null
        stopPhoneSensors()
        phoneRumble(0, 0)
    }

    private fun normalizeWsUrl(raw: String): String {
        var text = raw.trim()
        if (text.isEmpty()) throw IllegalArgumentException("Empty host")

        val hadScheme = text.contains("://")
        if (!hadScheme) text = "ws://$text"

        val uri = URI(text)
        val inputScheme = (uri.scheme ?: "ws").lowercase()
        val wsScheme = when (inputScheme) {
            "https", "wss" -> "wss"
            "http", "ws" -> "ws"
            else -> "ws"
        }

        val authority = uri.rawAuthority ?: throw IllegalArgumentException("Missing host")
        val hostPart = uri.host ?: authority
            .substringAfter('@')
            .let { a -> if (a.startsWith("[")) a.substringBefore(']') + "]" else a.substringBeforeLast(':', a) }
            .trim()
        if (hostPart.isEmpty()) throw IllegalArgumentException("Missing host")

        val safeHost = if (hostPart.contains(':') && !hostPart.startsWith("[")) "[$hostPart]" else hostPart
        val explicitPort = uri.port
        val needsBackendDefaultPort = explicitPort < 0 && (!hadScheme || wsScheme == "ws")
        val portText = when {
            explicitPort >= 0 -> ":$explicitPort"
            needsBackendDefaultPort -> ":8080"
            else -> ""
        }
        val path = uri.rawPath?.takeIf { it.isNotEmpty() } ?: "/"
        val query = uri.rawQuery?.let { "?$it" } ?: ""
        return "$wsScheme://$safeHost$portText$path$query"
    }

    private fun readU32LE(bytes: ByteString, off: Int): Int {
        return (bytes[off].toInt() and 0xFF) or
            ((bytes[off + 1].toInt() and 0xFF) shl 8) or
            ((bytes[off + 2].toInt() and 0xFF) shl 16) or
            ((bytes[off + 3].toInt() and 0xFF) shl 24)
    }

    private fun startSending() {
        if (sending) return
        val token = senderToken.incrementAndGet()
        sending = true
        Thread {
            try {
                startPhoneSensors()
                while (sending && controlClientActive && senderToken.get() == token) {
                    sendFrame()
                    Thread.sleep(4)
                }
            } catch (_: Throwable) {
                runOnUiThread {
                    if (senderToken.get() != token) return@runOnUiThread
                    statusText.text = "Input sender failed"
                    deactivateControlClient()
                }
            } finally {
                if (senderToken.get() == token) {
                    sending = false
                    stopPhoneSensors()
                }
            }
        }.start()
    }

    private fun sendFrame() {
        sendFrameInternal()
    }

    private fun sendResetFrameTo(socket: WebSocket) {
        try { sendFrameInternal(socketOverride = socket, flagsOverride = Protocol.FLAG_RESET, forceNeutral = true) } catch (_: Throwable) {}
    }

    private fun sendFrameInternal(socketOverride: WebSocket? = null, flagsOverride: Int? = null, forceNeutral: Boolean = false) {
        synchronized(sendLock) {
            val socket = socketOverride ?: ws ?: return
            val touchActive = !forceNeutral && touchClientActive()
            val flags = flagsOverride ?: if (touchActive) Protocol.FLAG_SINGLE_PAD else 0
            val timestampUs = System.currentTimeMillis() * 1000L
            val frame = Protocol.initFrame(flags, seq.getAndIncrement(), timestampUs)

            if (touchActive) {
                val now = SystemClock.uptimeMillis()
                val hid = if (now - lastTouchFrameMs <= 500L) {
                    touchHid ?: touchFrame?.let { Protocol.extractPad0HidFromWebFrame(it) } ?: Protocol.neutralHid()
                } else {
                    Protocol.neutralHid()
                }
                Protocol.setFrameHid(frame, 0, hid)
                phoneMotionSamples()?.let { Protocol.setFrameMotionSamples(frame, 0, it) }
            }

            if (!socket.send(frame.toByteString())) throw IllegalStateException("WebSocket send queue rejected frame")
        }
    }

    private fun pushMotionSampleLocked() {
        if (!hasLatestPhoneGyro || (!hasLatestPhoneGravity && !hasLatestPhoneAccel)) return

        val accel = if (hasLatestPhoneGravity) latestPhoneGravity else latestPhoneAccel
        val a = remapSensorForDisplay(accel)
        val g = remapSensorForDisplay(latestPhoneGyro)
        val accelScale = 4096.0f / Protocol.STANDARD_GRAVITY
        val gyroScale = 57.29577951308232f * 16.384f
        val sample = Protocol.motionFromValues(
            clampMotionShort(-a[0] * accelScale),
            clampMotionShort(-a[2] * accelScale),
            clampMotionShort( a[1] * accelScale),
            gyroDeadzoneShort(clampMotionShort(-g[0] * gyroScale)),
            gyroDeadzoneShort(clampMotionShort(-g[2] * gyroScale)),
            gyroDeadzoneShort(clampMotionShort( g[1] * gyroScale)),
            hasMotion = true
        )

        latestMotionSamples[0] = latestMotionSamples[1]
        latestMotionSamples[1] = latestMotionSamples[2]
        latestMotionSamples[2] = sample
        if (latestMotionSampleCount < Protocol.MOTION_SAMPLE_COUNT) latestMotionSampleCount++
    }

    private fun phoneMotionSamples(): Array<ByteArray>? {
        synchronized(phoneSensorLock) {
            if (latestMotionSampleCount < Protocol.MOTION_SAMPLE_COUNT) return null
            return Array(Protocol.MOTION_SAMPLE_COUNT) { i -> latestMotionSamples[i].copyOf() }
        }
    }

    private fun clampMotionShort(v: Float): Short = v.roundToInt().coerceIn(-32768, 32767).toShort()
    private fun gyroDeadzoneShort(v: Short): Short = if (kotlin.math.abs(v.toInt()) <= 8) 0 else v

    private fun remapSensorForDisplay(v: FloatArray): FloatArray {
        val rotation = try {
            if (Build.VERSION.SDK_INT >= 30) display?.rotation ?: Surface.ROTATION_0 else legacyDisplayRotation()
        } catch (_: Throwable) { Surface.ROTATION_0 }
        return when (rotation) {
            Surface.ROTATION_90  -> floatArrayOf(-v[1],  v[0], v[2])
            Surface.ROTATION_180 -> floatArrayOf(-v[0], -v[1], v[2])
            Surface.ROTATION_270 -> floatArrayOf( v[1], -v[0], v[2])
            else -> floatArrayOf(v[0], v[1], v[2])
        }
    }

    @Suppress("DEPRECATION")
    private fun legacyDisplayRotation(): Int = windowManager.defaultDisplay.rotation

    private fun startPhoneSensors() {
        if (phoneSensorsActive) return
        synchronized(phoneSensorLock) {
            hasLatestPhoneAccel = false
            hasLatestPhoneGravity = false
            hasLatestPhoneGyro = false
            latestPhoneAccel.fill(0.0f)
            latestPhoneGravity.fill(0.0f)
            latestPhoneGyro.fill(0.0f)
            latestMotionSampleCount = 0
            for (i in 0 until Protocol.MOTION_SAMPLE_COUNT) latestMotionSamples[i].fill(0)
        }
        var opened = false
        val gravityOpened = gravitySensor?.let {
            sensorManager.registerListener(phoneSensorListener, it, SensorManager.SENSOR_DELAY_GAME)
        } ?: false
        opened = gravityOpened
        if (!gravityOpened) {
            accelSensor?.let { opened = sensorManager.registerListener(phoneSensorListener, it, SensorManager.SENSOR_DELAY_GAME) || opened }
        }
        gyroSensor?.let { opened = sensorManager.registerListener(phoneSensorListener, it, SensorManager.SENSOR_DELAY_GAME) || opened }
        phoneSensorsActive = opened
    }

    private fun stopPhoneSensors() {
        if (!phoneSensorsActive) return
        try { sensorManager.unregisterListener(phoneSensorListener) } catch (_: Throwable) {}
        phoneSensorsActive = false
        synchronized(phoneSensorLock) {
            hasLatestPhoneAccel = false
            hasLatestPhoneGravity = false
            hasLatestPhoneGyro = false
            latestMotionSampleCount = 0
        }
    }

    private fun routeRumble(subpad: Int, low: Int, high: Int, duration10Ms: Int) {
        // Native mobile v1 owns one logical pad. Accept any subpad so server-side
        // remaps cannot make phone haptics disappear.
        if (!controlClientActive) return
        phoneRumble(low, high, duration10Ms)
    }

    private fun phoneRumble(low: Int, high: Int, duration10Ms: Int = 0) {
        try {
            val v = vibrator ?: return
            val neutral = (low == 0 && high == 0) || duration10Ms == 0
            val now = SystemClock.uptimeMillis()
            if (neutral) {
                rumbleLow = 0
                rumbleHigh = 0
                rumbleUntilMs = 0L
                rumbleLastSetMs = now
                v.cancel()
                Log.d(TAG, "rumble stop")
                return
            }

            // Match the desktop ns-client behavior: classic NSVR duration is in
            // 10ms units, but tiny Pro rumble pulses are too short to feel on a
            // phone if we vibrate for only 30-40ms. Keep a 250ms minimum haptic
            // window while still refreshing it when packets keep arriving.
            val durationMs = maxOf(250L, duration10Ms.coerceIn(1, 255) * 10L)
            val strength = maxOf(low, high).coerceIn(1, 255)
            if (rumbleLow == low && rumbleHigh == high && now - rumbleLastSetMs < 100L) {
                rumbleUntilMs = now + durationMs
                return
            }

            rumbleLow = low
            rumbleHigh = high
            rumbleUntilMs = now + durationMs
            rumbleLastSetMs = now

            if (Build.VERSION.SDK_INT >= 26) {
                val amp = if (v.hasAmplitudeControl()) strength.coerceAtLeast(64) else VibrationEffect.DEFAULT_AMPLITUDE
                v.vibrate(VibrationEffect.createOneShot(durationMs, amp))
            } else {
                @Suppress("DEPRECATION")
                v.vibrate(durationMs)
            }
            Log.d(TAG, "rumble start low=$low high=$high durationMs=$durationMs")
        } catch (t: Throwable) {
            Log.w(TAG, "phone rumble failed", t)
        }
    }

    private fun touchClientActive(): Boolean = controlClientActive && currentPage == Page.TOUCH_CONTROLS

    private fun setupWebView() {
        val baseUserAgent = webView.settings.userAgentString ?: ""
        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            allowFileAccess = true
            allowContentAccess = true
            mixedContentMode = WebSettings.MIXED_CONTENT_ALWAYS_ALLOW
            userAgentString = "$baseUserAgent NS-Mobile/1.0"
        }
        webView.addJavascriptInterface(JSBridge(), "NSBridge")
        webView.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(v: WebView, req: WebResourceRequest): Boolean {
                val url = req.url.toString()
                return when {
                    url.endsWith("/mobile") || url.endsWith("/mobile.html") -> { navTo(Page.TOUCH_CONTROLS); true }
                    url.endsWith("/editor") || url.endsWith("/editor.html") -> { navTo(Page.EDITOR); true }
                    else -> false
                }
            }

            override fun onPageFinished(v: WebView, url: String) {
                if (currentPage == Page.MAIN_MENU) {
                    v.evaluateJavascript("""
                        (function(){
                          var connect = document.getElementById('btnConnect');
                          if (connect) connect.style.display = 'none';
                          var kb = document.getElementById('kbModeContainer');
                          if (kb) kb.style.display = 'none';
                          var bindings = document.getElementById('btnBindings');
                          if (bindings) bindings.style.display = 'none';
                          var macros = document.getElementById('btnMacros');
                          if (macros) macros.style.display = 'none';
                          var touch = document.getElementById('btnTouchControls');
                          if (touch) touch.style.display = 'inline-block';
                          var editor = document.getElementById('btnEditor');
                          if (editor) editor.style.display = 'inline-block';
                        })();
                    """.trimIndent(), null)
                }
            }
        }
    }

    private fun loadUrl(url: String) { webView.loadUrl(url) }

    private fun pageUrl(page: Page): String = when (page) {
        Page.MAIN_MENU -> "file:///android_asset/ns_mobile/index.html"
        Page.TOUCH_CONTROLS -> "file:///android_asset/ns_mobile/mobile.html"
        Page.EDITOR -> "file:///android_asset/ns_mobile/editor.html"
    }

    private fun navTo(page: Page) {
        pageStack.add(currentPage)
        enterPage(page)
    }

    private fun enterPage(page: Page) {
        currentPage = page
        if (page == Page.TOUCH_CONTROLS || page == Page.EDITOR) deactivateControlClient()
        loadUrl(pageUrl(page))
    }

    private fun goBack() {
        if (pageStack.isNotEmpty()) enterPage(pageStack.removeAt(pageStack.lastIndex))
    }

    inner class JSBridge {
        @JavascriptInterface
        fun onOpen() {
            runOnUiThread {
                if (currentPage == Page.TOUCH_CONTROLS) activateControlClient()
            }
        }

        @JavascriptInterface
        fun onBinary(json: String) {
            if (currentPage != Page.TOUCH_CONTROLS || !controlClientActive) return
            try {
                val now = SystemClock.uptimeMillis()
                if (now - lastBridgeFrameParseMs < 8L) return
                lastBridgeFrameParseMs = now
                val arr = JSONArray(json)
                if (arr.length() < 20 + Protocol.HID_SIZE) return
                val frame = ByteArray(arr.length()) { i -> arr.getInt(i).toByte() }
                touchFrame = frame
                touchHid = Protocol.extractPad0HidFromWebFrame(frame)
                lastTouchFrameMs = now
            } catch (_: Throwable) {}
        }

        @JavascriptInterface
        fun onTouchState(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int) {
            if (currentPage != Page.TOUCH_CONTROLS || !controlClientActive) return
            touchHid = Protocol.hid(
                normalizeSystemShortcuts(buttons),
                hat.coerceIn(0, 8),
                lx.coerceIn(0, 255),
                ly.coerceIn(0, 255),
                rx.coerceIn(0, 255),
                ry.coerceIn(0, 255),
                present = true
            )
            lastTouchFrameMs = SystemClock.uptimeMillis()
        }

        @JavascriptInterface
        fun onClose() { runOnUiThread { deactivateControlClient() } }

        @JavascriptInterface
        fun onBack() { runOnUiThread { goBack() } }
    }

    private fun normalizeSystemShortcuts(buttonsIn: Int): Int {
        var buttons = buttonsIn
        val captureCombo = (buttons and Protocol.BTN_MINUS) != 0 && (buttons and Protocol.BTN_PLUS) != 0
        val homeCombo = (buttons and Protocol.BTN_LSTICK) != 0 && (buttons and Protocol.BTN_RSTICK) != 0
        if (captureCombo) {
            buttons = buttons or Protocol.BTN_CAPTURE
            buttons = buttons and Protocol.BTN_MINUS.inv() and Protocol.BTN_PLUS.inv() and Protocol.BTN_HOME.inv()
            if (homeCombo) buttons = buttons and Protocol.BTN_LSTICK.inv() and Protocol.BTN_RSTICK.inv()
        } else if (homeCombo) {
            buttons = buttons or Protocol.BTN_HOME
            buttons = buttons and Protocol.BTN_LSTICK.inv() and Protocol.BTN_RSTICK.inv() and Protocol.BTN_CAPTURE.inv()
        }
        return buttons
    }

    override fun onDestroy() {
        disconnect()
        super.onDestroy()
    }

    private fun activateControlClient() {
        if (controlClientActive) return
        touchHid = null
        touchFrame = null
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        controlClientActive = true
        if (!connectWs()) controlClientActive = false
    }

    private fun deactivateControlClient() {
        if (!controlClientActive && ws == null && !sending) return
        val closingWs = ws
        senderToken.incrementAndGet()
        sending = false
        controlClientActive = false
        touchHid = null
        touchFrame = null
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        ws = null
        stopPhoneSensors()
        phoneRumble(0, 0)

        if (closingWs != null) {
            Thread {
                try {
                    repeat(3) {
                        sendResetFrameTo(closingWs)
                        try { Thread.sleep(4) } catch (_: InterruptedException) { return@Thread }
                    }
                    try { closingWs.close(1000, "Leaving touch controls") } catch (_: Throwable) {}
                } catch (_: Throwable) {}
            }.start()
        }
    }

    private fun disconnect() {
        deactivateControlClient()
        connected = false
        try { webView.loadUrl("about:blank") } catch (_: Throwable) {}
        setContentView(connectView)
    }
}
