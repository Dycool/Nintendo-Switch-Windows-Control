import UIKit
import WebKit
import CoreMotion
import Network
import GameController
import CoreHaptics

private enum Page {
    case mainMenu
    case touchControls
    case editor
}

private enum ClientMode {
    case none
    case touch
    case physical
}

private enum ProtocolWire {
    static let frameSize = 212
    static let hidSize = 8
    static let motionSampleSize = 12
    static let motionSampleCount = 3
    static let padCount = 4

    static let rumblePacketSize = 8
    static let precisionRumblePacketSize = 20
    static let rumbleMagic: UInt32 = 0x4E535652
    static let precisionRumbleMagic: UInt32 = 0x4E535648

    static let flagReset = 0x01
    static let flagSinglePad = 0x04

    static let btnY       = 1 << 0
    static let btnB       = 1 << 1
    static let btnA       = 1 << 2
    static let btnX       = 1 << 3
    static let btnL       = 1 << 4
    static let btnR       = 1 << 5
    static let btnZL      = 1 << 6
    static let btnZR      = 1 << 7
    static let btnMinus   = 1 << 8
    static let btnPlus    = 1 << 9
    static let btnLStick  = 1 << 10
    static let btnRStick  = 1 << 11
    static let btnHome    = 1 << 12
    static let btnCapture = 1 << 13

    static let hatN = 0
    static let hatNE = 1
    static let hatE = 2
    static let hatSE = 3
    static let hatS = 4
    static let hatSW = 5
    static let hatW = 6
    static let hatNW = 7
    static let hatNeutral = 8

    static func neutralHid() -> [UInt8] {
        var out = [UInt8](repeating: 0, count: hidSize)
        out.withUnsafeMutableBufferPointer { b in
            ns_hid_write_neutral(b.baseAddress!)
        }
        return out
    }

    static func neutralMotion() -> [UInt8] {
        [UInt8](repeating: 0, count: motionSampleSize)
    }

    static func hid(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int, present: Bool) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: hidSize)
        out.withUnsafeMutableBufferPointer { b in
            ns_hid_write(b.baseAddress!, UInt16(buttons & 0xFFFF), UInt8(clamping: hat),
                         UInt8(clamping: lx), UInt8(clamping: ly), UInt8(clamping: rx), UInt8(clamping: ry),
                         present ? 1 : 0)
        }
        return out
    }

    static func axisToByte(_ value: Float) -> Int {
        Int(ns_axis_to_byte(value))
    }

    static func normalizeShortcuts(_ buttons: Int) -> Int {
        Int(ns_normalize_system_shortcuts(UInt16(buttons & 0xFFFF)))
    }

    static func motionFromApple(gravityX: Float, gravityY: Float, gravityZ: Float,
                                rotationX: Float, rotationY: Float, rotationZ: Float) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: motionSampleSize)
        out.withUnsafeMutableBufferPointer { b in
            ns_motion_from_apple(b.baseAddress!, gravityX, gravityY, gravityZ, rotationX, rotationY, rotationZ)
        }
        return out
    }

    static func initFrame(flags: Int, seq: UInt32, timestampUs: UInt64) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: frameSize)
        out.withUnsafeMutableBufferPointer { b in
            ns_web_frame_init(b.baseAddress!, UInt8(flags & 0xFF), seq, timestampUs)
        }
        return out
    }

    static func setFrameHid(_ frame: inout [UInt8], pad: Int, hid: [UInt8]) {
        frame.withUnsafeMutableBufferPointer { f in
            hid.withUnsafeBufferPointer { h in
                ns_web_frame_set_hid(f.baseAddress!, Int32(pad), h.baseAddress!)
            }
        }
    }

    static func setFrameMotionSamples(_ frame: inout [UInt8], pad: Int, samples: [[UInt8]]) {
        guard samples.count >= motionSampleCount else { return }
        frame.withUnsafeMutableBufferPointer { f in
            samples[0].withUnsafeBufferPointer { m0 in
                samples[1].withUnsafeBufferPointer { m1 in
                    samples[2].withUnsafeBufferPointer { m2 in
                        ns_web_frame_set_motion_samples(f.baseAddress!, Int32(pad), m0.baseAddress!, m1.baseAddress!, m2.baseAddress!)
                    }
                }
            }
        }
    }

    static func extractPad0Hid(from frame: [UInt8]) -> [UInt8]? {
        guard frame.count >= 20 + hidSize else { return nil }
        var out = [UInt8](repeating: 0, count: hidSize)
        frame.withUnsafeBufferPointer { f in
            out.withUnsafeMutableBufferPointer { h in
                _ = ns_web_frame_extract_hid(f.baseAddress!, frame.count, 0, h.baseAddress!)
            }
        }
        return out
    }
}

private final class Locked<T> {
    private let lock = NSLock()
    private var value: T

    init(_ value: T) { self.value = value }

    func withLock<R>(_ body: (inout T) -> R) -> R {
        lock.lock()
        defer { lock.unlock() }
        return body(&value)
    }
}

private final class PhysicalPad {
    var controller: GCController?
    var name = "Empty"
    var present = false
    var buttons = 0
    var dpadUp = false
    var dpadDown = false
    var dpadLeft = false
    var dpadRight = false
    var lx = 128
    var ly = 128
    var rx = 128
    var ry = 128
    var hasMotion = false
    var hasGyro = false
    var hasRumble = false
    var rumbleLow = 0
    var rumbleHigh = 0
    var rumbleUntilMs: UInt64 = 0
    var rumbleLastSetMs: UInt64 = 0
    var hapticEngine: CHHapticEngine?
    var hapticPlayer: CHHapticAdvancedPatternPlayer?
    var motionSamples = Array(repeating: ProtocolWire.neutralMotion(), count: ProtocolWire.motionSampleCount)
    var motionSampleCount = 0

