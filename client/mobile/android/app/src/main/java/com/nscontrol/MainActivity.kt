package com.nscontrol

import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Bundle
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.view.*
import android.webkit.*
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import okhttp3.*
import okio.ByteString
import okio.ByteString.Companion.toByteString
import java.io.ByteArrayInputStream
import java.net.HttpURLConnection
import java.net.URL
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.abs
import kotlin.math.roundToInt

class MainActivity : AppCompatActivity() {
    private lateinit var connectView: View
    private lateinit var webView: WebView
    private lateinit var hostInput: EditText
    private lateinit var statusText: TextView
    private var host = ""
    private var connected = false // Web UI loaded
    @Volatile private var controlClientActive = false // actual input/rumble WebSocket client
    private var ws: WebSocket? = null
    private val client = OkHttpClient.Builder().build()
    private var seq = 0
    private var sending = false

    // Gyro (rad/s)
    private val sensorManager by lazy { getSystemService(SENSOR_SERVICE) as SensorManager }
    private var gyroListener: SensorEventListener? = null
    private var accelListener: SensorEventListener? = null
    private var gyroValues = FloatArray(3) // x, y, z angular velocity, rad/s
    private var accelValues = floatArrayOf(0f, STANDARD_GRAVITY, 0f) // x, y, z acceleration, m/s^2

    // Haptics
    private val vibrator by lazy { getSystemService(VIBRATOR_SERVICE) as Vibrator }

    // Touch data bridged from WebView JS
    @Volatile private var touchFrame: ByteArray? = null
    @Volatile private var lastTouchFrameMs: Long = 0

    // Native Bluetooth/USB controller state. Android exposes normal buttons/sticks
    // and sometimes a rumble motor. It does not reliably expose Switch/Pro gyro as
    // controller IMU, so motion still comes from the phone sensors on Android.
    private data class ControllerState(
        var deviceId: Int = -1,
        var buttons: Int = 0,
        var lx: Float = 0f,
        var ly: Float = 0f,
        var rx: Float = 0f,
        var ry: Float = 0f,
        var dpadUp: Boolean = false,
        var dpadDown: Boolean = false,
        var dpadLeft: Boolean = false,
        var dpadRight: Boolean = false,
        var lastInputMs: Long = 0
    )
    private val controllerLock = Any()
    private val controllerState = ControllerState()

