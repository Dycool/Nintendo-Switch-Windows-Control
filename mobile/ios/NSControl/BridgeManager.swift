import Foundation

private let kSinglePad: UInt8     = UInt8(NS_FLAG_SINGLE_PAD)
private let kFrameSize            = Int(NS_PROTOCOL_WEB_FRAME_SIZE)
private let kPadSize              = Int(NS_PROTOCOL_EXT_PAD_SIZE)
private let kMotionSize           = Int(NS_PROTOCOL_MOTION_SIZE)

enum BridgeClientMode {
    case controllers
    case touchControls
}

private func normalizeSystemShortcuts(_ buttonsIn: UInt16) -> UInt16 {
    var buttons = buttonsIn
    let captureCombo = (buttons & UInt16(NS_BTN_MINUS)) != 0 && (buttons & UInt16(NS_BTN_PLUS)) != 0
    let homeCombo = (buttons & UInt16(NS_BTN_LSTICK)) != 0 && (buttons & UInt16(NS_BTN_RSTICK)) != 0
    if captureCombo {
        buttons |= UInt16(NS_BTN_CAPTURE)
        buttons &= ~UInt16(NS_BTN_MINUS)
        buttons &= ~UInt16(NS_BTN_PLUS)
        buttons &= ~UInt16(NS_BTN_HOME)
        if homeCombo {
            buttons &= ~UInt16(NS_BTN_LSTICK)
            buttons &= ~UInt16(NS_BTN_RSTICK)
        }
    } else if homeCombo {
        buttons |= UInt16(NS_BTN_HOME)
        buttons &= ~UInt16(NS_BTN_LSTICK)
        buttons &= ~UInt16(NS_BTN_RSTICK)
        buttons &= ~UInt16(NS_BTN_CAPTURE)
    }
    return buttons
}

final class BridgeManager: NSObject, URLSessionWebSocketDelegate {
    static let shared = BridgeManager()

    var onStatus: ((String) -> Void)?
    var onRumble: ((_ pad: Int, _ low: UInt8, _ high: UInt8) -> Void)?