    func reset() {
        stopRumble()
        controller = nil
        name = "Empty"
        present = false
        buttons = 0
        dpadUp = false
        dpadDown = false
        dpadLeft = false
        dpadRight = false
        lx = 128; ly = 128; rx = 128; ry = 128
        hasMotion = false
        hasGyro = false
        hasRumble = false
        rumbleLow = 0
        rumbleHigh = 0
        rumbleUntilMs = 0
        rumbleLastSetMs = 0
        motionSampleCount = 0
        motionSamples = Array(repeating: ProtocolWire.neutralMotion(), count: ProtocolWire.motionSampleCount)
    }

    func hid() -> [UInt8] {
        let hat: Int
        if dpadUp && dpadRight { hat = ProtocolWire.hatNE }
        else if dpadUp && dpadLeft { hat = ProtocolWire.hatNW }
        else if dpadDown && dpadRight { hat = ProtocolWire.hatSE }
        else if dpadDown && dpadLeft { hat = ProtocolWire.hatSW }
        else if dpadUp { hat = ProtocolWire.hatN }
        else if dpadRight { hat = ProtocolWire.hatE }
        else if dpadDown { hat = ProtocolWire.hatS }
        else if dpadLeft { hat = ProtocolWire.hatW }
        else { hat = ProtocolWire.hatNeutral }
        return ProtocolWire.hid(buttons: ProtocolWire.normalizeShortcuts(buttons), hat: hat,
                                lx: lx, ly: ly, rx: rx, ry: ry, present: present)
    }

    func stopRumble() {
        try? hapticPlayer?.stop(atTime: CHHapticTimeImmediate)
        hapticPlayer = nil
        hapticEngine?.stop(completionHandler: nil)
        hapticEngine = nil
    }
}

final class ViewController: UIViewController, WKScriptMessageHandler, WKNavigationDelegate, URLSessionWebSocketDelegate, UIGestureRecognizerDelegate {
    var orientationMask: UIInterfaceOrientationMask = .allButUpsideDown

    private let connectView = UIView()
    private let hostField = UITextField()
    private let statusLabel = UILabel()
    private let connectButton = UIButton(type: .system)
    private var webView: WKWebView!

    private var host = ""
    private var connected = false
    private var controlClientActive = false
    private var sending = false
    private var webSocket: URLSessionWebSocketTask?
    private lazy var session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
    private var seq: UInt32 = 0
    private let sendQueue = DispatchQueue(label: "ns.mobile.ios.sender")
    private let stateQueue = DispatchQueue(label: "ns.mobile.ios.state")
    private var senderToken = 0

    private var currentOrientation: UIInterfaceOrientation = .landscapeRight
    private var currentPage: Page = .mainMenu
    private var pageStack: [Page] = []
    private var activeClientMode: ClientMode = .none

    private let motionManager = CMMotionManager()
    private let motionQueue = OperationQueue()
    private var phoneSensorsActive = false
    private let phoneMotion = Locked(PhoneMotionState())

    private struct PhoneMotionState {
        var samples = Array(repeating: ProtocolWire.neutralMotion(), count: ProtocolWire.motionSampleCount)
        var count = 0
    }

    private var touchHid: [UInt8]?
    private var touchFrame: [UInt8]?
    private var lastTouchFrameMs: UInt64 = 0
    private var lastBridgeFrameParseMs: UInt64 = 0