    // Navigation
    private enum class Page { MAIN_MENU, TOUCH_CONTROLS, EDITOR }
    private var currentPage = Page.MAIN_MENU
    private val pageStack = mutableListOf<Page>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        connectView = layoutInflater.inflate(R.layout.connect, null)
        webView = WebView(this)
        hostInput = connectView.findViewById(R.id.hostInput)
        statusText = connectView.findViewById(R.id.statusText)
        connectView.findViewById<Button>(R.id.connectBtn).setOnClickListener { onConnect() }
        setContentView(connectView)
        getPreferences(MODE_PRIVATE).getString("host", "")?.let {
            hostInput.setText(it); hostInput.setSelection(it.length)
        }
    }

    private fun onConnect() {
        host = hostInput.text.toString().trim()
        if (host.isEmpty()) return
        getPreferences(MODE_PRIVATE).edit().putString("host", host).apply()
        setupWebView()
        setContentView(webView)
        connected = true
        currentPage = Page.MAIN_MENU
        pageStack.clear()
        loadUrl("http://$host:8080/")
        statusText.text = "Loaded"
    }

    // ═══════════════════════════════
    //  WebSocket
    // ═══════════════════════════════

    private fun connectWs() {
        val req = Request.Builder().url("ws://$host:8080/").build()
        ws = client.newWebSocket(req, object : WebSocketListener() {
            override fun onOpen(w: WebSocket, r: Response) {
                runOnUiThread { statusText.text = "Connected" }
            }
            override fun onClosed(w: WebSocket, code: Int, reason: String) {
                runOnUiThread { statusText.text = "Disconnected" }
            }
            override fun onFailure(w: WebSocket, t: Throwable, r: Response?) {
                runOnUiThread { statusText.text = "Connection failed" }
            }
            override fun onMessage(w: WebSocket, bytes: ByteString) {
                if (bytes.size >= 8) {
                    val low = bytes[5].toInt() and 0xFF
                    val high = bytes[6].toInt() and 0xFF
                    routeRumble(low, high)
                }
            }
        })
    }

    // ═══════════════════════════════
    //  Send loop (~250 Hz)
    // ═══════════════════════════════

    private fun startSending() {
        if (sending) return; sending = true
        Thread {
            while (sending && controlClientActive) {
                sendFrame()
                Thread.sleep(4)
            }
        }.start()
    }

    private fun sendFrame() {
        sendFrameInternal(flagsOverride = null)
    }

    private fun sendResetFrame() {
        // FLAG_RESET = 0x01. This releases all pads immediately before the WS closes.
        sendFrameInternal(flagsOverride = 0x01, forceNeutral = true)
    }

    private fun sendFrameInternal(flagsOverride: Int? = null, forceNeutral: Boolean = false) {
        val controller = if (forceNeutral) null else snapshotController()
        val touchActive = !forceNeutral && touchModeActive()
        val buf = ByteBuffer.allocate(116).order(ByteOrder.LITTLE_ENDIAN)
        buf.putInt(0x4E535743)        // magic
        buf.put(5)                     // WEB_PROTO_VERSION
        val flags = flagsOverride ?: if (controller == null && touchActive) 0x04 else 0 // FLAG_SINGLE_PAD only while the touch page is active
        buf.put(flags.toByte())
        buf.putShort(0)                // reserved
        buf.putInt(seq++)
        buf.putLong(System.nanoTime() / 1000)

        for (p in 0 until 4) {
            if (p == 0 && !forceNeutral) {
                if (controller != null) {
                    putControllerHid(buf, controller)
                } else {
                    val touch = touchFrame
                    if (touchActive && touch != null && touch.size >= 28) {
                        // The browser/mobile page sends a complete 116-byte web frame.
                        // Pad 0's HIDReport starts at byte 20 and is 8 bytes long.
                        buf.put(touch.sliceArray(20 until 28))
                    } else {
                        putNeutralHid(buf)
                    }
                }
                // Touch mode uses phone gyro. Android's public controller APIs usually
                // do not expose Switch Pro / Bluetooth gamepad IMU, so controller gyro
                // uses phone IMU as a fallback only when a controller is active.
                if (controller != null || touchActive) putMotion(buf) else buf.put(ByteArray(16))
            } else {
                putNeutralHid(buf)
                buf.put(ByteArray(16))
            }
        }
        ws?.send(buf.array().toByteString())
    }

    private fun putNeutralHid(buf: ByteBuffer) {
        buf.putShort(0)  // buttons
        buf.put(8)       // neutral hat
        buf.put(128.toByte()); buf.put(128.toByte()) // left stick
        buf.put(128.toByte()); buf.put(128.toByte()) // right stick
        buf.put(0)       // vendor/present byte; FLAG_SINGLE_PAD keeps pad 0 present server-side
    }

    private fun putControllerHid(buf: ByteBuffer, st: ControllerState) {
        buf.putShort(st.buttons.toShort())
        buf.put(resolveControllerHat(st))
        buf.put(axisToByte(st.lx))
        buf.put(axisToByte(st.ly))
        buf.put(axisToByte(st.rx))
        buf.put(axisToByte(st.ry))
        buf.put(0x01) // EXT_PAD_PRESENT
    }

    private fun resolveControllerHat(st: ControllerState): Byte {
        return when {
            st.dpadUp && st.dpadRight -> 1
            st.dpadUp && st.dpadLeft -> 7
            st.dpadDown && st.dpadRight -> 3
            st.dpadDown && st.dpadLeft -> 5
            st.dpadUp -> 0
            st.dpadRight -> 2
            st.dpadDown -> 4
            st.dpadLeft -> 6
            else -> 8
        }.toByte()
    }

    private fun axisToByte(v: Float): Byte {
        val clamped = v.coerceIn(-1f, 1f)
        return ((clamped + 1f) * 127.5f).roundToInt().coerceIn(0, 255).toByte()
    }

    private fun putMotion(buf: ByteBuffer) {
        val a = accelValues
        val g = gyroValues

        // Match the desktop SDL path and protocol.hpp layout:
        // MotionReport = ax, ay, az, gx, gy, gz.
        val ax = clampMotion(-a[0] * ACCEL_SCALE)
        val ay = clampMotion(-a[2] * ACCEL_SCALE)
        val az = clampMotion( a[1] * ACCEL_SCALE)
        val gx = clampMotion(-g[0] * GYRO_SCALE)
        val gy = clampMotion(-g[2] * GYRO_SCALE)
        val gz = clampMotion( g[1] * GYRO_SCALE)

        buf.putShort(ax); buf.putShort(ay); buf.putShort(az)
        buf.putShort(gx); buf.putShort(gy); buf.putShort(gz)
        buf.put(1)          // has_motion
        buf.put(ByteArray(3))
    }

    // ═══════════════════════════════
    //  Gyro
    // ═══════════════════════════════

    private fun startGyro() {
        val gyro = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        if (gyro != null) {
            gyroListener = object : SensorEventListener {
                override fun onSensorChanged(e: SensorEvent) {
                    System.arraycopy(e.values, 0, gyroValues, 0, 3)
                }
                override fun onAccuracyChanged(s: Sensor, a: Int) {}
            }
            sensorManager.registerListener(gyroListener, gyro, SensorManager.SENSOR_DELAY_FASTEST)
        }

        val accel = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        if (accel != null) {
            accelListener = object : SensorEventListener {
                override fun onSensorChanged(e: SensorEvent) {
                    System.arraycopy(e.values, 0, accelValues, 0, 3)
                }
                override fun onAccuracyChanged(s: Sensor, a: Int) {}
            }
            sensorManager.registerListener(accelListener, accel, SensorManager.SENSOR_DELAY_GAME)
        }
    }

    // ═══════════════════════════════
    //  Haptics
    // ═══════════════════════════════

    private fun routeRumble(low: Int, high: Int) {
        val controller = snapshotController()
        if (low == 0 && high == 0) {
            if (controller != null) {
                InputDevice.getDevice(controller.deviceId)?.vibrator?.cancel()
            } else {
                vibrator.cancel()
            }
            return
        }

        // If a Bluetooth/USB controller is the active input source, never rumble
        // the phone as a fake fallback. Either rumble the controller, or stay silent.
        if (controller != null) {
            playControllerRumble(controller.deviceId, low, high)
            return
        }

        // Do not rumble the phone from the main menu / editor. Phone haptics are
        // only for the actual touch-controls page while it is actively sending input.
        if (touchModeActive()) playPhoneHaptic(low, high)
    }

    private fun touchModeActive(): Boolean {
        val t = lastTouchFrameMs
        return controlClientActive && currentPage == Page.TOUCH_CONTROLS && t != 0L && SystemClock.uptimeMillis() - t < 750L
    }

    private fun playControllerRumble(deviceId: Int, low: Int, high: Int): Boolean {
        val v = InputDevice.getDevice(deviceId)?.vibrator ?: return false
        if (!v.hasVibrator()) return false
        val amp = ((low + high) / 2).coerceIn(1, 255)
        v.vibrate(VibrationEffect.createOneShot(70, amp))
        return true
    }

    private fun playPhoneHaptic(low: Int, high: Int) {
        if (low == 0 && high == 0) return
        val amp = ((low + high) / 2).coerceIn(1, 255)
        vibrator.vibrate(VibrationEffect.createOneShot(50, amp))
    }

    // ═══════════════════════════════
    //  WebView
    // ═══════════════════════════════

    private fun setupWebView() {
        val baseUserAgent = webView.settings.userAgentString ?: ""
        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            allowFileAccess = false
            mixedContentMode = WebSettings.MIXED_CONTENT_ALWAYS_ALLOW
            userAgentString = "$baseUserAgent NSControl/1.0"
        }
        webView.addJavascriptInterface(JSBridge(), "NSBridge")
        webView.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(v: WebView, req: WebResourceRequest): Boolean {
                val url = req.url.toString()
                return when {
                    url.endsWith("/mobile") -> { navTo(Page.TOUCH_CONTROLS, url); true }
                    url.endsWith("/editor") -> { navTo(Page.EDITOR, url); true }
                    else -> false
                }
            }

            // Inject bridge JS by intercepting the HTML and prepending the script
            override fun shouldInterceptRequest(
                v: WebView, req: WebResourceRequest
            ): WebResourceResponse? {
                val url = req.url.toString()
                if (!req.isForMainFrame || !url.startsWith("http://$host:8080")) return null
                return try {
                    val conn = URL(url).openConnection() as HttpURLConnection
                    conn.connectTimeout = 5000
                    conn.readTimeout = 5000
                    val ctype = conn.contentType ?: "text/html"
                    val data = conn.inputStream.readBytes()
                    val body = String(data)
                    // Only inject into HTML responses that have </head>
                    val headIdx = body.indexOf("</head>")
                    if (headIdx < 0) {
                        WebResourceResponse(ctype, "UTF-8", ByteArrayInputStream(data))
                    } else {
                        val injected = StringBuilder(body)
                        injected.insert(headIdx, BRIDGE_SCRIPT)
                        WebResourceResponse(ctype, "UTF-8",
                            ByteArrayInputStream(injected.toString().toByteArray()))
                    }
                } catch (_: Exception) { null }
            }

            override fun onPageFinished(v: WebView, url: String) {
                if (currentPage == Page.TOUCH_CONTROLS || currentPage == Page.EDITOR) {
                    injectBackButton(v)
                }
            }
        }
    }

    private fun loadUrl(url: String) { webView.loadUrl(url) }

    private fun navTo(page: Page, url: String) {
        pageStack.add(currentPage)
        enterPage(page, url)
    }

    private fun enterPage(page: Page, url: String) {
        currentPage = page
        if (page == Page.TOUCH_CONTROLS) activateControlClient() else deactivateControlClient()
        loadUrl(url)
    }

    private fun goBack() {
        if (pageStack.isNotEmpty()) {
            val previous = pageStack.removeLast()
            enterPage(previous, when (previous) {
                Page.MAIN_MENU -> "http://$host:8080/"
                Page.TOUCH_CONTROLS -> "http://$host:8080/mobile"
                Page.EDITOR -> "http://$host:8080/editor"
            })
        }
    }

    private fun injectBackButton(v: WebView) {
        v.evaluateJavascript(INJECT_BACK_JS, null)
    }

    // ═══════════════════════════════
    //  JavaScript Bridge Interface
    // ═══════════════════════════════

    inner class JSBridge {
        @JavascriptInterface
        fun onBinary(json: String) {
            if (currentPage != Page.TOUCH_CONTROLS || !controlClientActive) return
            try {
                val arr = org.json.JSONArray(json)
                touchFrame = ByteArray(arr.length()) { i -> arr.getInt(i).toByte() }
                lastTouchFrameMs = SystemClock.uptimeMillis()
            } catch (_: Exception) {}
        }

        @JavascriptInterface
        fun onClose() { runOnUiThread { deactivateControlClient() } }

        @JavascriptInterface
        fun onBack() { runOnUiThread { goBack() } }
    }

    // ═══════════════════════════════
    //  Native Bluetooth/USB controller input
    // ═══════════════════════════════

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (isControllerSource(event.source) && handleControllerKey(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (!isControllerSource(event.source)) return super.onGenericMotionEvent(event)
        val device = event.device ?: return super.onGenericMotionEvent(event)
        synchronized(controllerLock) {
            controllerState.deviceId = event.deviceId
            controllerState.lx = centeredAxis(event, device, MotionEvent.AXIS_X)
            controllerState.ly = centeredAxis(event, device, MotionEvent.AXIS_Y)
            val rz = centeredAxis(event, device, MotionEvent.AXIS_RZ)
            val z = centeredAxis(event, device, MotionEvent.AXIS_Z)
            val rx = centeredAxis(event, device, MotionEvent.AXIS_RX)
            val ry = centeredAxis(event, device, MotionEvent.AXIS_RY)
            controllerState.rx = if (abs(z) >= abs(rx)) z else rx
            controllerState.ry = if (abs(rz) >= abs(ry)) rz else ry
            val hatX = centeredAxis(event, device, MotionEvent.AXIS_HAT_X)
            val hatY = centeredAxis(event, device, MotionEvent.AXIS_HAT_Y)
            controllerState.dpadLeft = hatX < -0.5f
            controllerState.dpadRight = hatX > 0.5f
            controllerState.dpadUp = hatY < -0.5f
            controllerState.dpadDown = hatY > 0.5f

            val lt = event.getAxisValue(MotionEvent.AXIS_LTRIGGER).coerceIn(0f, 1f)
            val rt = event.getAxisValue(MotionEvent.AXIS_RTRIGGER).coerceIn(0f, 1f)
            if (lt > 0.5f) controllerState.buttons = controllerState.buttons or BTN_ZL
            else controllerState.buttons = controllerState.buttons and BTN_ZL.inv()
            if (rt > 0.5f) controllerState.buttons = controllerState.buttons or BTN_ZR
            else controllerState.buttons = controllerState.buttons and BTN_ZR.inv()

            controllerState.lastInputMs = SystemClock.uptimeMillis()
        }
        return true
    }

    private fun handleControllerKey(event: KeyEvent): Boolean {
        val down = event.action == KeyEvent.ACTION_DOWN
        if (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) return false
        var handled = true
        synchronized(controllerLock) {
            controllerState.deviceId = event.deviceId
            when (event.keyCode) {
                KeyEvent.KEYCODE_BUTTON_A -> setControllerButton(BTN_B, down)      // physical SOUTH -> Switch B
                KeyEvent.KEYCODE_BUTTON_B -> setControllerButton(BTN_A, down)      // physical EAST  -> Switch A
                KeyEvent.KEYCODE_BUTTON_X -> setControllerButton(BTN_Y, down)      // physical WEST  -> Switch Y
                KeyEvent.KEYCODE_BUTTON_Y -> setControllerButton(BTN_X, down)      // physical NORTH -> Switch X
                KeyEvent.KEYCODE_BUTTON_L1 -> setControllerButton(BTN_L, down)
                KeyEvent.KEYCODE_BUTTON_R1 -> setControllerButton(BTN_R, down)
                KeyEvent.KEYCODE_BUTTON_L2 -> setControllerButton(BTN_ZL, down)
                KeyEvent.KEYCODE_BUTTON_R2 -> setControllerButton(BTN_ZR, down)
                KeyEvent.KEYCODE_BUTTON_SELECT -> setControllerButton(BTN_MINUS, down)
                KeyEvent.KEYCODE_BUTTON_START -> setControllerButton(BTN_PLUS, down)
                KeyEvent.KEYCODE_BUTTON_THUMBL -> setControllerButton(BTN_LSTICK, down)
                KeyEvent.KEYCODE_BUTTON_THUMBR -> setControllerButton(BTN_RSTICK, down)
                KeyEvent.KEYCODE_BUTTON_MODE -> setControllerButton(BTN_HOME, down)
                KeyEvent.KEYCODE_DPAD_UP -> controllerState.dpadUp = down
                KeyEvent.KEYCODE_DPAD_DOWN -> controllerState.dpadDown = down
                KeyEvent.KEYCODE_DPAD_LEFT -> controllerState.dpadLeft = down
                KeyEvent.KEYCODE_DPAD_RIGHT -> controllerState.dpadRight = down
                else -> handled = false
            }
            if (handled) controllerState.lastInputMs = SystemClock.uptimeMillis()
        }
        return handled
    }

    private fun setControllerButton(mask: Int, down: Boolean) {
        controllerState.buttons = if (down) controllerState.buttons or mask else controllerState.buttons and mask.inv()
    }

    private fun snapshotController(): ControllerState? {
        synchronized(controllerLock) {
            val id = controllerState.deviceId
            if (id < 0 || InputDevice.getDevice(id) == null) return null
            return controllerState.copy()
        }
    }

    private fun isControllerSource(source: Int): Boolean {
        return (source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
               (source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
    }

    private fun centeredAxis(event: MotionEvent, device: InputDevice, axis: Int): Float {
        val range = device.getMotionRange(axis, event.source)
        val value = event.getAxisValue(axis)
        val flat = range?.flat ?: 0.05f
        return if (abs(value) > flat) value.coerceIn(-1f, 1f) else 0f
    }

    // ═══════════════════════════════
    //  Android back button
    // ═══════════════════════════════

    override fun onBackPressed() {
        when {
            connected && currentPage != Page.MAIN_MENU -> goBack()
            connected -> disconnect()
            else -> super.onBackPressed()
        }
    }

    override fun onDestroy() {
        disconnect(); super.onDestroy()
    }

    private fun activateControlClient() {
        if (controlClientActive) return
        touchFrame = null
        lastTouchFrameMs = 0
        controlClientActive = true
        connectWs()
        startGyro()
        startSending()
    }

    private fun deactivateControlClient() {
        if (!controlClientActive && ws == null && !sending) return
        // Send a few reset frames before closing so the backend releases inputs immediately.
        repeat(3) {
            sendResetFrame()
            Thread.sleep(4)
        }
        sending = false
        controlClientActive = false
        touchFrame = null
        lastTouchFrameMs = 0
        vibrator.cancel()
        ws?.close(1000, "Leaving controller mode")
        ws = null
        stopGyro()
    }

    private fun disconnect() {
        deactivateControlClient()
        connected = false
        webView.loadUrl("about:blank")
        setContentView(connectView)
    }

    private fun stopGyro() {
        gyroListener?.let { sensorManager.unregisterListener(it) }
        accelListener?.let { sensorManager.unregisterListener(it) }
        gyroListener = null
        accelListener = null
    }

    private fun clampMotion(v: Float): Short {
        return when {
            v > 32767f -> 32767
            v < -32768f -> -32768
            else -> v.roundToInt().toShort()
        }
    }

    companion object {
        private const val BTN_Y = 1 shl 0
        private const val BTN_B = 1 shl 1
        private const val BTN_A = 1 shl 2
        private const val BTN_X = 1 shl 3
        private const val BTN_L = 1 shl 4
        private const val BTN_R = 1 shl 5
        private const val BTN_ZL = 1 shl 6
        private const val BTN_ZR = 1 shl 7
        private const val BTN_MINUS = 1 shl 8
        private const val BTN_PLUS = 1 shl 9
        private const val BTN_LSTICK = 1 shl 10
        private const val BTN_RSTICK = 1 shl 11
        private const val BTN_HOME = 1 shl 12

        private const val STANDARD_GRAVITY = 9.80665f
        private const val ACCEL_SCALE = 4096.0f / STANDARD_GRAVITY
        private const val GYRO_SCALE = 938.732f // 57.2958 * 16.384, rad/s → Switch IMU units

        // Injected at <head> — overrides WebSocket to bridge through native
        private val BRIDGE_SCRIPT = """
<script>
(function(){
if (window.__bridgeLoaded) return;
window.__bridgeLoaded = true;
window.__bridge = {
    send: function(data) {
        if (data instanceof ArrayBuffer) {
            var u8 = new Uint8Array(data);
            var arr = [];
            for (var i = 0; i < u8.length; i++) arr.push(u8[i]);
            NSBridge.onBinary(JSON.stringify(arr));
        }
    }
};
var OrigWS = window.WebSocket;
window.WebSocket = function(url, protocols) {
    this.readyState = 0;
    this.binaryType = 'arraybuffer';
    this.onopen = null; this.onclose = null; this.onerror = null; this.onmessage = null;
    this.send = function(data) { window.__bridge.send(data); };
    this.close = function() { NSBridge.onClose(); };
    setTimeout(function() {
        this.readyState = 1;
        if (this.onopen) this.onopen();
    }.bind(this), 0);
};
window.WebSocket.CONNECTING = 0;
window.WebSocket.OPEN = 1;
window.WebSocket.CLOSING = 2;
window.WebSocket.CLOSED = 3;
})();
</script>
"""

        private const val INJECT_BACK_JS = """
(function(){
var b = document.createElement('div');
b.textContent = '\u2190 Back';
b.style.cssText = 'position:fixed;top:12px;left:12px;z-index:99999;background:rgba(0,0,0,0.55);color:#fff;padding:8px 18px;border-radius:20px;font-size:15px;cursor:pointer;-webkit-user-select:none;';
b.onclick = function(){ NSBridge.onBack(); };
document.body.appendChild(b);
})();
"""
    }
}
