import Foundation
import CoreMotion
import CoreHaptics
import GameController

// Constants matching server/include/protocol.hpp (Swift-friendly)
private let kProtoMagic: UInt32   = 0x4E535743
private let kWebProtoVer: UInt8   = 5
private let kSinglePad: UInt8     = 0x04
private let kExtPresent: UInt8    = 0x01
private let kPadSize              = 24 // sizeof(ExtendedHIDReport)

private let kBtnY: UInt16      = 1<<0;  let kBtnB: UInt16      = 1<<1
private let kBtnA: UInt16      = 1<<2;  let kBtnX: UInt16      = 1<<3
private let kBtnL: UInt16      = 1<<4;  let kBtnR: UInt16      = 1<<5
private let kBtnZL: UInt16     = 1<<6;  let kBtnZR: UInt16     = 1<<7
private let kBtnMinus: UInt16  = 1<<8;  let kBtnPlus: UInt16   = 1<<9
private let kBtnLStick: UInt16 = 1<<10; let kBtnRStick: UInt16 = 1<<11
private let kBtnHome: UInt16   = 1<<12; let kBtnCapture: UInt16 = 1<<13

private let kHatN: UInt8 = 0; let kHatNE: UInt8 = 1; let kHatE: UInt8 = 2; let kHatSE: UInt8 = 3
private let kHatS: UInt8 = 4; let kHatSW: UInt8 = 5; let kHatW: UInt8 = 6; let kHatNW: UInt8 = 7
private let kHatNeutral: UInt8 = 8

private let kGyroScale: Float = 938.732 // 57.2958 * 16.384 (rad/s → Switch IMU)

final class BridgeManager: NSObject, URLSessionWebSocketDelegate {
    static let shared = BridgeManager()

    var onStatus: ((String) -> Void)?
    var onRumble: ((_ pad: Int, _ low: UInt8, _ high: UInt8) -> Void)?

    private var ws: URLSessionWebSocketTask?
    private lazy var session = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: nil)
    private let motion = CMMotionManager()
    private var engine: CHHapticEngine?
    private var engineNeedsStart = true
    private var seq: UInt32 = 0
    private var connected = false
    private let queue = DispatchQueue(label: "bridge", qos: .userInitiated)
    // Pad 0 touch data bridged from WebView JS (24 raw bytes)
    private var touchPad = Data(count: kPadSize)

    private override init() {
        super.init()
        startHaptics()
        observeControllers()
    }

    // MARK: - Connection

    func connect(host: String, port: UInt16 = 8080) {
        disconnect()
        guard let url = URL(string: "ws://\(host):\(port)/") else { return }
        var req = URLRequest(url: url)
        req.timeoutInterval = 10
        ws = session.webSocketTask(with: req)
        ws?.resume()
        readRumble()
        startGyro()
        DispatchQueue.main.async { self.onStatus?("Connecting...") }
    }

    func disconnect() {
        ws?.cancel()
        ws = nil
        connected = false
        stopGyro()
        DispatchQueue.main.async { self.onStatus?("Disconnected") }
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
        stopGyro()
        DispatchQueue.main.async { self.onStatus?("Disconnected") }
    }

    /// Called from injected JS bridge with 24 raw bytes for pad 0
    func bridgeTouchPad(_ data: Data) {
        objc_sync_enter(self)
        if data.count >= kPadSize { touchPad = Data(data.prefix(kPadSize)) }
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

    private func sendFrame() {
        var frame = Data(capacity: 116)
        // Header (all little-endian)
        withUnsafeBytes(of: kProtoMagic.littleEndian) { frame.append(contentsOf: $0) }
        frame.append(kWebProtoVer)
        frame.append(kSinglePad)
        frame.append(contentsOf: [0, 0]) // reserved
        withUnsafeBytes(of: seq.littleEndian) { frame.append(contentsOf: $0) }
        let ts = UInt64(Date().timeIntervalSince1970 * 1_000_000).littleEndian
        withUnsafeBytes(of: ts) { frame.append(contentsOf: $0) }

        for p in 0..<4 {
            var pad = Data(count: kPadSize)
            if p == 0 {
                objc_sync_enter(self)
                pad = self.touchPad
                objc_sync_exit(self)
                // Overwrite motion bytes (offset 8-19) with gyro
                if let g = self.currentGyro {
                    // Match protocol.hpp layout and the desktop SDL mapping:
                    // MotionReport = ax, ay, az, gx, gy, gz.
                    let ax = clampMotion(Float(-g.gravity.x) * 4096)
                    let ay = clampMotion(Float(-g.gravity.z) * 4096)
                    let az = clampMotion(Float( g.gravity.y) * 4096)
                    let gx = clampMotion(Float(-g.rotationRate.x) * kGyroScale)
                    let gy = clampMotion(Float(-g.rotationRate.z) * kGyroScale)
                    let gz = clampMotion(Float( g.rotationRate.y) * kGyroScale)
                    pad[8..<10] = withUnsafeBytes(of: ax.littleEndian) { Data($0) }
                    pad[10..<12] = withUnsafeBytes(of: ay.littleEndian) { Data($0) }
                    pad[12..<14] = withUnsafeBytes(of: az.littleEndian) { Data($0) }
                    pad[14..<16] = withUnsafeBytes(of: gx.littleEndian) { Data($0) }
                    pad[16..<18] = withUnsafeBytes(of: gy.littleEndian) { Data($0) }
                    pad[18..<20] = withUnsafeBytes(of: gz.littleEndian) { Data($0) }
                    pad[20] = 1 // has_motion
                }
                if let gc = self.controllers[0] {
                    self.mergeGCController(gc, into: &pad)
                }
            } else if p < self.controllers.count, let gc = self.controllers[p] {
                self.mergeGCController(gc, into: &pad)
                pad[7] |= kExtPresent
            }
            frame.append(pad)
        }
        seq &+= 1
        ws?.send(.data(frame)) { _ in }
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
                        self.playHaptic(low: low, high: high)
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

    private func playHaptic(low: UInt8, high: UInt8) {
        guard let engine = engine else { return }
        if engineNeedsStart { try? engine.start(); engineNeedsStart = false }
        do {
            let intensity = Float(high) / 255.0
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

    // MARK: - Physical controllers (GCController)

    private var controllers: [GCController?] = [nil, nil, nil, nil]

    private func observeControllers() {
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            guard let gc = n.object as? GCController else { return }
            for i in 0..<4 where self?.controllers[i] == nil { self?.controllers[i] = gc; break }
        }
        NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] n in
            guard let gc = n.object as? GCController else { return }
            self?.controllers = self?.controllers.map { $0 === gc ? nil : $0 } ?? []
        }
        GCController.startWirelessControllerDiscovery {}
    }

    private func clampMotion(_ v: Float) -> Int16 {
        if v > 32767 { return 32767 }
        if v < -32768 { return -32768 }
        return Int16(v.rounded())
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
