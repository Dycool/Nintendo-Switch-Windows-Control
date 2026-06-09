import Foundation
import CoreMotion
import CoreHaptics
import GameController

// Thin Swift aliases for the shared C protocol library.
private let kSinglePad: UInt8     = UInt8(NS_FLAG_SINGLE_PAD)
private let kExtPresent: UInt8    = UInt8(NS_EXT_PAD_PRESENT)
private let kFrameSize            = Int(NS_PROTOCOL_WEB_FRAME_SIZE)
private let kPadSize              = Int(NS_PROTOCOL_EXT_PAD_SIZE)
private let kMotionSize           = Int(NS_PROTOCOL_MOTION_SIZE)

private let kBtnY: UInt16      = UInt16(NS_BTN_Y);      let kBtnB: UInt16      = UInt16(NS_BTN_B)
private let kBtnA: UInt16      = UInt16(NS_BTN_A);      let kBtnX: UInt16      = UInt16(NS_BTN_X)
private let kBtnL: UInt16      = UInt16(NS_BTN_L);      let kBtnR: UInt16      = UInt16(NS_BTN_R)
private let kBtnZL: UInt16     = UInt16(NS_BTN_ZL);     let kBtnZR: UInt16     = UInt16(NS_BTN_ZR)
private let kBtnMinus: UInt16  = UInt16(NS_BTN_MINUS);  let kBtnPlus: UInt16   = UInt16(NS_BTN_PLUS)
private let kBtnLStick: UInt16 = UInt16(NS_BTN_LSTICK); let kBtnRStick: UInt16 = UInt16(NS_BTN_RSTICK)
private let kBtnHome: UInt16   = UInt16(NS_BTN_HOME);   let kBtnCapture: UInt16 = UInt16(NS_BTN_CAPTURE)

private let kHatN: UInt8 = UInt8(NS_HAT_N);   let kHatNE: UInt8 = UInt8(NS_HAT_NE)
private let kHatE: UInt8 = UInt8(NS_HAT_E);   let kHatSE: UInt8 = UInt8(NS_HAT_SE)
private let kHatS: UInt8 = UInt8(NS_HAT_S);   let kHatSW: UInt8 = UInt8(NS_HAT_SW)
private let kHatW: UInt8 = UInt8(NS_HAT_W);   let kHatNW: UInt8 = UInt8(NS_HAT_NW)
private let kHatNeutral: UInt8 = UInt8(NS_HAT_NEUTRAL)

final class BridgeManager: NSObject, URLSessionWebSocketDelegate {
    static let shared = BridgeManager()

    var onStatus: ((String) -> Void)?
    var onRumble: ((_ pad: Int, _ low: UInt8, _ high: UInt8) -> Void)?