    private let physicalPads = Locked((0..<ProtocolWire.padCount).map { _ in PhysicalPad() })
    private var controllerSlots: [ObjectIdentifier: Int] = [:]

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        requestLocalNetworkPermission()
        setupConnectView()
        setupWebView()
        setupControllerObservers()
        view.addSubview(connectView)
        connectView.frame = view.bounds
        connectView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        hostField.text = UserDefaults.standard.string(forKey: "host") ?? ""
    }

    private func requestLocalNetworkPermission() {
        let connection = NWConnection(
            host: NWEndpoint.Host("255.255.255.255"),
            port: NWEndpoint.Port(integerLiteral: 0),
            using: .udp
        )
        connection.stateUpdateHandler = { state in
            switch state {
            case .ready, .failed, .cancelled, .waiting:
                connection.cancel()
                connection.stateUpdateHandler = nil
            default:
                break
            }
        }
        connection.start(queue: DispatchQueue.global(qos: .background))
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        currentOrientation = view.window?.windowScene?.interfaceOrientation ?? .landscapeRight
        connectView.frame = view.bounds
        webView?.frame = view.bounds
    }

    private func setupConnectView() {
        connectView.backgroundColor = UIColor(red: 0.03, green: 0.03, blue: 0.04, alpha: 1.0)

        let logoView = UIImageView(image: UIImage(named: "Logo"))
        logoView.contentMode = .scaleAspectFit
        logoView.translatesAutoresizingMaskIntoConstraints = false

        let titleLabel = UILabel()
        titleLabel.text = "NS Mobile"
        titleLabel.textColor = UIColor(red: 0.80, green: 0.0, blue: 0.0, alpha: 1.0)
        titleLabel.font = UIFont.systemFont(ofSize: 34, weight: .bold)
        titleLabel.textAlignment = .center
        titleLabel.translatesAutoresizingMaskIntoConstraints = false

        let subtitleLabel = UILabel()
        subtitleLabel.text = "Connect to your Raspberry Pi server"
        subtitleLabel.textColor = UIColor(white: 1.0, alpha: 0.55)
        subtitleLabel.font = UIFont.systemFont(ofSize: 16)
        subtitleLabel.textAlignment = .center
        subtitleLabel.translatesAutoresizingMaskIntoConstraints = false

        hostField.placeholder = "Server IP or domain"
        hostField.autocapitalizationType = .none
        hostField.autocorrectionType = .no
        hostField.keyboardType = .URL
        hostField.returnKeyType = .go
        hostField.textColor = .white
        hostField.tintColor = .white
        hostField.backgroundColor = UIColor(white: 1.0, alpha: 0.10)
        hostField.layer.cornerRadius = 12
        hostField.layer.borderWidth = 1
        hostField.layer.borderColor = UIColor(white: 1.0, alpha: 0.15).cgColor
        hostField.leftView = UIView(frame: CGRect(x: 0, y: 0, width: 16, height: 1))
        hostField.leftViewMode = .always
        hostField.translatesAutoresizingMaskIntoConstraints = false
        hostField.attributedPlaceholder = NSAttributedString(string: "Server IP or domain", attributes: [.foregroundColor: UIColor(white: 1.0, alpha: 0.40)])
        hostField.addTarget(self, action: #selector(hostReturnPressed), for: .editingDidEndOnExit)

        connectButton.setTitle("Connect", for: .normal)
        connectButton.titleLabel?.font = UIFont.systemFont(ofSize: 18, weight: .semibold)
        connectButton.backgroundColor = UIColor(red: 0.80, green: 0.0, blue: 0.0, alpha: 1.0)
        connectButton.tintColor = .white
        connectButton.layer.cornerRadius = 12
        connectButton.translatesAutoresizingMaskIntoConstraints = false
        connectButton.addTarget(self, action: #selector(onConnect), for: .touchUpInside)

        statusLabel.text = "Ready"
        statusLabel.textColor = UIColor(white: 1.0, alpha: 0.45)
        statusLabel.textAlignment = .center
        statusLabel.translatesAutoresizingMaskIntoConstraints = false

        let stack = UIStackView(arrangedSubviews: [logoView, titleLabel, subtitleLabel, hostField, connectButton, statusLabel])
        stack.axis = .vertical
        stack.spacing = 0
        stack.setCustomSpacing(16, after: logoView)
        stack.setCustomSpacing(8, after: titleLabel)
        stack.setCustomSpacing(40, after: subtitleLabel)
        stack.setCustomSpacing(20, after: hostField)
        stack.setCustomSpacing(16, after: connectButton)
        stack.translatesAutoresizingMaskIntoConstraints = false
        connectView.addSubview(stack)

        NSLayoutConstraint.activate([
            logoView.heightAnchor.constraint(equalToConstant: 96),
            logoView.widthAnchor.constraint(equalToConstant: 96),
            stack.centerYAnchor.constraint(equalTo: connectView.centerYAnchor),
            stack.centerXAnchor.constraint(equalTo: connectView.centerXAnchor),
            stack.widthAnchor.constraint(equalTo: connectView.safeAreaLayoutGuide.widthAnchor, constant: -64),
            hostField.heightAnchor.constraint(equalToConstant: 56),
            connectButton.heightAnchor.constraint(equalToConstant: 56)
        ])
        stack.widthAnchor.constraint(lessThanOrEqualToConstant: 400).isActive = true
    }

    private func setupWebView() {
        let content = WKUserContentController()
        let bridgeScript = """
        (function(){
          if (window.NSBridge) return;
          function post(name,args){ window.webkit.messageHandlers.NSBridge.postMessage({name:name,args:args||[]}); }
          window.NSBridge = {
            onOpen:function(){post('onOpen');},
            onBinary:function(json){post('onBinary',[json]);},
            onTouchState:function(buttons,hat,lx,ly,rx,ry){post('onTouchState',[buttons,hat,lx,ly,rx,ry]);},
            onClose:function(){post('onClose');},
            onPhysicalStart:function(){post('onPhysicalStart');},
            onPhysicalStop:function(){post('onPhysicalStop');},
            onPhysicalRefresh:function(){post('onPhysicalRefresh');},
            onOpenTouch:function(){post('onOpenTouch');},
            onOpenEditor:function(){post('onOpenEditor');},
            onBack:function(){post('onBack');}
          };
        })();
        """
        content.addUserScript(WKUserScript(source: bridgeScript, injectionTime: .atDocumentStart, forMainFrameOnly: false))
        content.add(self, name: "NSBridge")

        let config = WKWebViewConfiguration()
        config.userContentController = content
        config.preferences.javaScriptEnabled = true
        webView = WKWebView(frame: view.bounds, configuration: config)
        webView.navigationDelegate = self
        webView.backgroundColor = .black
        webView.isOpaque = false
        webView.scrollView.bounces = false
        webView.scrollView.isScrollEnabled = false
        webView.autoresizingMask = [.flexibleWidth, .flexibleHeight]

        let edge = UIScreenEdgePanGestureRecognizer(target: self, action: #selector(edgeBack(_:)))
        edge.edges = .left
        edge.cancelsTouchesInView = false
        edge.delegate = self
        webView.addGestureRecognizer(edge)
    }

    @objc private func hostReturnPressed() { onConnect() }

    @objc private func onConnect() {
        host = (hostField.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        guard !host.isEmpty else { return }
        UserDefaults.standard.set(host, forKey: "host")
        connected = true
        currentPage = .mainMenu
        pageStack.removeAll()
        statusLabel.text = "Loaded"
        connectView.removeFromSuperview()
        view.addSubview(webView)
        load(page: .mainMenu)
    }

    @objc private func edgeBack(_ recognizer: UIScreenEdgePanGestureRecognizer) {
        guard recognizer.state == .ended, connected else { return }
        let translation = recognizer.translation(in: webView)
        guard translation.x > 40 else { return }

        if currentPage == .mainMenu {
            disconnect()
        } else {
            goBack()
        }
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
        connected && (currentPage == .mainMenu || currentPage == .touchControls || currentPage == .editor)
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer) -> Bool {
        true
    }

    private func pageURL(_ page: Page) -> URL? {
        let name: String
        switch page {
        case .mainMenu: name = "index"
        case .touchControls: name = "mobile"
        case .editor: name = "editor"
        }
        return Bundle.main.url(forResource: name, withExtension: "html", subdirectory: "ns_mobile")
    }

    private func load(page: Page) {
        guard let url = pageURL(page), let dir = Bundle.main.url(forResource: "ns_mobile", withExtension: nil) else { return }
        webView.loadFileURL(url, allowingReadAccessTo: dir)
    }

    private func navTo(_ page: Page) {
        if page == .touchControls || page == .editor {
            deactivateControlClient()
            clearPhysicalControllers()
        }
        pageStack.append(currentPage)
        enterPage(page)
    }

    private func enterPage(_ page: Page) {
        currentPage = page
        if page == .touchControls || page == .editor {
            deactivateControlClient()
            setLandscapeMode(true)
            setFullscreen(true)
        } else {
            setLandscapeMode(false)
            setFullscreen(false)
        }
        load(page: page)
    }

    private func goBack() {
        guard !pageStack.isEmpty else { return }
        if currentPage == .touchControls || currentPage == .editor {
            deactivateControlClient()
            clearPhysicalControllers()
        }
        let page = pageStack.removeLast()
        enterPage(page)
    }

    private func setLandscapeMode(_ landscape: Bool) {
        orientationMask = landscape ? .landscape : .allButUpsideDown
        if #available(iOS 16.0, *) {
            view.window?.windowScene?.requestGeometryUpdate(.iOS(interfaceOrientations: orientationMask))
        }
        if landscape {
            UIDevice.current.setValue(UIInterfaceOrientation.landscapeRight.rawValue, forKey: "orientation")
        }
        if #available(iOS 16.0, *) {
            setNeedsUpdateOfSupportedInterfaceOrientations()
        }
    }

    private func setFullscreen(_ fullscreen: Bool) {
        navigationController?.setNavigationBarHidden(fullscreen, animated: false)
        setNeedsUpdateOfHomeIndicatorAutoHidden()
        UIView.performWithoutAnimation { setNeedsStatusBarAppearanceUpdate() }
    }

    override var prefersStatusBarHidden: Bool { currentPage == .touchControls || currentPage == .editor }
    override var prefersHomeIndicatorAutoHidden: Bool { currentPage == .touchControls || currentPage == .editor }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        if currentPage == .mainMenu {
            webView.evaluateJavaScript(mainMenuInjection(), completionHandler: nil)
        }
    }

    private func mainMenuInjection() -> String {
        """
        (function(){
          var kb = document.getElementById('kbModeContainer'); if (kb) kb.style.display = 'none';
          var bindings = document.getElementById('btnBindings'); if (bindings) bindings.style.display = 'none';
          var macros = document.getElementById('btnMacros'); if (macros) macros.style.display = 'none';
          var oldStart = document.getElementById('btn' + 'HubStart'); if (oldStart) oldStart.remove();
          var oldStop = document.getElementById('btn' + 'HubStop'); if (oldStop) oldStop.remove();
          var oldRefresh = document.getElementById('btn' + 'HubRefresh'); if (oldRefresh) oldRefresh.remove();
          var connect = document.getElementById('btnConnect');
          if (connect) {
            connect.style.display = 'inline-block';
            connect.textContent = 'Connect';
            connect.onclick = function(ev){ if(ev)ev.preventDefault(); if(window.NSBridge&&NSBridge.onPhysicalStart)NSBridge.onPhysicalStart(); return false; };
          }
          function nsButtonHost(){
            return document.querySelector('.actions') || document.querySelector('main') || document.body;
          }
          var touch = document.getElementById('btnTouchControls');
          if (!touch) {
            touch = document.createElement('button');
            touch.id = 'btnTouchControls';
            touch.textContent = 'Touch Controls';
            nsButtonHost().appendChild(touch);
          }
          touch.style.display = 'inline-block';
          touch.onclick = function(ev){ if(ev)ev.preventDefault(); if(window.NSBridge&&NSBridge.onOpenTouch)NSBridge.onOpenTouch(); else window.location.href='mobile.html'; return false; };

          var editor = document.getElementById('btnEditor');
          if (!editor) {
            editor = document.createElement('button');
            editor.id = 'btnEditor';
            editor.textContent = 'Editor';
            nsButtonHost().appendChild(editor);
          }
          editor.style.display = 'inline-block';
          editor.onclick = function(ev){ if(ev)ev.preventDefault(); if(window.NSBridge&&NSBridge.onOpenEditor)NSBridge.onOpenEditor(); else window.location.href='editor.html'; return false; };
          if (window.NSBridge && NSBridge.onPhysicalRefresh) NSBridge.onPhysicalRefresh();
        })();
        """
    }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        guard message.name == "NSBridge" else { return }
        guard let dict = message.body as? [String: Any], let name = dict["name"] as? String else { return }
        let args = dict["args"] as? [Any] ?? []

        switch name {
        case "onOpen":
            if currentPage == .touchControls { activateTouchClient() }
        case "onBinary":
            if let json = args.first as? String { onBinary(json: json) }
        case "onTouchState":
            if args.count >= 6 {
                onTouchState(buttons: intArg(args[0]), hat: intArg(args[1]), lx: intArg(args[2]), ly: intArg(args[3]), rx: intArg(args[4]), ry: intArg(args[5]))
            }
        case "onClose":
            deactivateControlClient()
        case "onPhysicalStart":
            togglePhysicalControllers()
        case "onPhysicalStop":
            deactivateControlClient()
            updatePhysicalStatusOnPage(prefix: "Not connected")
        case "onPhysicalRefresh":
            scanPhysicalControllers()
            updatePhysicalStatusOnPage()
        case "onOpenTouch":
            navTo(.touchControls)
        case "onOpenEditor":
            navTo(.editor)
        case "onBack":
            goBack()
        default:
            break
        }
    }

    private func intArg(_ value: Any) -> Int {
        if let v = value as? Int { return v }
        if let v = value as? Double { return Int(v) }
        if let v = value as? String { return Int(v) ?? 0 }
        return 0
    }

    private func onBinary(json: String) {
        guard currentPage == .touchControls && controlClientActive else { return }
        let now = uptimeMs()
        guard now - lastBridgeFrameParseMs >= 8 else { return }
        lastBridgeFrameParseMs = now
        guard let data = json.data(using: .utf8), let arr = try? JSONSerialization.jsonObject(with: data) as? [Any], arr.count >= 20 + ProtocolWire.hidSize else { return }
        let frame = arr.map { UInt8(clamping: intArg($0)) }
        touchFrame = frame
        touchHid = ProtocolWire.extractPad0Hid(from: frame)
        lastTouchFrameMs = now
    }

    private func onTouchState(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int) {
        guard currentPage == .touchControls && controlClientActive else { return }
        touchHid = ProtocolWire.hid(buttons: ProtocolWire.normalizeShortcuts(buttons),
                                    hat: min(max(hat, 0), 8),
                                    lx: min(max(lx, 0), 255),
                                    ly: min(max(ly, 0), 255),
                                    rx: min(max(rx, 0), 255),
                                    ry: min(max(ry, 0), 255),
                                    present: true)
        lastTouchFrameMs = uptimeMs()
    }

    private func togglePhysicalControllers() {
        if controlClientActive && activeClientMode == .physical {
            deactivateControlClient()
            updatePhysicalStatusOnPage(prefix: "Not connected")
        } else {
            activatePhysicalControllers()
        }
    }

    private func activatePhysicalControllers() {
        if controlClientActive && activeClientMode == .physical {
            scanPhysicalControllers()
            updatePhysicalStatusOnPage(prefix: "Connected")
            return
        }
        deactivateControlClient()
        currentPage = .mainMenu
        activeClientMode = .physical
        controlClientActive = true
        touchHid = nil
        touchFrame = nil
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        scanPhysicalControllers()
        updatePhysicalStatusOnPage(prefix: "Connected")
        if !connectWs() {
            controlClientActive = false
            activeClientMode = .none
            clearPhysicalControllers()
            updatePhysicalStatusOnPage(prefix: "Not connected")
        }
    }

    private func activateTouchClient() {
        if controlClientActive && activeClientMode == .touch { return }
        deactivateControlClient()
        clearPhysicalControllers()
        activeClientMode = .touch
        touchHid = nil
        touchFrame = nil
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        controlClientActive = true
        if !connectWs() {
            controlClientActive = false
            activeClientMode = .none
        }
    }

    private func deactivateControlClient() {
        if !controlClientActive && webSocket == nil && !sending { return }
        let closing = webSocket
        senderToken += 1
        sending = false
        controlClientActive = false
        touchHid = nil
        touchFrame = nil
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        webSocket = nil
        stopPhoneSensors()
        stopAllPhysicalRumble()
        if activeClientMode != .physical {
            clearPhysicalControllers()
        }
        activeClientMode = .none

        if let closing {
            sendQueue.async { [weak self] in
                guard let self else { return }
                for _ in 0..<3 {
                    self.sendResetFrame(to: closing)
                    Thread.sleep(forTimeInterval: 0.004)
                }
                closing.cancel(with: .normalClosure, reason: "Leaving controls".data(using: .utf8))
            }
        }
    }

    private func disconnect() {
        deactivateControlClient()
        connected = false
        setLandscapeMode(false)
        setFullscreen(false)
        webView.loadHTMLString("", baseURL: nil)
        webView.removeFromSuperview()
        view.addSubview(connectView)
    }

    private func connectWs() -> Bool {
        do {
            let url = try normalizeWsUrl(host)
            let task = session.webSocketTask(with: url)
            webSocket = task
            task.resume()
            receiveLoop(task)
            return true
        } catch {
            statusLabel.text = "Invalid server address"
            return false
        }
    }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol protocol: String?) {
        DispatchQueue.main.async { [weak self, weak webSocketTask] in
            guard let self, let task = webSocketTask, self.webSocket === task, self.controlClientActive else { return }
            self.statusLabel.text = "Connected"
            self.startSending()
        }
    }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?) {
        DispatchQueue.main.async { [weak self, weak webSocketTask] in
            guard let self, let task = webSocketTask, self.webSocket === task else { return }
            self.handleWsClosed(text: "Disconnected")
        }
    }

    private func receiveLoop(_ task: URLSessionWebSocketTask) {
        task.receive { [weak self, weak task] result in
            guard let self, let task else { return }
            switch result {
            case .success(let message):
                if case .data(let data) = message { self.handleRumblePacket(data) }
                if self.webSocket === task { self.receiveLoop(task) }
            case .failure:
                DispatchQueue.main.async { [weak self, weak task] in
                    guard let self, let task, self.webSocket === task else { return }
                    self.handleWsClosed(text: "Connection failed")
                }
            }
        }
    }

    private func handleWsClosed(text: String) {
        statusLabel.text = text
        sending = false
        controlClientActive = false
        touchHid = nil
        touchFrame = nil
        lastTouchFrameMs = 0
        webSocket = nil
        stopPhoneSensors()
    }

    private func normalizeWsUrl(_ raw: String) throws -> URL {
        var text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { throw URLError(.badURL) }
        let hadScheme = text.contains("://")
        if !hadScheme { text = "ws://\(text)" }
        guard var comps = URLComponents(string: text) else { throw URLError(.badURL) }
        let inputScheme = (comps.scheme ?? "ws").lowercased()
        comps.scheme = (inputScheme == "https" || inputScheme == "wss") ? "wss" : "ws"
        if comps.port == nil && (!hadScheme || comps.scheme == "ws") { comps.port = 8080 }
        if comps.path.isEmpty { comps.path = "/" }
        guard let url = comps.url else { throw URLError(.badURL) }
        return url
    }

    private func startSending() {
        if sending { return }
        senderToken += 1
        let token = senderToken
        sending = true
        sendQueue.async { [weak self] in
            guard let self else { return }
            if self.activeClientMode == .touch { self.startPhoneSensors() }
            while self.sending && self.controlClientActive && self.senderToken == token {
                self.collectPhysicalMotionSamplesIfNeeded()
                self.sendFrame()
                Thread.sleep(forTimeInterval: 0.004)
            }
            DispatchQueue.main.async { [weak self] in
                guard let self, self.senderToken == token else { return }
                self.sending = false
                self.stopPhoneSensors()
                if self.activeClientMode != .physical { self.clearPhysicalControllers() }
            }
        }
    }

    private func sendFrame() {
        guard let socket = webSocket else { return }
        let touchActive = controlClientActive && activeClientMode == .touch && currentPage == .touchControls
        let physicalActive = controlClientActive && activeClientMode == .physical && currentPage == .mainMenu
        let flags = touchActive ? ProtocolWire.flagSinglePad : 0
        var frame = ProtocolWire.initFrame(flags: flags, seq: nextSeq(), timestampUs: UInt64(Date().timeIntervalSince1970 * 1_000_000.0))

        if touchActive {
            let now = uptimeMs()
            let hid: [UInt8]
            if now - lastTouchFrameMs <= 500 {
                hid = touchHid ?? (touchFrame.flatMap { ProtocolWire.extractPad0Hid(from: $0) }) ?? ProtocolWire.neutralHid()
            } else {
                hid = ProtocolWire.neutralHid()
            }
            ProtocolWire.setFrameHid(&frame, pad: 0, hid: hid)
            if let samples = phoneMotionSamples() {
                ProtocolWire.setFrameMotionSamples(&frame, pad: 0, samples: samples)
            }
        } else if physicalActive {
            physicalPads.withLock { pads in
                for i in 0..<ProtocolWire.padCount {
                    let pad = pads[i]
                    guard pad.present else { continue }
                    ProtocolWire.setFrameHid(&frame, pad: i, hid: pad.hid())
                    if pad.hasMotion && pad.motionSampleCount >= ProtocolWire.motionSampleCount {
                        ProtocolWire.setFrameMotionSamples(&frame, pad: i, samples: pad.motionSamples)
                    }
                }
            }
        }

        socket.send(.data(Data(frame))) { [weak self] error in
            guard let self, error != nil else { return }
            DispatchQueue.main.async { [weak self] in
                self?.statusLabel.text = "Input sender failed"
                self?.deactivateControlClient()
            }
        }
    }

    private func sendResetFrame(to socket: URLSessionWebSocketTask) {
        let frame = ProtocolWire.initFrame(flags: ProtocolWire.flagReset, seq: nextSeq(), timestampUs: UInt64(Date().timeIntervalSince1970 * 1_000_000.0))
        socket.send(.data(Data(frame)), completionHandler: { _ in })
    }

    private func nextSeq() -> UInt32 {
        let out = seq
        seq &+= 1
        return out
    }

    private func startPhoneSensors() {
        guard !phoneSensorsActive else { return }
        phoneMotion.withLock { state in
            state.count = 0
            state.samples = Array(repeating: ProtocolWire.neutralMotion(), count: ProtocolWire.motionSampleCount)
        }
        guard motionManager.isDeviceMotionAvailable else { return }
        motionManager.deviceMotionUpdateInterval = 1.0 / 200.0
        motionManager.startDeviceMotionUpdates(to: motionQueue) { [weak self] motion, _ in
            guard let self, let motion else { return }
            self.pushPhoneMotionSample(motion)
        }
        phoneSensorsActive = true
    }

    private func stopPhoneSensors() {
        guard phoneSensorsActive else { return }
        motionManager.stopDeviceMotionUpdates()
        phoneSensorsActive = false
        phoneMotion.withLock { state in state.count = 0 }
    }

    private func pushPhoneMotionSample(_ motion: CMDeviceMotion) {
        let orientation = currentOrientation
        let g0 = motion.gravity
        let r0 = motion.rotationRate
        let g = remapForInterfaceOrientation(x: Float(g0.x), y: Float(g0.y), z: Float(g0.z), orientation: orientation)
        let r = remapForInterfaceOrientation(x: Float(r0.x), y: Float(r0.y), z: Float(r0.z), orientation: orientation)
        let sample = ProtocolWire.motionFromApple(gravityX: g.0, gravityY: g.1, gravityZ: g.2,
                                                  rotationX: r.0, rotationY: r.1, rotationZ: r.2)
        phoneMotion.withLock { state in
            state.samples[0] = state.samples[1]
            state.samples[1] = state.samples[2]
            state.samples[2] = sample
            if state.count < ProtocolWire.motionSampleCount { state.count += 1 }
        }
    }

    private func phoneMotionSamples() -> [[UInt8]]? {
        phoneMotion.withLock { state in
            guard state.count >= ProtocolWire.motionSampleCount else { return nil }
            return state.samples
        }
    }

    private func remapForInterfaceOrientation(x: Float, y: Float, z: Float, orientation: UIInterfaceOrientation) -> (Float, Float, Float) {
        switch orientation {
        case .landscapeLeft:
            // Match Android Surface.ROTATION_90 behavior: (-Y, +X, +Z).
            return (-y, x, z)
        case .landscapeRight:
            // Match Android Surface.ROTATION_270 behavior: (+Y, -X, +Z).
            return (y, -x, z)
        case .portraitUpsideDown:
            return (-x, -y, z)
        default:
            return (x, y, z)
        }
    }

    private func setupControllerObservers() {
        NotificationCenter.default.addObserver(self, selector: #selector(controllerChanged), name: .GCControllerDidConnect, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(controllerChanged), name: .GCControllerDidDisconnect, object: nil)
        GCController.startWirelessControllerDiscovery(completionHandler: nil)
    }

    @objc private func controllerChanged() {
        if activeClientMode == .physical {
            scanPhysicalControllers()
            updatePhysicalStatusOnPage()
        }
    }

    private func scanPhysicalControllers() {
        let controllers = Array(GCController.controllers().prefix(ProtocolWire.padCount))

        physicalPads.withLock { pads in
            controllerSlots.removeAll()
            for pad in pads { pad.reset() }

            for (slot, controller) in controllers.enumerated() {
                let pad = pads[slot]
                pad.controller = controller
                pad.name = controller.vendorName ?? "Controller \(slot + 1)"
                pad.present = true
                pad.hasGyro = controller.motion != nil
                pad.hasRumble = controller.haptics != nil
                controllerSlots[ObjectIdentifier(controller)] = slot
            }
        }

        for controller in controllers {
            configureController(controller)
        }
    }

    private func clearPhysicalControllers() {
        controllerSlots.removeAll()
        physicalPads.withLock { pads in
            for pad in pads { pad.reset() }
        }
    }

    private func configureController(_ controller: GCController) {
        controller.extendedGamepad?.valueChangedHandler = { [weak self, weak controller] gamepad, _ in
            guard let self, let controller else { return }
            self.updateGamepadState(controller: controller, gamepad: gamepad)
        }
        if let gamepad = controller.extendedGamepad {
            updateGamepadState(controller: controller, gamepad: gamepad)
        }
    }

    private func updateGamepadState(controller: GCController, gamepad: GCExtendedGamepad) {
        let id = ObjectIdentifier(controller)
        physicalPads.withLock { pads in
            guard let slot = controllerSlots[id], slot >= 0, slot < pads.count else { return }
            let pad = pads[slot]
            var buttons = 0
            // Physical-position mapping, same as SDL/Android: south->Switch B, east->A, west->Y, north->X.
            if gamepad.buttonA.isPressed { buttons |= ProtocolWire.btnB }
            if gamepad.buttonB.isPressed { buttons |= ProtocolWire.btnA }
            if gamepad.buttonX.isPressed { buttons |= ProtocolWire.btnY }
            if gamepad.buttonY.isPressed { buttons |= ProtocolWire.btnX }
            if gamepad.leftShoulder.isPressed { buttons |= ProtocolWire.btnL }
            if gamepad.rightShoulder.isPressed { buttons |= ProtocolWire.btnR }
            if gamepad.leftTrigger.value > 0.5 { buttons |= ProtocolWire.btnZL }
            if gamepad.rightTrigger.value > 0.5 { buttons |= ProtocolWire.btnZR }
            if gamepad.buttonOptions?.isPressed == true { buttons |= ProtocolWire.btnMinus }
            if gamepad.buttonMenu.isPressed { buttons |= ProtocolWire.btnPlus }
            if gamepad.buttonHome?.isPressed == true { buttons |= ProtocolWire.btnHome }
            if #available(iOS 12.1, *) {
                if gamepad.leftThumbstickButton?.isPressed == true { buttons |= ProtocolWire.btnLStick }
                if gamepad.rightThumbstickButton?.isPressed == true { buttons |= ProtocolWire.btnRStick }
            }
            pad.buttons = buttons
            pad.dpadUp = gamepad.dpad.up.isPressed
            pad.dpadDown = gamepad.dpad.down.isPressed
            pad.dpadLeft = gamepad.dpad.left.isPressed
            pad.dpadRight = gamepad.dpad.right.isPressed
            pad.lx = ProtocolWire.axisToByte(gamepad.leftThumbstick.xAxis.value)
            pad.ly = ProtocolWire.axisToByte(-gamepad.leftThumbstick.yAxis.value)
            pad.rx = ProtocolWire.axisToByte(gamepad.rightThumbstick.xAxis.value)
            pad.ry = ProtocolWire.axisToByte(-gamepad.rightThumbstick.yAxis.value)
        }
    }

    private func collectPhysicalMotionSamplesIfNeeded() {
        guard activeClientMode == .physical else { return }
        physicalPads.withLock { pads in
            for pad in pads {
                guard pad.present, let motion = pad.controller?.motion else { continue }
                let g = motion.gravity
                let r = motion.rotationRate
                // Physical controller gyro stays in controller space. Do not apply screen-orientation remap here.
                let sample = ProtocolWire.motionFromApple(gravityX: Float(g.x), gravityY: Float(g.y), gravityZ: Float(g.z),
                                                          rotationX: Float(r.x), rotationY: Float(r.y), rotationZ: Float(r.z))
                pad.motionSamples[0] = pad.motionSamples[1]
                pad.motionSamples[1] = pad.motionSamples[2]
                pad.motionSamples[2] = sample
                if pad.motionSampleCount < ProtocolWire.motionSampleCount { pad.motionSampleCount += 1 }
                pad.hasMotion = pad.motionSampleCount >= ProtocolWire.motionSampleCount
            }
        }
    }

    private func updatePhysicalStatusOnPage(prefix: String? = nil) {
        guard currentPage == .mainMenu else { return }
        let lines = physicalPads.withLock { pads in
            (0..<ProtocolWire.padCount).map { i -> String in
                let pad = pads[i]
                if !pad.present { return "P\(i + 1): Empty" }
                return "P\(i + 1): \(pad.name)\(pad.hasGyro ? " +gyro" : "")"
            }
        }
        let status: String
        if let prefix { status = prefix }
        else {
            switch activeClientMode {
            case .physical: status = "Connected"
            case .touch: status = "Touch Controls running"
            case .none: status = "Ready"
            }
        }
        let connectText = (activeClientMode == .physical && controlClientActive) ? "Disconnect" : "Connect"
        var js = "(function(){"
        js += "var s=document.getElementById('statusText'); if(s)s.textContent='\(jsEscape(status))';"
        js += "var b=document.getElementById('btnConnect'); if(b)b.textContent='\(jsEscape(connectText))';"
        for i in 0..<ProtocolWire.padCount {
            js += "var p=document.getElementById('p\(i + 1)Text'); if(p)p.textContent='\(jsEscape(lines[i]))';"
        }
        js += "})()"
        webView.evaluateJavaScript(js, completionHandler: nil)
    }

    private func handleRumblePacket(_ data: Data) {
        let bytes = [UInt8](data)
        guard bytes.count == ProtocolWire.rumblePacketSize || bytes.count == ProtocolWire.precisionRumblePacketSize else { return }
        let magic = readU32LE(bytes, 0)
        guard magic == ProtocolWire.rumbleMagic || magic == ProtocolWire.precisionRumbleMagic else { return }
        let subpad = Int(bytes[4])
        let low = Int(bytes[5])
        let high = Int(bytes[6])
        let duration10Ms = Int(bytes[7])
        routeRumble(subpad: subpad, low: low, high: high, duration10Ms: duration10Ms)
    }

    private func routeRumble(subpad: Int, low: Int, high: Int, duration10Ms: Int) {
        guard controlClientActive else { return }
        switch activeClientMode {
        case .physical:
            physicalRumble(subpad: subpad, low: low, high: high, duration10Ms: duration10Ms)
        case .touch, .none:
            break
        }
    }

    private func physicalRumble(subpad: Int, low: Int, high: Int, duration10Ms: Int) {
        guard subpad >= 0 && subpad < ProtocolWire.padCount else { return }
        let now = uptimeMs()
        let neutral = (low == 0 && high == 0) || duration10Ms == 0
        physicalPads.withLock { pads in
            let pad = pads[subpad]
            guard pad.present else { return }
            if neutral {
                pad.rumbleLow = 0
                pad.rumbleHigh = 0
                pad.rumbleUntilMs = 0
                pad.rumbleLastSetMs = now
                pad.stopRumble()
                return
            }
            let duration = UInt64(max(250, min(max(duration10Ms, 1), 255) * 10))
            if pad.rumbleLow == low && pad.rumbleHigh == high && now - pad.rumbleLastSetMs < 100 {
                pad.rumbleUntilMs = now + duration
                return
            }
            pad.rumbleLow = low
            pad.rumbleHigh = high
            pad.rumbleUntilMs = now + duration
            pad.rumbleLastSetMs = now
            playControllerHaptic(pad: pad, low: low, high: high, durationMs: duration)
        }
    }

    private func playControllerHaptic(pad: PhysicalPad, low: Int, high: Int, durationMs: UInt64) {
        guard let haptics = pad.controller?.haptics else { return }
        do {
            pad.stopRumble()
            guard let engine = try? haptics.createEngine(withLocality: .default) else { return }
            try engine.start()
            let strength = Float(max(low, high)) / 255.0
            let intensity = CHHapticEventParameter(parameterID: .hapticIntensity, value: max(0.05, strength))
            let sharpness = CHHapticEventParameter(parameterID: .hapticSharpness, value: min(1.0, max(0.2, Float(high) / 255.0)))
            let event = CHHapticEvent(eventType: .hapticContinuous, parameters: [intensity, sharpness], relativeTime: 0, duration: Double(durationMs) / 1000.0)
            let pattern = try CHHapticPattern(events: [event], parameters: [])
            let player = try engine.makeAdvancedPlayer(with: pattern)
            try player.start(atTime: CHHapticTimeImmediate)
            pad.hapticEngine = engine
            pad.hapticPlayer = player
        } catch {
            // Many iOS controllers expose input but no controller haptics. Ignore quietly.
        }
    }

    private func stopAllPhysicalRumble() {
        physicalPads.withLock { pads in pads.forEach { $0.stopRumble() } }
    }

    private func readU32LE(_ bytes: [UInt8], _ off: Int) -> UInt32 {
        guard off + 3 < bytes.count else { return 0 }
        return UInt32(bytes[off]) |
            (UInt32(bytes[off + 1]) << 8) |
            (UInt32(bytes[off + 2]) << 16) |
            (UInt32(bytes[off + 3]) << 24)
    }

    private func uptimeMs() -> UInt64 {
        UInt64(ProcessInfo.processInfo.systemUptime * 1000.0)
    }

    private func jsEscape(_ v: String) -> String {
        v.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "'", with: "\\'")
            .replacingOccurrences(of: "\n", with: " ")
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
        webView.configuration.userContentController.removeScriptMessageHandler(forName: "NSBridge")
    }
}
