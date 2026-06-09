package com.nscontrol

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.input.InputManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import android.view.HapticFeedbackConstants
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
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
import kotlin.math.abs
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
    private enum class ClientMode { NONE, TOUCH, HUB }

    private class PhysicalPad {
        var deviceId: Int = -1
        var name: String = "Empty"
        var present: Boolean = false
        var buttons: Int = 0
        var dpadUp: Boolean = false
        var dpadDown: Boolean = false
        var dpadLeft: Boolean = false
        var dpadRight: Boolean = false
        var lx: Int = 128
        var ly: Int = 128
        var rx: Int = 128
        var ry: Int = 128
        var hasMotion: Boolean = false
        var hasRumble: Boolean = false
        var hasGyro: Boolean = false
        val motionSamples: Array<ByteArray> = Array(Protocol.MOTION_SAMPLE_COUNT) { Protocol.neutralMotion() }
        var motionSampleCount: Int = 0

        fun reset() {
            deviceId = -1
            name = "Empty"
            present = false
            buttons = 0
            dpadUp = false
            dpadDown = false
            dpadLeft = false
            dpadRight = false
            lx = 128; ly = 128; rx = 128; ry = 128
            hasMotion = false
            hasRumble = false
            hasGyro = false
            motionSampleCount = 0
            for (i in 0 until Protocol.MOTION_SAMPLE_COUNT) motionSamples[i].fill(0)
        }

        fun hid(): ByteArray {
            val hat = when {
                dpadUp && dpadRight -> Protocol.HAT_NE
                dpadUp && dpadLeft -> Protocol.HAT_NW
                dpadDown && dpadRight -> Protocol.HAT_SE
                dpadDown && dpadLeft -> Protocol.HAT_SW
                dpadUp -> Protocol.HAT_N
                dpadRight -> Protocol.HAT_E
                dpadDown -> Protocol.HAT_S
                dpadLeft -> Protocol.HAT_W
                else -> Protocol.HAT_NEUTRAL
            }
            return Protocol.hid(buttons, hat, lx, ly, rx, ry, present)
        }
    }

    private var currentPage = Page.MAIN_MENU
    private val pageStack = mutableListOf<Page>()
    @Volatile private var activeClientMode = ClientMode.NONE

    private lateinit var inputManager: InputManager
    private val physicalLock = Any()
    private val physicalPads = Array(Protocol.PAD_COUNT) { PhysicalPad() }
    private val physicalGravity = Array(Protocol.PAD_COUNT) { FloatArray(3) }
    private val physicalAccel = Array(Protocol.PAD_COUNT) { FloatArray(3) }
    private val physicalGyro = Array(Protocol.PAD_COUNT) { FloatArray(3) }
    private val physicalHasGravity = BooleanArray(Protocol.PAD_COUNT)
    private val physicalHasAccel = BooleanArray(Protocol.PAD_COUNT)
    private val physicalHasGyro = BooleanArray(Protocol.PAD_COUNT)
    private val physicalSensorManagers = arrayOfNulls<SensorManager>(Protocol.PAD_COUNT)
    private val physicalSensorListeners = arrayOfNulls<SensorEventListener>(Protocol.PAD_COUNT)

    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) { if (activeClientMode == ClientMode.HUB) runOnUiThread { scanPhysicalControllers() } }
        override fun onInputDeviceRemoved(deviceId: Int) { if (activeClientMode == ClientMode.HUB) runOnUiThread { scanPhysicalControllers() } }
        override fun onInputDeviceChanged(deviceId: Int) { if (activeClientMode == ClientMode.HUB) runOnUiThread { scanPhysicalControllers() } }
    }

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

        inputManager = getSystemService(Context.INPUT_SERVICE) as InputManager
        inputManager.registerInputDeviceListener(inputDeviceListener, null)

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

    // WebSocket to the Raspberry Pi backend. Either Controller Hub or Touch Controls owns the only live session.
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
                if (activeClientMode == ClientMode.TOUCH) startPhoneSensors()
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
                    if (activeClientMode != ClientMode.HUB) stopPhysicalControllerSensors()
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

            when {
                activeClientMode == ClientMode.TOUCH && touchActive -> {
                    val now = SystemClock.uptimeMillis()
                    val hid = if (now - lastTouchFrameMs <= 500L) {
                        touchHid ?: touchFrame?.let { Protocol.extractPad0HidFromWebFrame(it) } ?: Protocol.neutralHid()
                    } else {
                        Protocol.neutralHid()
                    }
                    Protocol.setFrameHid(frame, 0, hid)
                    phoneMotionSamples()?.let { Protocol.setFrameMotionSamples(frame, 0, it) }
                }
                activeClientMode == ClientMode.HUB && !forceNeutral -> {
                    synchronized(physicalLock) {
                        for (i in 0 until Protocol.PAD_COUNT) {
                            val pad = physicalPads[i]
                            if (!pad.present) continue
                            Protocol.setFrameHid(frame, i, pad.hid())
                            if (pad.hasMotion && pad.motionSampleCount >= Protocol.MOTION_SAMPLE_COUNT) {
                                Protocol.setFrameMotionSamples(frame, i, Array(Protocol.MOTION_SAMPLE_COUNT) { j -> pad.motionSamples[j].copyOf() })
                            }
                        }
                    }
                }
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
        if (!controlClientActive) return
        when (activeClientMode) {
            ClientMode.HUB -> physicalRumble(subpad, low, high, duration10Ms)
            ClientMode.TOUCH -> {
                // Touch mode owns one virtual pad. The current backend should report
                // subpad 0 for this session, but accept any subpad while testing so
                // a temporary server-side mapping cannot make haptics vanish.
                phoneRumble(low, high, duration10Ms)
            }
            ClientMode.NONE -> Unit
        }
    }

    private fun phoneRumble(low: Int, high: Int, duration10Ms: Int = 0) {
        try {
            val v = vibrator
            val neutral = (low == 0 && high == 0) || duration10Ms == 0
            val now = SystemClock.uptimeMillis()
            if (neutral) {
                rumbleLow = 0
                rumbleHigh = 0
                rumbleUntilMs = 0L
                rumbleLastSetMs = now
                v?.cancel()
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

            val didVibrate = if (v != null && v.hasVibrator()) {
                if (Build.VERSION.SDK_INT >= 26) {
                    val amp = if (v.hasAmplitudeControl()) strength.coerceAtLeast(80) else VibrationEffect.DEFAULT_AMPLITUDE
                    v.vibrate(VibrationEffect.createOneShot(durationMs, amp))
                } else {
                    @Suppress("DEPRECATION")
                    v.vibrate(durationMs)
                }
                true
            } else false
            // Some devices expose weak/disabled vibrator access to WebView apps.
            // Fire a platform haptic too; it is harmless if the real vibrator worked.
            try { webView.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS) } catch (_: Throwable) {}
            Log.d(TAG, "rumble start low=$low high=$high durationMs=$durationMs didVibrate=$didVibrate")
        } catch (t: Throwable) {
            Log.w(TAG, "phone rumble failed", t)
        }
    }

    private fun touchClientActive(): Boolean = controlClientActive && activeClientMode == ClientMode.TOUCH && currentPage == Page.TOUCH_CONTROLS

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
                          var group = document.querySelector('.btn-group');
                          if (group && !document.getElementById('btnHubStart')) {
                            var start = document.createElement('button');
                            start.id = 'btnHubStart'; start.textContent = 'Controller Hub';
                            start.style.cssText = 'background:#e8f5e9;border-color:#a5d6a7;';
                            start.onclick = function(){ if (window.NSBridge && NSBridge.onHubStart) NSBridge.onHubStart(); };
                            var stop = document.createElement('button');
                            stop.id = 'btnHubStop'; stop.textContent = 'Stop Hub';
                            stop.onclick = function(){ if (window.NSBridge && NSBridge.onHubStop) NSBridge.onHubStop(); };
                            var refresh = document.createElement('button');
                            refresh.id = 'btnHubRefresh'; refresh.textContent = 'Refresh Pads';
                            refresh.onclick = function(){ if (window.NSBridge && NSBridge.onHubRefresh) NSBridge.onHubRefresh(); };
                            group.insertBefore(start, group.firstChild);
                            group.insertBefore(stop, start.nextSibling);
                            group.insertBefore(refresh, stop.nextSibling);
                          }
                          if (window.NSBridge && NSBridge.onHubRefresh) NSBridge.onHubRefresh();
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
        fun onHubStart() { runOnUiThread { activateControllerHub() } }

        @JavascriptInterface
        fun onHubStop() { runOnUiThread { deactivateControlClient(); updateHubStatusOnPage("Hub stopped") } }

        @JavascriptInterface
        fun onHubRefresh() { runOnUiThread { scanPhysicalControllers(); updateHubStatusOnPage() } }

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

    private fun activateControllerHub() {
        if (controlClientActive && activeClientMode == ClientMode.HUB) {
            scanPhysicalControllers()
            updateHubStatusOnPage("Hub already running")
            return
        }
        deactivateControlClient()
        currentPage = Page.MAIN_MENU
        activeClientMode = ClientMode.HUB
        controlClientActive = true
        scanPhysicalControllers()
        updateHubStatusOnPage("Connecting hub...")
        if (!connectWs()) {
            controlClientActive = false
            activeClientMode = ClientMode.NONE
            updateHubStatusOnPage("Hub connection failed")
        }
    }

    private fun scanPhysicalControllers() {
        if (!::inputManager.isInitialized) return
        synchronized(physicalLock) {
            val oldByDevice = physicalPads.filter { it.present }.associateBy { it.deviceId }
            stopPhysicalControllerSensorsLocked(clearPads = false)
            for (pad in physicalPads) pad.reset()

            val devices = InputDevice.getDeviceIds()
                .mapNotNull { InputDevice.getDevice(it) }
                .filter { isControllerDevice(it) }
                .take(Protocol.PAD_COUNT)

            for ((slot, device) in devices.withIndex()) {
                val prev = oldByDevice[device.id]
                val pad = physicalPads[slot]
                pad.deviceId = device.id
                pad.name = device.name ?: "Controller ${slot + 1}"
                pad.present = true
                pad.hasRumble = deviceHasVibrator(device)
                if (prev != null) {
                    pad.buttons = prev.buttons; pad.dpadUp = prev.dpadUp; pad.dpadDown = prev.dpadDown
                    pad.dpadLeft = prev.dpadLeft; pad.dpadRight = prev.dpadRight
                    pad.lx = prev.lx; pad.ly = prev.ly; pad.rx = prev.rx; pad.ry = prev.ry
                }
                startPhysicalControllerSensorsLocked(slot, device)
            }
        }
        updateHubStatusOnPage()
    }

    private fun isControllerDevice(device: InputDevice): Boolean {
        val sources = device.sources
        val gamepad = (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
        val joystick = (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        return gamepad || joystick
    }

    private fun slotForDeviceIdLocked(deviceId: Int): Int = physicalPads.indexOfFirst { it.present && it.deviceId == deviceId }

    private fun hatFromDpad(up: Boolean, down: Boolean, left: Boolean, right: Boolean): Int = when {
        up && right -> Protocol.HAT_NE
        up && left -> Protocol.HAT_NW
        down && right -> Protocol.HAT_SE
        down && left -> Protocol.HAT_SW
        up -> Protocol.HAT_N
        right -> Protocol.HAT_E
        down -> Protocol.HAT_S
        left -> Protocol.HAT_W
        else -> Protocol.HAT_NEUTRAL
    }

    private fun axisToByte(value: Float): Int {
        val v = value.coerceIn(-1.0f, 1.0f)
        return ((v + 1.0f) * 127.5f).roundToInt().coerceIn(0, 255)
    }

    private fun centeredAxis(event: MotionEvent, device: InputDevice?, axis: Int): Float {
        val value = event.getAxisValue(axis)
        val flat = device?.getMotionRange(axis, event.source)?.flat ?: 0.05f
        return if (abs(value) <= flat) 0.0f else value.coerceIn(-1.0f, 1.0f)
    }

    private fun buttonBitForKeyCode(code: Int): Int = when (code) {
        KeyEvent.KEYCODE_BUTTON_A -> Protocol.BTN_B
        KeyEvent.KEYCODE_BUTTON_B -> Protocol.BTN_A
        KeyEvent.KEYCODE_BUTTON_X -> Protocol.BTN_Y
        KeyEvent.KEYCODE_BUTTON_Y -> Protocol.BTN_X
        KeyEvent.KEYCODE_BUTTON_L1 -> Protocol.BTN_L
        KeyEvent.KEYCODE_BUTTON_R1 -> Protocol.BTN_R
        KeyEvent.KEYCODE_BUTTON_L2 -> Protocol.BTN_ZL
        KeyEvent.KEYCODE_BUTTON_R2 -> Protocol.BTN_ZR
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BACK -> Protocol.BTN_MINUS
        KeyEvent.KEYCODE_BUTTON_START -> Protocol.BTN_PLUS
        KeyEvent.KEYCODE_BUTTON_THUMBL -> Protocol.BTN_LSTICK
        KeyEvent.KEYCODE_BUTTON_THUMBR -> Protocol.BTN_RSTICK
        KeyEvent.KEYCODE_BUTTON_MODE -> Protocol.BTN_HOME
        else -> 0
    }

    private fun handleControllerKey(event: KeyEvent): Boolean {
        if (activeClientMode != ClientMode.HUB || !controlClientActive) return false
        val actionDown = event.action == KeyEvent.ACTION_DOWN
        if (!actionDown && event.action != KeyEvent.ACTION_UP) return false
        var handled = false
        synchronized(physicalLock) {
            val slot = slotForDeviceIdLocked(event.deviceId)
            if (slot < 0) return false
            val pad = physicalPads[slot]
            when (event.keyCode) {
                KeyEvent.KEYCODE_DPAD_UP -> { pad.dpadUp = actionDown; handled = true }
                KeyEvent.KEYCODE_DPAD_DOWN -> { pad.dpadDown = actionDown; handled = true }
                KeyEvent.KEYCODE_DPAD_LEFT -> { pad.dpadLeft = actionDown; handled = true }
                KeyEvent.KEYCODE_DPAD_RIGHT -> { pad.dpadRight = actionDown; handled = true }
                else -> {
                    val bit = buttonBitForKeyCode(event.keyCode)
                    if (bit != 0) {
                        pad.buttons = if (actionDown) pad.buttons or bit else pad.buttons and bit.inv()
                        handled = true
                    }
                }
            }
        }
        return handled
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (handleControllerKey(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (activeClientMode == ClientMode.HUB && controlClientActive &&
            (event.source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK &&
            event.action == MotionEvent.ACTION_MOVE) {
            synchronized(physicalLock) {
                val slot = slotForDeviceIdLocked(event.deviceId)
                if (slot >= 0) {
                    val device = event.device
                    val pad = physicalPads[slot]
                    pad.lx = axisToByte(centeredAxis(event, device, MotionEvent.AXIS_X))
                    pad.ly = axisToByte(centeredAxis(event, device, MotionEvent.AXIS_Y))
                    pad.rx = axisToByte(centeredAxis(event, device, MotionEvent.AXIS_Z))
                    pad.ry = axisToByte(centeredAxis(event, device, MotionEvent.AXIS_RZ))
                    val l2 = maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE))
                    val r2 = maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS))
                    if (l2 > 0.5f) pad.buttons = pad.buttons or Protocol.BTN_ZL else pad.buttons = pad.buttons and Protocol.BTN_ZL.inv()
                    if (r2 > 0.5f) pad.buttons = pad.buttons or Protocol.BTN_ZR else pad.buttons = pad.buttons and Protocol.BTN_ZR.inv()
                    val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
                    val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
                    pad.dpadLeft = hx < -0.5f; pad.dpadRight = hx > 0.5f
                    pad.dpadUp = hy < -0.5f; pad.dpadDown = hy > 0.5f
                    return true
                }
            }
        }
        return super.onGenericMotionEvent(event)
    }

    private fun deviceHasVibrator(device: InputDevice): Boolean = try {
        if (Build.VERSION.SDK_INT >= 31) device.vibratorManager.vibratorIds.isNotEmpty() else {
            @Suppress("DEPRECATION")
            device.vibrator.hasVibrator()
        }
    } catch (_: Throwable) { false }

    private fun physicalRumble(subpad: Int, low: Int, high: Int, duration10Ms: Int) {
        if (subpad !in 0 until Protocol.PAD_COUNT) return
        val deviceId = synchronized(physicalLock) { physicalPads[subpad].deviceId }
        if (deviceId < 0) return
        val device = InputDevice.getDevice(deviceId) ?: return
        val neutral = (low == 0 && high == 0) || duration10Ms == 0
        val durationMs = if (neutral) 0L else maxOf(250L, duration10Ms.coerceIn(1, 255) * 10L)
        val strength = maxOf(low, high).coerceIn(1, 255)
        try {
            val vib: Vibrator? = if (Build.VERSION.SDK_INT >= 31) {
                device.vibratorManager.defaultVibrator
            } else {
                @Suppress("DEPRECATION")
                device.vibrator
            }
            if (neutral) {
                vib?.cancel()
                Log.d(TAG, "controller rumble stop slot=${subpad + 1}")
                return
            }
            if (vib != null && vib.hasVibrator()) {
                if (Build.VERSION.SDK_INT >= 26) {
                    val amp = if (vib.hasAmplitudeControl()) strength.coerceAtLeast(64) else VibrationEffect.DEFAULT_AMPLITUDE
                    vib.vibrate(VibrationEffect.createOneShot(durationMs, amp))
                } else {
                    @Suppress("DEPRECATION")
                    vib.vibrate(durationMs)
                }
                Log.d(TAG, "controller rumble slot=${subpad + 1} low=$low high=$high durationMs=$durationMs")
            } else {
                Log.d(TAG, "controller has no vibrator slot=${subpad + 1} ${device.name}")
            }
        } catch (t: Throwable) {
            Log.w(TAG, "controller rumble failed slot=${subpad + 1}", t)
        }
    }

    private fun stopAllPhysicalRumble() {
        for (i in 0 until Protocol.PAD_COUNT) physicalRumble(i, 0, 0, 0)
    }

    private fun startPhysicalControllerSensorsLocked(slot: Int, device: InputDevice) {
        if (Build.VERSION.SDK_INT < 31) return
        try {
            val sm = device.sensorManager ?: return
            val gravity = sm.getDefaultSensor(Sensor.TYPE_GRAVITY)
            val accel = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
            val gyro = sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE) ?: return
            val listener = object : SensorEventListener {
                override fun onSensorChanged(event: SensorEvent) {
                    synchronized(physicalLock) {
                        if (slot !in 0 until Protocol.PAD_COUNT || physicalPads[slot].deviceId != device.id) return
                        when (event.sensor.type) {
                            Sensor.TYPE_GRAVITY -> { for (i in 0 until minOf(3, event.values.size)) physicalGravity[slot][i] = event.values[i]; physicalHasGravity[slot] = true }
                            Sensor.TYPE_ACCELEROMETER -> { for (i in 0 until minOf(3, event.values.size)) physicalAccel[slot][i] = event.values[i]; physicalHasAccel[slot] = true }
                            Sensor.TYPE_GYROSCOPE -> { for (i in 0 until minOf(3, event.values.size)) physicalGyro[slot][i] = event.values[i]; physicalHasGyro[slot] = true; pushPhysicalMotionSampleLocked(slot) }
                        }
                    }
                }
                override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
            }
            gravity?.let { sm.registerListener(listener, it, SensorManager.SENSOR_DELAY_GAME) }
            if (gravity == null) accel?.let { sm.registerListener(listener, it, SensorManager.SENSOR_DELAY_GAME) }
            sm.registerListener(listener, gyro, SensorManager.SENSOR_DELAY_GAME)
            physicalSensorManagers[slot] = sm
            physicalSensorListeners[slot] = listener
            physicalPads[slot].hasGyro = true
        } catch (t: Throwable) {
            Log.d(TAG, "controller gyro unavailable slot=${slot + 1}: ${t.message}")
        }
    }

    private fun pushPhysicalMotionSampleLocked(slot: Int) {
        if (!physicalHasGyro[slot] || (!physicalHasGravity[slot] && !physicalHasAccel[slot])) return
        val accel = if (physicalHasGravity[slot]) physicalGravity[slot] else physicalAccel[slot]
        val a = remapSensorForDisplay(accel)
        val g = remapSensorForDisplay(physicalGyro[slot])
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
        val pad = physicalPads[slot]
        pad.motionSamples[0] = pad.motionSamples[1]
        pad.motionSamples[1] = pad.motionSamples[2]
        pad.motionSamples[2] = sample
        if (pad.motionSampleCount < Protocol.MOTION_SAMPLE_COUNT) pad.motionSampleCount++
        pad.hasMotion = pad.motionSampleCount >= Protocol.MOTION_SAMPLE_COUNT
    }

    private fun stopPhysicalControllerSensors() {
        synchronized(physicalLock) { stopPhysicalControllerSensorsLocked(clearPads = true) }
    }

    private fun stopPhysicalControllerSensorsLocked(clearPads: Boolean) {
        for (i in 0 until Protocol.PAD_COUNT) {
            try {
                val sm = physicalSensorManagers[i]
                val listener = physicalSensorListeners[i]
                if (sm != null && listener != null) sm.unregisterListener(listener)
            } catch (_: Throwable) {}
            physicalSensorManagers[i] = null
            physicalSensorListeners[i] = null
            physicalHasGravity[i] = false; physicalHasAccel[i] = false; physicalHasGyro[i] = false
            if (clearPads) physicalPads[i].reset()
        }
    }

    private fun updateHubStatusOnPage(prefix: String? = null) {
        if (currentPage != Page.MAIN_MENU) return
        val lines = synchronized(physicalLock) {
            Array(Protocol.PAD_COUNT) { i ->
                val p = physicalPads[i]
                if (!p.present) "P${i + 1}: Empty"
                else "P${i + 1}: ${p.name}  gyro=${if (p.hasGyro) "yes" else "no"}  rumble=${if (p.hasRumble) "yes" else "no"}"
            }
        }
        val status = prefix ?: when (activeClientMode) {
            ClientMode.HUB -> "Controller Hub running"
            ClientMode.TOUCH -> "Touch Controls running"
            ClientMode.NONE -> "Ready"
        }
        fun jsEscape(v: String) = v.replace("\", "\\").replace("'", "\'").replace("\n", " ")
        val js = buildString {
            append("(function(){")
            append("var s=document.getElementById('statusText'); if(s)s.textContent='").append(jsEscape(status)).append("';")
            for (i in 0 until Protocol.PAD_COUNT) {
                append("var p=document.getElementById('p").append(i + 1).append("Text'); if(p)p.textContent='").append(jsEscape(lines[i])).append("';")
            }
            append("})()")
        }
        try { webView.evaluateJavascript(js, null) } catch (_: Throwable) {}
    }

    override fun onDestroy() {
        disconnect()
        try { if (::inputManager.isInitialized) inputManager.unregisterInputDeviceListener(inputDeviceListener) } catch (_: Throwable) {}
        super.onDestroy()
    }

    private fun activateControlClient() {
        if (controlClientActive && activeClientMode == ClientMode.TOUCH) return
        deactivateControlClient()
        activeClientMode = ClientMode.TOUCH
        touchHid = null
        touchFrame = null
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        controlClientActive = true
        if (!connectWs()) {
            controlClientActive = false
            activeClientMode = ClientMode.NONE
        }
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
        stopPhysicalControllerSensors()
        stopAllPhysicalRumble()
        phoneRumble(0, 0)
        activeClientMode = ClientMode.NONE

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
