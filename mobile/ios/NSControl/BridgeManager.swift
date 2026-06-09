import Foundation
import CoreMotion
import UIKit
import GameController
import CoreHaptics

private let kSinglePad: UInt8 = UInt8(NS_FLAG_SINGLE_PAD)
private let kFrameSize = Int(NS_PROTOCOL_WEB_FRAME_SIZE)
private let kPadSize = Int(NS_PROTOCOL_EXT_PAD_SIZE)
private let kMotionSize = Int(NS_PROTOCOL_MOTION_SIZE)
private let kMotionSampleCount = Int(NS_PROTOCOL_MOTION_SAMPLE_COUNT)

enum BridgeClientMode {
    case touchControls
    case controllerHub
}

final class BridgeManager: NSObject, URLSessionWebSocketDelegate {
    static let shared = BridgeManager()

    var onStatus: ((String) -> Void)?
    var onRumble: ((_ pad: Int, _ low: UInt8, _ high: UInt8) -> Void)?

    private var ws: URLSessionWebSocketTask?
    private lazy var session = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: nil)
    private var seq: UInt32 = 0
    private let seqLock = NSLock()
    private var connected = false
    private var currentMode: BridgeClientMode = .touchControls
    private var activeHost: String? = nil
    private var sessionToken: UInt64 = 0
    private let queue = DispatchQueue(label: "bridge", qos: .userInitiated)

    private var touchPad = Data(count: kPadSize)
    private var lastTouchPadAt = Date.distantPast

    private let motionManager = CMMotionManager()
    private let phoneMotionLock = NSLock()
    private var nativePhoneMotionSamples: [Data] = []
    private var controllerMotionSamples: [[Data]] = Array(repeating: [], count: Int(NS_PROTOCOL_PAD_COUNT))

    private struct ControllerRumbleState {
        var low: UInt8 = 0
        var high: UInt8 = 0
        var until: TimeInterval = 0
        var lastSet: TimeInterval = 0
        var player: CHHapticPatternPlayer? = nil
    }
    private var controllerRumbleStates = Array(repeating: ControllerRumbleState(), count: Int(NS_PROTOCOL_PAD_COUNT))
    private var controllerHapticEngines: [ObjectIdentifier: CHHapticEngine] = [:]

    private func nextSeq() -> UInt32 {
        seqLock.lock()
        defer { seqLock.unlock() }
        let value = seq
        seq &+= 1
        return value
    }

    private override init() {
        super.init()
    }

    // MARK: - Connection

    func connect(host: String, port: UInt16 = 8080, mode: BridgeClientMode = .touchControls) {
        guard let url = normalizeWebSocketURL(host, defaultPort: port) else {
            DispatchQueue.main.async { self.onStatus?("Invalid server address") }
            return
        }
        let key = url.absoluteString
        if activeHost == key && ws != nil { return }
        disconnect()
        currentMode = mode
        activeHost = key
        sessionToken &+= 1
        var req = URLRequest(url: url)
        req.timeoutInterval = 10
        ws = session.webSocketTask(with: req)
        ws?.resume()
        let token = sessionToken
        readRumble(token: token)
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
        sessionToken &+= 1
        if ws != nil {
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
        stopNativePhoneMotion()
        stopControllerHub()
        if hadClient { DispatchQueue.main.async { self.onStatus?("Disconnected") } }
    }

    func urlSession(_ session: URLSession,
                    webSocketTask: URLSessionWebSocketTask,
                    didOpenWithProtocol protocol: String?) {
        connected = true
        DispatchQueue.main.async { self.onStatus?("Connected") }
        if currentMode == .touchControls {
            startNativePhoneMotion()
        } else {
            startControllerHub()
        }
        let token = sessionToken
        startSendLoop(token: token)
    }

    func urlSession(_ session: URLSession,
                    webSocketTask: URLSessionWebSocketTask,
                    didCloseWith closeCode: URLSessionWebSocketTask.CloseCode,
                    reason: Data?) {
        connected = false
        activeHost = nil
        stopNativePhoneMotion()
        stopControllerHub()
        DispatchQueue.main.async { self.onStatus?("Disconnected") }
    }

    // MARK: - Touch input from WebView

    /// Kept as fallback for older bundled pages.
    func bridgeTouchPad(_ data: Data) {
        guard connected else { return }
        objc_sync_enter(self)
        if data.count >= kPadSize {
            touchPad = Data(data.prefix(kPadSize))
            lastTouchPadAt = Date()
        }
        objc_sync_exit(self)
    }

    func bridgeTouchState(buttons: UInt16, hat: UInt8, lx: UInt8, ly: UInt8, rx: UInt8, ry: UInt8) {
        guard connected else { return }
        var hid = Data(count: Int(NS_PROTOCOL_HID_SIZE))
        hid.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_hid_write(base, ns_normalize_system_shortcuts(buttons), hat, lx, ly, rx, ry, 1)
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

    private func startSendLoop(token: UInt64) {
        queue.async { [weak self] in
            guard let self = self else { return }
            while self.connected && self.sessionToken == token {
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
        if currentMode == .controllerHub {
            sendControllerHubFrame()
            return
        }
        var frame = Data(count: kFrameSize)
        let timestampUs = UInt64(Date().timeIntervalSince1970 * 1_000_000)
        let frameSeq = nextSeq()

        frame.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_web_frame_init(base, kSinglePad, frameSeq, timestampUs)
        }

        var pad: Data
        objc_sync_enter(self)
        let isFresh = Date().timeIntervalSince(lastTouchPadAt) <= 0.5
        pad = isFresh ? touchPad : neutralPad()
        objc_sync_exit(self)

        mergePhoneMotion(into: &pad)

        frame.withUnsafeMutableBytes { frameRaw in
            pad.withUnsafeBytes { padRaw in
                guard let frameBase = frameRaw.bindMemory(to: UInt8.self).baseAddress,
                      let padBase = padRaw.bindMemory(to: UInt8.self).baseAddress else { return }
                ns_web_frame_set_pad(frameBase, 0, padBase)
            }
        }
        ws?.send(.data(frame)) { _ in }
    }

    private func neutralPad() -> Data {
        var pad = Data(count: kPadSize)
        pad.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_pad_write_neutral(base)
        }
        return pad
    }

    private func mergeMotionSamples(_ samples: [Data], into pad: inout Data) {
        guard samples.count >= kMotionSampleCount else { return }
        pad.withUnsafeMutableBytes { padRaw in
            samples[0].withUnsafeBytes { m0 in
                samples[1].withUnsafeBytes { m1 in
                    samples[2].withUnsafeBytes { m2 in
                        guard let padBase = padRaw.bindMemory(to: UInt8.self).baseAddress,
                              let b0 = m0.bindMemory(to: UInt8.self).baseAddress,
                              let b1 = m1.bindMemory(to: UInt8.self).baseAddress,
                              let b2 = m2.bindMemory(to: UInt8.self).baseAddress else { return }
                        ns_pad_set_motion_samples(padBase, b0, b1, b2)
                    }
                }
            }
        }
    }

    private func mergePhoneMotion(into pad: inout Data) {
        if let samples = latestNativePhoneMotionSamples() {
            mergeMotionSamples(samples, into: &pad)
        }
    }

    // MARK: - CoreMotion

    private func startNativePhoneMotion() {
        guard motionManager.isDeviceMotionAvailable else { return }
        motionManager.deviceMotionUpdateInterval = 1.0 / 250.0
        motionManager.startDeviceMotionUpdates(to: OperationQueue()) { [weak self] motion, _ in
            guard let self = self, let motion = motion else { return }
            var bytes = Data(count: kMotionSize)
            bytes.withUnsafeMutableBytes { raw in
                guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
                ns_motion_from_apple(base,
                    Float(motion.gravity.x), -Float(motion.gravity.y), Float(motion.gravity.z),
                    Float(motion.rotationRate.x), -Float(motion.rotationRate.y), Float(motion.rotationRate.z))
            }
            self.phoneMotionLock.lock()
            self.nativePhoneMotionSamples.append(bytes)
            if self.nativePhoneMotionSamples.count > kMotionSampleCount {
                self.nativePhoneMotionSamples.removeFirst(self.nativePhoneMotionSamples.count - kMotionSampleCount)
            }
            self.phoneMotionLock.unlock()
        }
    }

    private func stopNativePhoneMotion() {
        if motionManager.isDeviceMotionActive { motionManager.stopDeviceMotionUpdates() }
        phoneMotionLock.lock()
        nativePhoneMotionSamples.removeAll()
        phoneMotionLock.unlock()
    }

    private func latestNativePhoneMotionSamples() -> [Data]? {
        phoneMotionLock.lock()
        let out = nativePhoneMotionSamples.count >= kMotionSampleCount ? nativePhoneMotionSamples : nil
        phoneMotionLock.unlock()
        return out
    }

    // MARK: - Rumble / haptics

    private func readRumble(token: UInt64) {
        ws?.receive { [weak self] result in
            guard let self = self, self.sessionToken == token else { return }
            switch result {
            case .success(let msg):
                if case .data(let d) = msg, d.count == 8 {
                    let magic = UInt32(d[d.startIndex]) |
                        (UInt32(d[d.startIndex + 1]) << 8) |
                        (UInt32(d[d.startIndex + 2]) << 16) |
                        (UInt32(d[d.startIndex + 3]) << 24)
                    if magic == 0x4E535652 { // 'NSVR' little-endian payload
                        let subpad = Int(d[d.startIndex + 4])
                        let low = d[d.startIndex + 5]
                        let high = d[d.startIndex + 6]
                        let duration = d[d.startIndex + 7]
                        DispatchQueue.main.async {
                            self.onRumble?(subpad, low, high)
                            self.controllerRumble(slot: subpad, low: low, high: high, duration10Ms: duration)
                        }
                    }
                }
                self.readRumble(token: token)
            case .failure:
                if self.sessionToken == token { self.disconnect() }
            }
        }
    }

    // MARK: - Controller Hub

    private func startControllerHub() {
        controllerMotionSamples = Array(repeating: [], count: Int(NS_PROTOCOL_PAD_COUNT))
        controllerRumbleStates = Array(repeating: ControllerRumbleState(), count: Int(NS_PROTOCOL_PAD_COUNT))
        controllerHapticEngines.removeAll()
        for c in GCController.controllers() {
            if let motion = c.motion, motion.sensorsRequireManualActivation {
                motion.sensorsActive = true
            }
        }
        GCController.startWirelessControllerDiscovery { 
            for c in GCController.controllers() {
                if let motion = c.motion, motion.sensorsRequireManualActivation {
                    motion.sensorsActive = true
                }
            }
        }
    }

    private func stopControllerHub() {
        GCController.stopWirelessControllerDiscovery()
        for i in controllerRumbleStates.indices {
            try? controllerRumbleStates[i].player?.stop(atTime: 0)
        }
        for engine in controllerHapticEngines.values { engine.stop(completionHandler: nil) }
        controllerHapticEngines.removeAll()
        controllerRumbleStates = Array(repeating: ControllerRumbleState(), count: Int(NS_PROTOCOL_PAD_COUNT))
        controllerMotionSamples = Array(repeating: [], count: Int(NS_PROTOCOL_PAD_COUNT))
    }

    private func sendControllerHubFrame() {
        var frame = Data(count: kFrameSize)
        let timestampUs = UInt64(Date().timeIntervalSince1970 * 1_000_000)
        let frameSeq = nextSeq()
        frame.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_web_frame_init(base, 0, frameSeq, timestampUs)
        }

        let controllers = Array(GCController.controllers().prefix(Int(NS_PROTOCOL_PAD_COUNT)))
        for (slot, controller) in controllers.enumerated() {
            if let motion = controller.motion, motion.sensorsRequireManualActivation {
                motion.sensorsActive = true
            }
            var pad = padForController(controller, slot: slot)
            frame.withUnsafeMutableBytes { frameRaw in
                pad.withUnsafeBytes { padRaw in
                    guard let frameBase = frameRaw.bindMemory(to: UInt8.self).baseAddress,
                          let padBase = padRaw.bindMemory(to: UInt8.self).baseAddress else { return }
                    ns_web_frame_set_pad(frameBase, Int32(slot), padBase)
                }
            }
        }
        ws?.send(.data(frame)) { _ in }
    }

    private func padForController(_ controller: GCController, slot: Int) -> Data {
        var pad = neutralPad()
        guard let gp = controller.extendedGamepad else { return pad }
        var buttons: UInt16 = 0
        if gp.buttonX.isPressed { buttons |= UInt16(NS_BTN_Y) }
        if gp.buttonA.isPressed { buttons |= UInt16(NS_BTN_B) }
        if gp.buttonB.isPressed { buttons |= UInt16(NS_BTN_A) }
        if gp.buttonY.isPressed { buttons |= UInt16(NS_BTN_X) }
        if gp.leftShoulder.isPressed { buttons |= UInt16(NS_BTN_L) }
        if gp.rightShoulder.isPressed { buttons |= UInt16(NS_BTN_R) }
        if gp.leftTrigger.value > 0.5 { buttons |= UInt16(NS_BTN_ZL) }
        if gp.rightTrigger.value > 0.5 { buttons |= UInt16(NS_BTN_ZR) }
        if gp.buttonOptions?.isPressed == true { buttons |= UInt16(NS_BTN_MINUS) }
        if gp.buttonMenu.isPressed { buttons |= UInt16(NS_BTN_PLUS) }
        if gp.leftThumbstickButton?.isPressed == true { buttons |= UInt16(NS_BTN_LSTICK) }
        if gp.rightThumbstickButton?.isPressed == true { buttons |= UInt16(NS_BTN_RSTICK) }
        if gp.buttonHome?.isPressed == true { buttons |= UInt16(NS_BTN_HOME) }

        var hid = Data(count: Int(NS_PROTOCOL_HID_SIZE))
        hid.withUnsafeMutableBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            ns_hid_write_controller(base, buttons,
                                    gp.dpad.up.isPressed ? 1 : 0,
                                    gp.dpad.down.isPressed ? 1 : 0,
                                    gp.dpad.left.isPressed ? 1 : 0,
                                    gp.dpad.right.isPressed ? 1 : 0,
                                    gp.leftThumbstick.xAxis.value,
                                    -gp.leftThumbstick.yAxis.value,
                                    gp.rightThumbstick.xAxis.value,
                                    -gp.rightThumbstick.yAxis.value,
                                    1)
        }
        pad.withUnsafeMutableBytes { padRaw in
            hid.withUnsafeBytes { hidRaw in
                guard let padBase = padRaw.bindMemory(to: UInt8.self).baseAddress,
                      let hidBase = hidRaw.bindMemory(to: UInt8.self).baseAddress else { return }
                ns_pad_set_hid(padBase, hidBase)
            }
        }

        if let motion = controller.motion {
            var sample = Data(count: kMotionSize)
            sample.withUnsafeMutableBytes { raw in
                guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
                ns_motion_from_apple(base,
                                     Float(motion.gravity.x), -Float(motion.gravity.y), Float(motion.gravity.z),
                                     Float(motion.rotationRate.x), -Float(motion.rotationRate.y), Float(motion.rotationRate.z))
            }
            if slot < controllerMotionSamples.count {
                controllerMotionSamples[slot].append(sample)
                if controllerMotionSamples[slot].count > kMotionSampleCount {
                    controllerMotionSamples[slot].removeFirst(controllerMotionSamples[slot].count - kMotionSampleCount)
                }
                if controllerMotionSamples[slot].count >= kMotionSampleCount {
                    mergeMotionSamples(controllerMotionSamples[slot], into: &pad)
                }
            }
        }
        return pad
    }

    private func controllerRumble(slot: Int, low: UInt8, high: UInt8, duration10Ms: UInt8) {
        guard currentMode == .controllerHub, slot >= 0, slot < Int(NS_PROTOCOL_PAD_COUNT) else { return }
        let neutral = (low == 0 && high == 0) || duration10Ms == 0
        let now = Date().timeIntervalSinceReferenceDate
        let duration = neutral ? 0.0 : max(0.25, Double(max(1, Int(duration10Ms))) * 0.010)
        if !neutral {
            var st = controllerRumbleStates[slot]
            if st.low == low && st.high == high && now - st.lastSet < 0.10 {
                st.until = now + duration
                controllerRumbleStates[slot] = st
                return
            }
            st.low = low; st.high = high; st.until = now + duration; st.lastSet = now
            controllerRumbleStates[slot] = st
        } else {
            try? controllerRumbleStates[slot].player?.stop(atTime: 0)
            controllerRumbleStates[slot] = ControllerRumbleState()
        }
        DispatchQueue.main.async { self.onRumble?(slot, low, high) }

        guard !neutral, #available(iOS 14.0, *) else { return }
        let controllers = Array(GCController.controllers().prefix(Int(NS_PROTOCOL_PAD_COUNT)))
        guard slot < controllers.count else { return }
        let controller = controllers[slot]
        guard let haptics = controller.haptics else { return }
        let key = ObjectIdentifier(controller)
        let engine: CHHapticEngine
        if let cached = controllerHapticEngines[key] {
            engine = cached
        } else if let created = haptics.createEngine(withLocality: .handles) {
            engine = created
            engine.playsHapticsOnly = true
            controllerHapticEngines[key] = created
        } else {
            return
        }
        do {
            try engine.start()
            let strength = min(1.0, Float(max(low, high)) / 255.0)
            let intensity = CHHapticEventParameter(parameterID: .hapticIntensity, value: strength)
            let sharpness = CHHapticEventParameter(parameterID: .hapticSharpness, value: 0.45)
            let event = CHHapticEvent(eventType: .hapticContinuous,
                                      parameters: [intensity, sharpness],
                                      relativeTime: 0,
                                      duration: duration)
            let pattern = try CHHapticPattern(events: [event], parameters: [])
            try controllerRumbleStates[slot].player?.stop(atTime: 0)
            let player = try engine.makePlayer(with: pattern)
            controllerRumbleStates[slot].player = player
            try player.start(atTime: 0)
        } catch {
            // Some controllers/OS versions expose input but refuse haptic engines.
        }
    }
}