    private var ws: URLSessionWebSocketTask?
    private lazy var session = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: nil)
    private let sdl = SDLControllerBridge.shared
    private var seq: UInt32 = 0
    private let seqLock = NSLock()
    private var connected = false
    private var activeHost: String? = nil
    private var mode: BridgeClientMode = .controllers
    private let queue = DispatchQueue(label: "bridge", qos: .userInitiated)
    private var touchPad = Data(count: kPadSize)
    private var lastTouchPadAt = Date.distantPast

    private func nextSeq() -> UInt32 {
        seqLock.lock()
        defer { seqLock.unlock() }
        let value = seq
        seq &+= 1
        return value
    }

    private override init() {
        super.init()
        if !sdl.start() {
            NSLog("SDLControllerBridge init failed")
        }
    }

    deinit {
        sdl.stop()
    }

    // MARK: - Connection

    func connect(host: String, port: UInt16 = 8080, mode newMode: BridgeClientMode = .controllers) {
        guard let url = normalizeWebSocketURL(host, defaultPort: port) else {
            DispatchQueue.main.async { self.onStatus?("Invalid server address") }
            return
        }
        let key = url.absoluteString
        if activeHost == key && ws != nil { return }
        disconnect()
        activeHost = key
        mode = newMode
        var req = URLRequest(url: url)
        req.timeoutInterval = 10
        ws = session.webSocketTask(with: req)
        ws?.resume()
        readRumble()
        DispatchQueue.main.async { self.onStatus?("Connecting...") }
    }

    private func normalizeWebSocketURL(_ raw: String, defaultPort: UInt16 = 8080) -> URL? {
        var text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return nil }

        let hadScheme = text.range(of: "://") != nil
        if !hadScheme { text = "ws://\(text)" }
        guard var components = URLComponents(string: text) else { return nil }

        let inputScheme = components.scheme?.lowercased() ?? "ws"
        switch inputScheme {
        case "https", "wss": components.scheme = "wss"
        case "http", "ws": components.scheme = "ws"
        default: components.scheme = "ws"
        }

        guard components.host?.isEmpty == false else { return nil }
        if components.path.isEmpty { components.path = "/" }
        if components.port == nil && (!hadScheme || components.scheme == "ws") {
            components.port = Int(defaultPort)
        }
        return components.url
    }

    func disconnect() {
        let hadClient = ws != nil || connected || activeHost != nil
        if ws != nil {
            for _ in 0..<3 { sendResetFrame() }
        }
        ws?.cancel(with: .normalClosure, reason: nil)
        ws = nil
        activeHost = nil
        connected = false
        mode = .controllers
        objc_sync_enter(self)
        touchPad = Data(count: kPadSize)
        lastTouchPadAt = Date.distantPast
        objc_sync_exit(self)

        // Stop all rumble
        for i in 0..<4 { sdl.padStopRumble(i) }
        sdl.phoneHapticRumble(low: 0, high: 0)
        sdl.phoneHapticClose()
        sdl.phoneSensorsClose()

        if hadClient { DispatchQueue.main.async { self.onStatus?("Disconnected") } }
    }

    func urlSession(_ session: URLSession,
                    webSocketTask: URLSessionWebSocketTask,
                    didOpenWithProtocol protocol: String?) {
        connected = true
        DispatchQueue.main.async { self.onStatus?("Connected") }
        // Open phone sensors + haptics once per session
        if sdl.phoneSensorsOpen() { sdl.phoneHapticOpen() }
        startSendLoop()
    }

    func urlSession(_ session: URLSession,
                    webSocketTask: URLSessionWebSocketTask,
                    didCloseWith closeCode: URLSessionWebSocketTask.CloseCode,
                    reason: Data?) {
        connected = false
        activeHost = nil
        DispatchQueue.main.async { self.onStatus?("Disconnected") }
    }

    /// Kept as fallback for older bundled pages.
    func bridgeTouchPad(_ data: Data) {
        guard connected && mode == .touchControls else { return }
        objc_sync_enter(self)
        if data.count >= kPadSize {
            touchPad = Data(data.prefix(kPadSize))
            lastTouchPadAt = Date()
        }
        objc_sync_exit(self)
    }

    func bridgeTouchState(buttons: UInt16, hat: UInt8, lx: UInt8, ly: UInt8, rx: UInt8, ry: UInt8) {
        guard connected && mode == .touchControls else { return }
        var hid = Data(count: Int(NS_PROTOCOL_HID_SIZE))
        hid.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_hid_write(base, normalizeSystemShortcuts(buttons), hat, lx, ly, rx, ry, 1)
        }
        var pad = neutralPad()
        pad.withUnsafeMutableBytes { padRaw in
            hid.withUnsafeBytes { hidRaw in
                guard let padBase = padRaw.bindMemory(to: UInt8.self).baseAddress,
                      let hidBase = hidRaw.bindMemory(to: UInt8.self).baseAddress else { return }
                ns_pad_set_hid(padBase, hidBase)
            }
        }
        objc_sync_enter(self)
        touchPad = pad
        lastTouchPadAt = Date()
        objc_sync_exit(self)
    }

    // MARK: - Send loop

    private func startSendLoop() {
        queue.async { [weak self] in
            guard let self = self else { return }
            while self.connected {
                self.sendFrame()
                Thread.sleep(forTimeInterval: 0.004)
            }
        }
    }

    private func sendResetFrame() {
        var frame = Data(count: kFrameSize)
        let timestampUs = UInt64(Date().timeIntervalSince1970 * 1_000_000)
        let frameSeq = nextSeq()
        frame.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_web_frame_init(base, UInt8(NS_FLAG_RESET), frameSeq, timestampUs)
        }
        ws?.send(.data(frame)) { _ in }
    }

    private func sendFrame() {
        sdl.poll()

        var frame = Data(count: kFrameSize)
        let touchActive = mode == .touchControls && touchModeActive()
        let anyController = mode == .controllers && (0..<4).contains { sdl.padConnected($0) }
        let flags: UInt8 = touchActive ? kSinglePad : UInt8(0)
        let timestampUs = UInt64(Date().timeIntervalSince1970 * 1_000_000)
        let frameSeq = nextSeq()

        frame.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_web_frame_init(base, flags, frameSeq, timestampUs)
        }

        for p in 0..<4 {
            var pad = self.neutralPad()
            if mode == .controllers && sdl.padConnected(p) {
                self.mergeSDLInput(p, into: &pad)
                if !self.mergeSDLMotion(p, into: &pad) {
                    self.mergePhoneMotion(into: &pad)
                }
            } else if mode == .touchControls && p == 0 && touchActive {
                objc_sync_enter(self)
                pad = self.touchPad
                objc_sync_exit(self)
                self.mergePhoneMotion(into: &pad)
            }
            frame.withUnsafeMutableBytes { frameRaw in
                pad.withUnsafeBytes { padRaw in
                    guard let frameBase = frameRaw.bindMemory(to: UInt8.self).baseAddress,
                          let padBase = padRaw.bindMemory(to: UInt8.self).baseAddress else { return }
                    ns_web_frame_set_pad(frameBase, Int32(p), padBase)
                }
            }
        }
        ws?.send(.data(frame)) { _ in }
    }

    private func touchModeActive() -> Bool {
        connected && Date().timeIntervalSince(lastTouchPadAt) < 2.0
    }

    // MARK: - Pad helpers

    private func neutralPad() -> Data {
        var pad = Data(count: kPadSize)
        pad.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_pad_write_neutral(base)
        }
        return pad
    }

    private func mergeMotionBytes(_ motion: Data, into pad: inout Data) {
        pad.withUnsafeMutableBytes { padRaw in
            motion.withUnsafeBytes { motionRaw in
                guard let padBase = padRaw.bindMemory(to: UInt8.self).baseAddress,
                      let motionBase = motionRaw.bindMemory(to: UInt8.self).baseAddress else { return }
                ns_pad_set_motion(padBase, motionBase)
            }
        }
    }

    private func mergeSDLInput(_ slot: Int, into pad: inout Data) {
        let input = sdl.padInput(slot)
        guard input.connected else { return }
        var hid = Data(count: Int(NS_PROTOCOL_HID_SIZE))
        hid.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_hid_write_controller(base,
                                    normalizeSystemShortcuts(input.buttons),
                                    input.dpad_up, input.dpad_down,
                                    input.dpad_left, input.dpad_right,
                                    input.lx, input.ly,
                                    input.rx, input.ry,
                                    1)
        }
        pad.withUnsafeMutableBytes { padRaw in
            hid.withUnsafeBytes { hidRaw in
                guard let padBase = padRaw.bindMemory(to: UInt8.self).baseAddress,
                      let hidBase = hidRaw.bindMemory(to: UInt8.self).baseAddress else { return }
                ns_pad_set_hid(padBase, hidBase)
            }
        }
    }

    private func mergeSDLMotion(_ slot: Int, into pad: inout Data) -> Bool {
        let motion = sdl.padMotion(slot)
        guard motion.has_motion else { return false }
        var motionBytes = Data(count: kMotionSize)
        motionBytes.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_motion_write_values(base,
                                   motion.ax, motion.ay, motion.az,
                                   motion.gx, motion.gy, motion.gz,
                                   1)
        }
        mergeMotionBytes(motionBytes, into: &pad)
        return true
    }

    private func mergePhoneMotion(into pad: inout Data) {
        let motion = sdl.phoneSensorsRead()
        guard motion.has_motion else { return }
        var motionBytes = Data(count: kMotionSize)
        motionBytes.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_motion_write_values(base,
                                   motion.ax, motion.ay, motion.az,
                                   motion.gx, motion.gy, motion.gz,
                                   1)
        }
        mergeMotionBytes(motionBytes, into: &pad)
    }

    // MARK: - Rumble

    private func readRumble() {
        ws?.receive { [weak self] result in
            guard let self = self else { return }
            switch result {
            case .success(let msg):
                if case .data(let d) = msg, d.count >= 8 {
                    let subpad = Int(d[d.startIndex + 4])
                    let low = d[d.startIndex + 5]
                    let high = d[d.startIndex + 6]
                    DispatchQueue.main.async {
                        self.onRumble?(subpad, low, high)
                        self.routeRumble(subpad: subpad, low: low, high: high)
                    }
                }
                self.readRumble()
            case .failure:
                self.disconnect()
            }
        }
    }

    private func routeRumble(subpad: Int, low: UInt8, high: UInt8) {
        guard connected else { return }

        if low == 0 && high == 0 {
            if mode == .controllers, subpad >= 0 && subpad < 4, sdl.padConnected(subpad) {
                sdl.padStopRumble(subpad)
            }
            if mode == .touchControls && subpad == 0 { sdl.phoneHapticRumble(low: 0, high: 0) }
            return
        }

        if mode == .controllers, subpad >= 0 && subpad < 4, sdl.padConnected(subpad) {
            sdl.padRumble(subpad, low: low, high: high)
            return
        }

        // Touch Controls is touch-only: server rumble becomes phone haptics,
        // never SDL controller rumble.
        if mode == .touchControls && touchModeActive() { sdl.phoneHapticRumble(low: low, high: high) }
    }
}
