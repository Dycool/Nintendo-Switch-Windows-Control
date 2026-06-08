package com.nscontrol

import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Bundle
import android.os.VibrationEffect
import android.os.Vibrator
import android.view.*
import android.webkit.*
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import okhttp3.*
import java.io.ByteArrayInputStream
import java.net.HttpURLConnection
import java.net.URL
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.roundToInt

class MainActivity : AppCompatActivity() {
    private lateinit var connectView: View
    private lateinit var webView: WebView
    private lateinit var hostInput: EditText
    private lateinit var statusText: TextView
    private var host = ""
    private var connected = false
    private var ws: WebSocket? = null
    private val client = OkHttpClient.Builder().build()
    private var seq = 0
    private var sending = false

    // Gyro (rad/s)
    private val sensorManager by lazy { getSystemService(SENSOR_SERVICE) as SensorManager }
    private var gyroListener: SensorEventListener? = null
    private var gyroValues = FloatArray(3) // x, y, z angular velocity

    // Haptics
    private val vibrator by lazy { getSystemService(VIBRATOR_SERVICE) as Vibrator }

    // Touch data bridged from WebView JS
    @Volatile private var touchFrame: ByteArray? = null

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
        connectWs()
        setupWebView()
        setContentView(webView)
        connected = true
        currentPage = Page.MAIN_MENU
        pageStack.clear()
        loadUrl("http://$host:8080/")
        startGyro()
        startSending()
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
                if (bytes.size() >= 8) {
                    val low = bytes.getByte(5).toInt() and 0xFF
                    val high = bytes.getByte(6).toInt() and 0xFF
                    playHaptic(low, high)
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
            while (sending && connected) {
                sendFrame()
                Thread.sleep(4)
            }
        }.start()
    }

    private fun sendFrame() {
        val buf = ByteBuffer.allocate(116).order(ByteOrder.LITTLE_ENDIAN)
        buf.putInt(0x4E535743)        // magic
        buf.put(5)                     // version
        buf.put(0x04)                  // FLAG_SINGLE_PAD
        buf.putShort(0)                // reserved
        buf.putInt(seq++)
        buf.putLong(System.nanoTime() / 1000)

        for (p in 0 until 4) {
            if (p == 0) {
                val touch = touchFrame
                if (touch != null && touch.size >= 44) {
                    buf.put(touch.sliceArray(20 until 28)) // HIDReport bytes
                    val g = gyroValues
                    buf.putShort(clampMotion(g[0] * GYRO_SCALE))
                    buf.putShort(clampMotion(g[1] * GYRO_SCALE))
                    buf.putShort(clampMotion(g[2] * GYRO_SCALE))
                    buf.putShort(0); buf.putShort(0); buf.putShort(0) // accel
                    buf.put(1) // has_motion
                    buf.put(ByteArray(3))
                } else {
                    buf.putShort(0); buf.put(8)
                    buf.put(128); buf.put(128); buf.put(128); buf.put(128); buf.put(0)
                    val g = gyroValues
                    buf.putShort(clampMotion(g[0] * GYRO_SCALE))
                    buf.putShort(clampMotion(g[1] * GYRO_SCALE))
                    buf.putShort(clampMotion(g[2] * GYRO_SCALE))
                    buf.putShort(0); buf.putShort(0); buf.putShort(0)
                    buf.put(1)
                    buf.put(ByteArray(3))
                }
            } else {
                buf.putShort(0); buf.put(8)
                buf.put(128); buf.put(128); buf.put(128); buf.put(128); buf.put(0)
                buf.put(ByteArray(16))
            }
        }
        ws?.send(ByteString.of(buf.array()))
    }

    // ═══════════════════════════════
    //  Gyro
    // ═══════════════════════════════

    private fun startGyro() {
        val sensor = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        if (sensor == null) return
        gyroListener = object : SensorEventListener {
            override fun onSensorChanged(e: SensorEvent) {
                System.arraycopy(e.values, 0, gyroValues, 0, 3)
            }
            override fun onAccuracyChanged(s: Sensor, a: Int) {}
        }
        sensorManager.registerListener(gyroListener, sensor, SensorManager.SENSOR_DELAY_FASTEST)
    }

    // ═══════════════════════════════
    //  Haptics
    // ═══════════════════════════════

    private fun playHaptic(low: Int, high: Int) {
        if (low == 0 && high == 0) return
        val amp = ((low + high) / 2).coerceIn(1, 255)
        vibrator?.vibrate(VibrationEffect.createOneShot(50, amp))
    }

    // ═══════════════════════════════
    //  WebView
    // ═══════════════════════════════

    private fun setupWebView() {
        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            allowFileAccess = false
            mixedContentMode = WebSettings.MIXED_CONTENT_ALWAYS_ALLOW
            userAgentString = settings.userAgentString + " NSControl/1.0"
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
        pageStack.add(currentPage); currentPage = page; loadUrl(url)
    }

    private fun goBack() {
        if (pageStack.isNotEmpty()) {
            currentPage = pageStack.removeLast()
            loadUrl(when (currentPage) {
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
            try {
                val arr = org.json.JSONArray(json)
                touchFrame = ByteArray(arr.length()) { i -> arr.getInt(i).toByte() }
            } catch (_: Exception) {}
        }

        @JavascriptInterface
        fun onBack() { runOnUiThread { goBack() } }
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

    private fun disconnect() {
        sending = false; ws?.close(1000, null); ws = null
        connected = false
        gyroListener?.let { sensorManager.unregisterListener(it) }
        gyroListener = null
    }

    private fun clampMotion(v: Float): Short {
        return when {
            v > 32767f -> 32767
            v < -32768f -> -32768
            else -> v.roundToInt().toShort()
        }
    }

    companion object {
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
    this.close = function() { };
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
