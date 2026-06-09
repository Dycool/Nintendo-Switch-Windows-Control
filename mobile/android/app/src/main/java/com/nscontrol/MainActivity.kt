package com.nscontrol

import android.os.Bundle
import android.os.Build
import android.os.SystemClock
import android.view.*
import android.webkit.*
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.activity.OnBackPressedCallback
import okhttp3.*
import okio.ByteString
import okio.ByteString.Companion.toByteString
import java.util.concurrent.atomic.AtomicInteger

class MainActivity : AppCompatActivity() {
    private lateinit var connectView: View
    private lateinit var webView: WebView
    private lateinit var hostInput: EditText
    private lateinit var statusText: TextView
    private var host = ""
    private var connected = false
    @Volatile private var controlClientActive = false
    private var ws: WebSocket? = null
    private val client = OkHttpClient.Builder().build()
    private val seq = AtomicInteger(0)
    @Volatile private var sending = false
    private val sendLock = Any()

    @Volatile private var touchHid: ByteArray? = null
    @Volatile private var touchFrame: ByteArray? = null
    @Volatile private var lastTouchFrameMs: Long = 0
    @Volatile private var lastBridgeFrameParseMs: Long = 0

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
            hostInput.setText(it); hostInput.setSelection(it.length)
        }

        // Initialize SDL3 gamepad/sensor/haptic subsystem once on app start.
        if (!SDLController.init()) {
            statusText.text = "SDL init failed"
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
        loadUrl(pageUrl(Page.MAIN_MENU))
        statusText.text = "Loaded"
    }

    // ═══════════════════════════════
    //  WebSocket
    // ═══════════════════════════════

    private fun connectWs(): Boolean {
        return try {
            val cleanHost = normalizeHostForWs(host)
            val req = Request.Builder().url("ws://$cleanHost:8080/").build()
            ws = client.newWebSocket(req, object : WebSocketListener() {
                override fun onOpen(w: WebSocket, r: Response) {
                    runOnUiThread {
                        if (!controlClientActive || ws !== w) return@runOnUiThread
                        statusText.text = "Connected"
                        startSending()
                    }
                }
                override fun onClosed(w: WebSocket, code: Int, reason: String) {
                    runOnUiThread {
                        statusText.text = "Disconnected"
                        if (ws === w) {
                            sending = false
                            controlClientActive = false
                            touchHid = null
                            touchFrame = null
                            lastTouchFrameMs = 0
                            ws = null
                        }
                    }
                }
                override fun onFailure(w: WebSocket, t: Throwable, r: Response?) {
                    runOnUiThread {
                        statusText.text = "Connection failed"
                        if (ws === w) {
                            sending = false
                            controlClientActive = false
                            touchHid = null
                            touchFrame = null
                            lastTouchFrameMs = 0
                            ws = null
                        }
                    }
                }
                override fun onMessage(w: WebSocket, bytes: ByteString) {
                    try {
                        if (bytes.size >= 8) {
                            val subpad = bytes[4].toInt() and 0xFF
                            val low = bytes[5].toInt() and 0xFF
                            val high = bytes[6].toInt() and 0xFF
                            runOnUiThread {
                                try { routeRumble(subpad, low, high) } catch (_: Throwable) {}
                            }
                        }
                    } catch (_: Throwable) {}
                }
            })
            true
        } catch (e: Exception) {
            runOnUiThread { statusText.text = "Invalid server address" }
            false
        }
    }

    private fun normalizeHostForWs(raw: String): String {
        var h = raw.trim()
        h = h.removePrefix("http://").removePrefix("https://").removePrefix("ws://").removePrefix("wss://")
        h = h.substringBefore('/').trim()
        if (h.endsWith(":8080")) h = h.removeSuffix(":8080")
        return h
    }

    // ═══════════════════════════════
    //  Send loop (~250 Hz)
    // ═══════════════════════════════

    private fun startSending() {
        if (sending) return
        sending = true
        Thread {
            try {
                // Open phone sensors/haptics once for the session.
                // The SDL layer handles the actual open/close.
                if (SDLController.phoneSensorsOpen()) {
                    SDLController.phoneHapticOpen()
                }
                while (sending && controlClientActive) {
                    sendFrame()
                    Thread.sleep(4)
                }
            } catch (t: Throwable) {
                runOnUiThread {
                    statusText.text = "Input sender failed"
                    sending = false
                    controlClientActive = false
                    touchHid = null
                    touchFrame = null
                    lastTouchFrameMs = 0
                    try { ws?.close(1000, "Input sender failed") } catch (_: Throwable) {}
                    ws = null
                }
            } finally {
                sending = false
                SDLController.phoneHapticClose()
                SDLController.phoneSensorsClose()
            }
        }.start()
    }

    private fun sendFrame() {
        sendFrameInternal(flagsOverride = null)
    }

    private fun sendResetFrame() {
        ws?.let { sendResetFrameTo(it) }
    }

    private fun sendResetFrameTo(socket: WebSocket) {
        try {
            sendFrameInternal(socketOverride = socket, flagsOverride = Protocol.FLAG_RESET, forceNeutral = true)
        } catch (_: Throwable) {}
    }

    private fun sendFrameInternal(socketOverride: WebSocket? = null, flagsOverride: Int? = null, forceNeutral: Boolean = false) {
        synchronized(sendLock) {
            val socket = socketOverride ?: ws ?: return

            // Poll SDL3 for latest gamepad/sensor state before reading.
            SDLController.poll()

            val hasController = !forceNeutral && controllerClientActive() && SDLController.padConnected(0)
            val touchActive = !forceNeutral && touchClientActive()
            val flags = flagsOverride ?: if (!hasController && touchActive) Protocol.FLAG_SINGLE_PAD else 0
            val timestampUs = System.currentTimeMillis() * 1000L

            val pad0Hid: ByteArray? = when {
                forceNeutral -> null
                hasController -> Protocol.controllerHid(
                    SDLController.padButtons(0),
                    SDLController.padDpadUp(0),
                    SDLController.padDpadDown(0),
                    SDLController.padDpadLeft(0),
                    SDLController.padDpadRight(0),
                    SDLController.padLX(0),
                    SDLController.padLY(0),
                    SDLController.padRX(0),
                    SDLController.padRY(0),
                    true
                )
                touchActive -> touchHid ?: touchFrame?.let { Protocol.extractPad0HidFromWebFrame(it) } ?: Protocol.neutralHid()
                else -> null
            }

            val pad0Motion: ByteArray? = if (!forceNeutral && (hasController || touchActive)) {
                currentMotionBytes(hasController)
            } else {
                null
            }

            val frame = Protocol.buildFrame(
                seq.getAndIncrement(),
                flags,
                timestampUs,
                pad0Hid,
                pad0Motion
            )
            socket.send(frame.toByteString())
        }
    }

    private fun currentMotionBytes(hasController: Boolean): ByteArray {
        // Prefer the physical controller's IMU when connected and producing motion.
        if (hasController && SDLController.padHasMotion(0)) {
            val m = SDLController.padMotion(0) ?: return Protocol.neutralMotion()
            return Protocol.motionFromValues(m[0], m[1], m[2], m[3], m[4], m[5], hasMotion = true)
        }
        // Fall back to phone sensors (touch mode, or controller without IMU).
        val m = SDLController.phoneSensorsRead()
            ?: return Protocol.neutralMotion()
        // Phone sensors are in device coordinates; remap to the current display orientation.
        val remapped = remapMotionForDisplay(m)
        return Protocol.motionFromValues(
            remapped[0], remapped[1], remapped[2],
            remapped[3], remapped[4], remapped[5],
            hasMotion = true
        )
    }

    private fun remapMotionForDisplay(m: ShortArray): ShortArray {
        // SDL convert_motion produces:
        //   ax = -accel_x, ay = -accel_z, az = +accel_y
        //   gx = -gyro_x, gy = -gyro_z, gz = +gyro_y
        // Remap to align with the current display rotation.
        val rotation = try {
            if (Build.VERSION.SDK_INT >= 30) display?.rotation ?: Surface.ROTATION_0
            else legacyDisplayRotation()
        } catch (_: Throwable) { Surface.ROTATION_0 }
        val ax = m[0]; val ay = m[1]; val az = m[2]
        val gx = m[3]; val gy = m[4]; val gz = m[5]
        return when (rotation) {
            Surface.ROTATION_90  -> shortArrayOf(az, ay, (-ax).toShort(), gz, gy, (-gx).toShort())
            Surface.ROTATION_180 -> shortArrayOf((-ax).toShort(), ay, (-az).toShort(), (-gx).toShort(), gy, (-gz).toShort())
            Surface.ROTATION_270 -> shortArrayOf((-az).toShort(), ay, ax, (-gz).toShort(), gy, gx)
            else -> shortArrayOf(ax, ay, az, gx, gy, gz)
        }
    }

    private fun remapSensorForDisplay(v: FloatArray): FloatArray {
        val rotation = try {
            if (Build.VERSION.SDK_INT >= 30) display?.rotation ?: Surface.ROTATION_0
            else legacyDisplayRotation()
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

    // ═══════════════════════════════
    //  Haptics / Rumble
    // ═══════════════════════════════

    private fun routeRumble(subpad: Int, low: Int, high: Int) {
        if (low == 0 && high == 0) {
            // Stop rumble on the requested subpad and phone haptics.
            if (subpad >= 0 && subpad < Protocol.PAD_COUNT) {
                if (SDLController.padConnected(subpad)) SDLController.padStopRumble(subpad)
            }
            if (subpad == 0) SDLController.phoneHapticRumble(0, 0)
            return
        }

        if (subpad >= 0 && subpad < Protocol.PAD_COUNT && SDLController.padConnected(subpad)) {
            SDLController.padRumble(subpad, low, high, 50)
            return
        }

        // No physical controller — fall back to phone haptics for touch controls.
        if (touchClientActive()) SDLController.phoneHapticRumble(low, high)
    }

    private fun touchClientActive(): Boolean {
        return controlClientActive && currentPage == Page.TOUCH_CONTROLS
    }

    private fun controllerClientActive(): Boolean {
        return controlClientActive && currentPage == Page.MAIN_MENU
    }

    // ═══════════════════════════════
    //  WebView
    // ═══════════════════════════════

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
                    url.endsWith("/mobile") || url.endsWith("/mobile.html") -> {
                        navTo(Page.TOUCH_CONTROLS)
                        true
                    }
                    url.endsWith("/editor") || url.endsWith("/editor.html") -> {
                        navTo(Page.EDITOR)
                        true
                    }
                    else -> false
                }
            }

            override fun onPageFinished(v: WebView, url: String) {
                if (currentPage == Page.MAIN_MENU) injectBranding(v)
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
        if (page != Page.TOUCH_CONTROLS) deactivateControlClient()
        loadUrl(pageUrl(page))
    }

    private fun goBack() {
        if (pageStack.isNotEmpty()) {
            val previous = pageStack.removeLast()
            enterPage(previous)
        }
    }

    private fun injectBranding(v: WebView) {
        v.evaluateJavascript(INJECT_BRANDING_JS, null)
    }

    // ═══════════════════════════════
    //  JavaScript Bridge Interface
    // ═══════════════════════════════

    inner class JSBridge {
        @JavascriptInterface
        fun onOpen() {
            runOnUiThread {
                if (currentPage == Page.TOUCH_CONTROLS || currentPage == Page.MAIN_MENU) activateControlClient()
            }
        }

        @JavascriptInterface
        fun onBinary(json: String) {
            if (currentPage != Page.TOUCH_CONTROLS || !controlClientActive) return
            try {
                val now = SystemClock.uptimeMillis()
                if (now - lastBridgeFrameParseMs < 8L) return
                lastBridgeFrameParseMs = now

                val arr = org.json.JSONArray(json)
                if (arr.length() < Protocol.FRAME_SIZE) return
                val frame = ByteArray(arr.length()) { i -> arr.getInt(i).toByte() }
                touchFrame = frame
                touchHid = Protocol.extractPad0HidFromWebFrame(frame)
                lastTouchFrameMs = now
            } catch (_: Throwable) {}
        }

        @JavascriptInterface
        fun onTouchState(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int) {
            if (currentPage != Page.TOUCH_CONTROLS || !controlClientActive) return
            try {
                val hid = ByteArray(Protocol.HID_SIZE)
                hid[0] = (buttons and 0xFF).toByte()
                hid[1] = ((buttons ushr 8) and 0xFF).toByte()
                hid[2] = hat.coerceIn(0, 8).toByte()
                hid[3] = lx.coerceIn(0, 255).toByte()
                hid[4] = ly.coerceIn(0, 255).toByte()
                hid[5] = rx.coerceIn(0, 255).toByte()
                hid[6] = ry.coerceIn(0, 255).toByte()
                hid[7] = 1
                touchHid = hid
                lastTouchFrameMs = SystemClock.uptimeMillis()
            } catch (_: Throwable) {}
        }

        @JavascriptInterface
        fun onClose() { runOnUiThread { deactivateControlClient() } }

        @JavascriptInterface
        fun onBack() { runOnUiThread { goBack() } }
    }

    // ═══════════════════════════════
    //  Lifecycle
    // ═══════════════════════════════

    override fun onDestroy() {
        disconnect()
        SDLController.quit()
        super.onDestroy()
    }

    private fun activateControlClient() {
        if (controlClientActive) return
        touchHid = null
        touchFrame = null
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        controlClientActive = true
        if (!connectWs()) {
            controlClientActive = false
            return
        }
    }

    private fun deactivateControlClient() {
        if (!controlClientActive && ws == null && !sending) return
        val closingWs = ws
        sending = false
        controlClientActive = false
        touchHid = null
        touchFrame = null
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        ws = null

        // Stop all rumble before closing.
        for (i in 0 until Protocol.PAD_COUNT) {
            if (SDLController.padConnected(i)) SDLController.padStopRumble(i)
        }
        SDLController.phoneHapticRumble(0, 0)

        if (closingWs != null) {
            Thread {
                try {
                    repeat(3) {
                        sendResetFrameTo(closingWs)
                        try { Thread.sleep(4) } catch (_: InterruptedException) { return@Thread }
                    }
                    try { closingWs.close(1000, "Leaving controller mode") } catch (_: Throwable) {}
                } catch (_: Throwable) {}
            }.start()
        }
    }

    private fun disconnect() {
        deactivateControlClient()
        connected = false
        webView.loadUrl("about:blank")
        setContentView(connectView)
    }

    companion object {
        private const val INJECT_BRANDING_JS = """
(function(){
var old = document.getElementById('ns-mobile-brand');
if (old) old.remove();
var b = document.createElement('div');
b.id = 'ns-mobile-brand';
b.style.cssText = 'position:fixed;top:12px;left:12px;z-index:99998;display:flex;align-items:center;gap:8px;background:rgba(255,255,255,0.92);color:#111;padding:7px 12px;border-radius:18px;font:700 15px system-ui,-apple-system,Segoe UI,sans-serif;box-shadow:0 3px 16px rgba(0,0,0,0.18);pointer-events:none;';
var img = document.createElement('img');
img.src = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAGAAAABgCAYAAADimHc4AAA/iklEQVR4nJW9ebRlx13f+6mqvfeZz5379iB1S+rWZM0eZVt4wAJjgwkQZCAEO0CCnbcCvDzeCoE8nqwszFsEeDE4ecEmQCAxAcvY2NjYxmBZWB4l2ZZkja1uqefuO997pj1V/d4fezy3W4LsXrfPsPeuXfX9zb9fVR3FP/AQEXXvvei3v13Z4ru7P/Chxd7SwetUo32r1+rM2zR9CVrPKO2hlG4pcZcrpRoiKK2UUkZlbSFKoxCUUuWrZI0q0Cicqp6tAQGVn5bie3fpjiICSilBBEFQiDhElFPiRCB7mIhz4kQEiJTW51FqIoIqzokICE5wTpwVnN0yfvPJJBwnSTg67sbDZ1cf/NRz73//+3eKx39YxNwFTikll+re7kP9/ZfAXR/+sLn37W+3AL/+6/+1p2+49S00u98vyrzaYS7TfiNwSmNFYUUQUYDgrMsBKNFB5aMvSYEq3yMgOcwlQS7qolziuxc68msv1URx5M/TWoFSU+ennqQUWmW00wpcHKKwI+3SEypNvhEPVj+58sWPfbogRh2zFztedCQiot7zHtQ99yj3S+/7veX+1a/8l6bR/icEnatj5TEYh4yjiCR1ThSSYwg5qJK/ufRD8hNSjfvS4Kr8f6lj8yIDyjuSX130ZwrV3ePMmESqa8k/ZfdJyQsCaDSCVkoHnlHtZoNeu4nnIuxo+1gy3PjA+hfv/b3f/u3f3rpbRN+TScILdvsFCXD33Xfre+65xwH8+z//yo+a7sJ7TXf2qu1hyPZ4YmPrcuZVSiFKKYXaxUGqUBqFAhGVDULU9INV/dPFRLiok3mTMkXu6lahAu+SreSIihJUzjl/H6EK7pKSKIJkFBEF0vS0mum0TK8dkOysPRGdff4Xf/Wnv/eTKIU4p15IJV2SAHffLfqee5T7+bv/4+z8y9/4G15n/p8PE9gcjNIUUYjSWk/fXvBpvUU1fUlN3Vzim6mL6yDkLddvzLT0lHKrbtgN/O5OSO3qgmKClMyziwCl0ZHdt1OoTYDctrhAa1nod72GhJJunv8Pn3z7Hb/yMCR1hn6BnmVHAf4vve9PloMjN/+519/z2pWNLTtJLCC64PJpcHcRQk1/FnYBWEhCXderS5BGdgGpytYqkFQFzvR39fc5yNMfQckUoIU01Bu7iG2lIMLuM4IDRBQacd1GoBZmOjpcOfGJ1c/9wTs++MEPbl+KCFOjLi74pff9ybI5fPNfqO7i7SvrW0nirKdV5ouoGgiFVqkpgarRgrF3qaUpwzht5aY7NPXmIn2QEWNKN0/r67r2q5tzKfm53m5lAYr269Dvpim7CFBanHoTItLyPbs4N+NHK89/Tv7mo3f9+u/9h+3d6sir3aAAWYmac3Lwuj+XzsLtK+tbSeqclzmJpfbLx1uYrtxA1lAv+qcUiFTdV+xSQ1Pkz64r+FRJ9c3UdVNqfRraepN1jHKzg5TPfxFTLhdzfTX66cfVZGrXPYJSSo3j1FvZ2EoWlw59V/iG7/3vP/Pwr//j94DNRyaQudgAvAeUUkrat7z+N83s/teubWwlqbPetIdSGK8atWXqzHRXLvExe5XdPa6fuehV8ndSe5bUPpRX537/RcpBVW1VglPT9eJq919Ss1LoHskeXuthQewaIPmhNYRp6q1v7STenqve1n7Xp+6+Ryl3d42aGjKf9R6l3L/6vU//kOrt+an1re00cmIq0aw4reAiioeqEoWqM2q6czLVRvbqxOKsRaxFrMtfLVibP9NVIEiJ4jSwgOTPryRtWlzEWcSliLPgUsSm2bNc/pwaqNntObi77XZNhV7CMkwp4YI3ICP4JEnMYBxatXjoF3/2t/7bGzMiSO7GZKqH73vXe1pXfufbvqL7SzfvDIcWlFYF1HWWqHmV7Pq6frGqqYVqFKC0BhS60UC0AQGtFUr... (line truncated to 2000 chars)
img.style.cssText = 'width:24px;height:24px;border-radius:6px;';
var t = document.createElement('span');
t.textContent = 'NS Mobile';
b.appendChild(img);
b.appendChild(t);
document.body.appendChild(b);
})();
"""
    }
}