    private var ws: URLSessionWebSocketTask?
    private lazy var session = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: nil)
    private let motion = CMMotionManager()
    private var engine: CHHapticEngine?
    private var engineNeedsStart = true
    private var controllerHapticEngines: [ObjectIdentifier: CHHapticEngine] = [:]
    private var seq: UInt32 = 0
    private var connected = false
    private var activeHost: String? = nil
    private let queue = DispatchQueue(label: "bridge", qos: .userInitiated)
    // Pad 0 touch data bridged from WebView JS (24 raw bytes)
    private var touchPad = Data(count: kPadSize)
    private var lastTouchPadAt = Date.distantPast

    private override init() {
        super.init()
        startHaptics()
        observeControllers()
        scanExistingControllers()
    }

    // MARK: - Connection

    func connect(host: String, port: UInt16 = 8080) {
        if activeHost == host && ws != nil { return }
        disconnect()
        guard let url = URL(string: "ws://\(host):\(port)/") else { return }
        activeHost = host
        var req = URLRequest(url: url)
        req.timeoutInterval = 10
        ws = session.webSocketTask(with: req)
        ws?.resume()
        readRumble()
        startGyro()
        DispatchQueue.main.async { self.onStatus?("Connecting...") }
    }

    func disconnect() {
        let hadClient = ws != nil || connected || activeHost != nil
        if ws != nil {
            // Release all virtual pads before closing the control WebSocket.
            for _ in 0..<3 { sendResetFrame() }
        }
        ws?.cancel(with: .normalClosure, reason: nil)
        ws = nil
        activeHost = nil
        connected = false
        objc_sync_enter(self)
        touchPad = Data(count: kPadSize)
        lastTouchPadAt = Date.distantPast
        objc_sync_exit(self)
        stopGyro()
        engine?.stop(completionHandler: nil)
        engineNeedsStart = true
        for e in controllerHapticEngines.values { e.stop(completionHandler: nil) }
        if hadClient { DispatchQueue.main.async { self.onStatus?("Disconnected") } }
    }

    func urlSession(_ session: URLSession,
                    webSocketTask: URLSessionWebSocketTask,
                    didOpenWithProtocol protocol: String?) {
        connected = true
        DispatchQueue.main.async { self.onStatus?("Connected") }
        startSendLoop()
    }

    func urlSession(_ session: URLSession,
                    webSocketTask: URLSessionWebSocketTask,
                    didCloseWith closeCode: URLSessionWebSocketTask.CloseCode,
                    reason: Data?) {
        connected = false
        activeHost = nil
        stopGyro()
        DispatchQueue.main.async { self.onStatus?("Disconnected") }
    }

    /// Called from injected JS bridge with 24 raw bytes for pad 0
    func bridgeTouchPad(_ data: Data) {
        guard connected else { return }
        objc_sync_enter(self)
        if data.count >= kPadSize {
            touchPad = Data(data.prefix(kPadSize))
            lastTouchPadAt = Date()
        }
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
        frame.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_web_frame_init(base, UInt8(NS_FLAG_RESET), seq, timestampUs)
        }
        seq &+= 1
        ws?.send(.data(frame)) { _ in }
    }

    private func sendFrame() {
        var frame = Data(count: kFrameSize)
        let hasController = controllers.contains { $0 != nil }
        let touchActive = touchModeActive()
        let flags: UInt8 = (!hasController && touchActive) ? kSinglePad : UInt8(0)
        let timestampUs = UInt64(Date().timeIntervalSince1970 * 1_000_000)

        frame.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_web_frame_init(base, flags, seq, timestampUs)
        }

        for p in 0..<4 {
            var pad = self.neutralPad()
            if p < self.controllers.count, let gc = self.controllers[p] {
                self.mergeGCController(gc, into: &pad)
                _ = self.mergeGCControllerMotion(gc, into: &pad)
            } else if p == 0 && touchActive {
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
        seq &+= 1
        ws?.send(.data(frame)) { _ in }
    }

    private func touchModeActive() -> Bool {
        connected && Date().timeIntervalSince(lastTouchPadAt) < 0.75
    }

    // MARK: - Gyro

    private var currentGyro: CMDeviceMotion?

    private func startGyro() {
        guard motion.isDeviceMotionAvailable else { return }
        motion.deviceMotionUpdateInterval = 1.0 / 250.0
        motion.startDeviceMotionUpdates(to: .main) { [weak self] m, _ in
            self?.currentGyro = m
        }
    }

    private func stopGyro() {
        motion.stopDeviceMotionUpdates()
        currentGyro = nil
    }

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

    private func mergePhoneMotion(into pad: inout Data) {
        guard let g = currentGyro else { return }
        var motionBytes = Data(count: kMotionSize)
        motionBytes.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_motion_from_apple(base,
                                 Float(g.gravity.x), Float(g.gravity.y), Float(g.gravity.z),
                                 Float(g.rotationRate.x), Float(g.rotationRate.y), Float(g.rotationRate.z))
        }
        mergeMotionBytes(motionBytes, into: &pad)
    }

    private func mergeGCControllerMotion(_ gc: GCController, into pad: inout Data) -> Bool {
        guard let m = gc.motion else { return false }
        if m.sensorsRequireManualActivation && !m.sensorsActive { m.sensorsActive = true }
        var motionBytes = Data(count: kMotionSize)
        motionBytes.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_motion_from_apple(base,
                                 Float(m.gravity.x), Float(m.gravity.y), Float(m.gravity.z),
                                 Float(m.rotationRate.x), Float(m.rotationRate.y), Float(m.rotationRate.z))
        }
        mergeMotionBytes(motionBytes, into: &pad)
        return true
    }

    // MARK: - Rumble

    private func readRumble() {
        ws?.receive { [weak self] result in
            guard let self = self else { return }
            switch result {
            case .success(let msg):
                if case .data(let d) = msg, d.count >= 8 {
                    let subpad = d[d.startIndex + 4]
                    let low = d[d.startIndex + 5]
                    let high = d[d.startIndex + 6]
                    DispatchQueue.main.async {
                        self.onRumble?(Int(subpad), low, high)
                        self.routeRumble(subpad: Int(subpad), low: low, high: high)
                    }
                }
                self.readRumble()
            case .failure:
                self.disconnect()
            }
        }
    }

    // MARK: - Haptics

    private func startHaptics() {
        guard CHHapticEngine.capabilitiesForHardware().supportsHaptics else { return }
        do {
            engine = try CHHapticEngine()
            engine?.stoppedHandler = { [weak self] _ in self?.engineNeedsStart = true }
            engine?.resetHandler = { [weak self] in
                self?.engineNeedsStart = true; try? self?.engine?.start()
            }
            try engine?.start()
            engineNeedsStart = false
        } catch { engine = nil }
    }

    private func routeRumble(subpad: Int, low: UInt8, high: UInt8) {
        guard connected else { return }
        if subpad >= 0 && subpad < controllers.count, let gc = controllers[subpad] {
            _ = playControllerHaptic(gc, low: low, high: high)
            return
        }

        // If any physical controller is active, never fake its rumble with phone
        // haptics. And if we are in the main menu/editor, stay silent too.
        if controllers.contains(where: { $0 != nil }) { return }
        if touchModeActive() { playPhoneHaptic(low: low, high: high) }
    }

    private func playPhoneHaptic(low: UInt8, high: UInt8) {
        guard low != 0 || high != 0 else { return }
        guard let engine = engine else { return }
        if engineNeedsStart { try? engine.start(); engineNeedsStart = false }
        do {
            let intensity = max(Float(high), Float(low)) / 255.0
            let sharpness = Float(low) / 255.0
            let ev = CHHapticEvent(eventType: .hapticContinuous,
                                   parameters: [
                                    CHHapticEventParameter(parameterID: .hapticIntensity, value: intensity),
                                    CHHapticEventParameter(parameterID: .hapticSharpness, value: sharpness)],
                                   relativeTime: 0, duration: 0.1)
            let pattern = try CHHapticPattern(events: [ev], parameters: [])
            let player = try engine.makePlayer(with: pattern)
            try player.start(atTime: 0)
        } catch {}
    }

    private func playControllerHaptic(_ gc: GCController, low: UInt8, high: UInt8) -> Bool {
        let id = ObjectIdentifier(gc)
        if low == 0 && high == 0 {
            controllerHapticEngines[id]?.stop(completionHandler: nil)
            return true
        }
        guard let haptics = gc.haptics else { return false }
        let engine: CHHapticEngine
        if let existing = controllerHapticEngines[id] {
            engine = existing
        } else if let created = haptics.createEngine(withLocality: GCHapticsLocality.default) {
            engine = created
            controllerHapticEngines[id] = created
        } else {
            return false
        }
        do {
            try engine.start()
            let intensity = max(Float(high), Float(low)) / 255.0
            let sharpness = Float(low) / 255.0
            let ev = CHHapticEvent(eventType: .hapticContinuous,
                                   parameters: [
                                    CHHapticEventParameter(parameterID: .hapticIntensity, value: intensity),
                                    CHHapticEventParameter(parameterID: .hapticSharpness, value: sharpness)],
                                   relativeTime: 0, duration: 0.1)
            let pattern = try CHHapticPattern(events: [ev], parameters: [])
            let player = try engine.makePlayer(with: pattern)
            try player.start(atTime: 0)
            return true
        } catch {
            return false
        }
    }

    // MARK: - Physical controllers (GCController)

    private var controllers: [GCController?] = [nil, nil, nil, nil]

    private func observeControllers() {
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            guard let self = self, let gc = n.object as? GCController else { return }
            self.addController(gc)
        }
        NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] n in
            guard let self = self, let gc = n.object as? GCController else { return }
            self.removeController(gc)
        }
        GCController.startWirelessControllerDiscovery {}
    }

    private func scanExistingControllers() {
        for gc in GCController.controllers() { addController(gc) }
    }

    private func addController(_ gc: GCController) {
        configureController(gc)
        for i in 0..<4 where controllers[i] == nil {
            controllers[i] = gc
            DispatchQueue.main.async { self.onStatus?("Controller connected") }
            return
        }
    }

    private func removeController(_ gc: GCController) {
        controllerHapticEngines[ObjectIdentifier(gc)]?.stop(completionHandler: nil)
        controllerHapticEngines.removeValue(forKey: ObjectIdentifier(gc))
        controllers = controllers.map { $0 === gc ? nil : $0 }
        DispatchQueue.main.async { self.onStatus?("Controller disconnected") }
    }

    private func configureController(_ gc: GCController) {
        if let m = gc.motion {
            m.sensorsActive = true
        }
    }

    /// Merges GCController state into the 24-byte pad buffer (little-endian)
    private func mergeGCController(_ gc: GCController, into pad: inout Data) {
        guard let gp = gc.extendedGamepad else { return }
        var buttons: UInt16 = 0
        if gp.buttonA.isPressed { buttons |= kBtnA }
        if gp.buttonB.isPressed { buttons |= kBtnB }
        if gp.buttonX.isPressed { buttons |= kBtnX }
        if gp.buttonY.isPressed { buttons |= kBtnY }
        if gp.leftShoulder.isPressed { buttons |= kBtnL }
        if gp.rightShoulder.isPressed { buttons |= kBtnR }
        if gp.leftTrigger.isPressed { buttons |= kBtnZL }
        if gp.rightTrigger.isPressed { buttons |= kBtnZR }
        if gp.leftThumbstickButton?.isPressed == true { buttons |= kBtnLStick }
        if gp.rightThumbstickButton?.isPressed == true { buttons |= kBtnRStick }
        if gp.buttonMenu.isPressed { buttons |= kBtnPlus }
        if gp.buttonOptions?.isPressed == true { buttons |= kBtnMinus }
        if gp.buttonHome?.isPressed == true { buttons |= kBtnHome }
        pad[0..<2] = withUnsafeBytes(of: buttons.littleEndian) { Data($0) }
        // D-pad → hat at offset 2, including diagonals.
        let up = gp.dpad.up.isPressed
        let down = gp.dpad.down.isPressed
        let left = gp.dpad.left.isPressed
        let right = gp.dpad.right.isPressed
        if up && right { pad[2] = kHatNE }
        else if up && left { pad[2] = kHatNW }
        else if down && right { pad[2] = kHatSE }
        else if down && left { pad[2] = kHatSW }
        else if up { pad[2] = kHatN }
        else if right { pad[2] = kHatE }
        else if down { pad[2] = kHatS }
        else if left { pad[2] = kHatW }
        else { pad[2] = kHatNeutral }

        // Protocol uses 0 = up/left and 255 = down/right. GameController Y is positive-up,
        // so invert Y to match the desktop/SDL client semantics.
        pad[3] = UInt8(clamping: Int(( gp.leftThumbstick.xAxis.value + 1) * 127.5))
        pad[4] = UInt8(clamping: Int((-gp.leftThumbstick.yAxis.value + 1) * 127.5))
        pad[5] = UInt8(clamping: Int(( gp.rightThumbstick.xAxis.value + 1) * 127.5))
        pad[6] = UInt8(clamping: Int((-gp.rightThumbstick.yAxis.value + 1) * 127.5))
        pad[7] |= kExtPresent
    }
}
