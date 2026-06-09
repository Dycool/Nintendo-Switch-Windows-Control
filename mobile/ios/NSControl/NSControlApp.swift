import SwiftUI
import WebKit

@main
struct NSControlApp: App {
    var body: some Scene {
        WindowGroup { ContentView() }
    }
}

struct ContentView: View {
    @State private var host = UserDefaults.standard.string(forKey: "host") ?? ""
    @State private var status = "Ready"
    @State private var page: Page = .mainMenu
    @State private var connected = false

    enum Page: String { case mainMenu, touchControls, editor }

    var body: some View {
        NavigationStack {
            if !connected {
                connectionView
            } else {
                WebViewContainer(page: $page, host: host)
                    .simultaneousGesture(
                        DragGesture(minimumDistance: 45)
                            .onEnded { value in
                                guard page != .mainMenu else { return }
                                if value.startLocation.x < 32 &&
                                   value.translation.width > 90 &&
                                   abs(value.translation.height) < 80 {
                                    BridgeManager.shared.disconnect()
                                    page = .mainMenu
                                }
                            }
                    )
                    .toolbar {
                        ToolbarItem(placement: .principal) {
                            Text(page == .mainMenu ? "" : page.rawValue.capitalized)
                                .font(.headline)
                        }
                        ToolbarItem(placement: .status) {
                            Text(status).font(.caption2).foregroundColor(.secondary)
                        }
                    }
                    .navigationBarBackButtonHidden(true)
            }
        }
        .animation(.easeInOut, value: connected)
        .animation(.easeInOut, value: page)
    }

    var connectionView: some View {
        VStack(spacing: 24) {
            Spacer()
            Image("AppLogo")
                .resizable()
                .scaledToFit()
                .frame(width: 96, height: 96)
            Text("NS Mobile").font(.largeTitle).bold()
            Text("Connect to your Raspberry Pi server").foregroundColor(.secondary)

            TextField("Server IP (e.g. 192.168.1.100)", text: $host)
                .textFieldStyle(.roundedBorder)
                .keyboardType(.URL)
                .textInputAutocapitalization(.never)
                .disableAutocorrection(true)
                .onChange(of: host) { newValue in UserDefaults.standard.set(newValue.trimmingCharacters(in: .whitespacesAndNewlines), forKey: "host") }


            Button("Connect") {
                host = host.trimmingCharacters(in: .whitespaces)
                guard !host.isEmpty else { return }
                BridgeManager.shared.disconnect()
                connected = true
                page = .mainMenu
                status = "Loaded"
            }
            .buttonStyle(.borderedProminent)
            .tint(.red)
            .controlSize(.large)
            .disabled(host.isEmpty)

            Text(status).foregroundColor(.secondary).font(.callout)
            Spacer()
        }
        .padding(32)
        .onAppear {
            BridgeManager.shared.onStatus = { s in DispatchQueue.main.async { status = s } }
        }
    }
}

struct WebViewContainer: UIViewRepresentable {
    @Binding var page: ContentView.Page
    let host: String

    func makeCoordinator() -> Coordinator { Coordinator(parent: self) }

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        let cc = config.userContentController
        cc.add(context.coordinator, name: "nsBridge")
        cc.addUserScript(WKUserScript(source: Self.bridgeJS,
                                       injectionTime: .atDocumentStart,
                                       forMainFrameOnly: false))
        let wv = WKWebView(frame: .zero, configuration: config)
        wv.navigationDelegate = context.coordinator
        // No visible Back button; use the platform back gesture where possible.
        wv.allowsBackForwardNavigationGestures = true
        wv.scrollView.contentInsetAdjustmentBehavior = .never
        wv.scrollView.bounces = false
        return wv
    }

    func updateUIView(_ wv: WKWebView, context: Context) {
        guard let target = Self.localURL(for: page) else { return }

        // Page transitions into Touch Controls or Editor must release all 4
        // controller slots first. Touch Controls later starts a fresh touch-only
        // session when its own Connect button creates the fake WebSocket.
        if context.coordinator.lastPage != page {
            if page == .touchControls || page == .editor {
                BridgeManager.shared.disconnect()
            }
            context.coordinator.lastPage = page
        }

        guard wv.url != target else { return }
        wv.loadFileURL(target, allowingReadAccessTo: target.deletingLastPathComponent())
    }

    private static func localURL(for page: ContentView.Page) -> URL? {
        let name: String
        switch page {
        case .mainMenu:      name = "index"
        case .touchControls: name = "mobile"
        case .editor:        name = "editor"
        }
        return Bundle.main.url(forResource: name, withExtension: "html", subdirectory: "WebApp")
    }

    static let bridgeJS = """
    window.NSBridge = {
        onHubStart: function(){ window.webkit.messageHandlers.nsBridge.postMessage({type:'hubStart'}); },
        onHubStop: function(){ window.webkit.messageHandlers.nsBridge.postMessage({type:'hubStop'}); },
        onHubRefresh: function(){ window.webkit.messageHandlers.nsBridge.postMessage({type:'hubRefresh'}); },
        onOpenTouch: function(){ window.webkit.messageHandlers.nsBridge.postMessage({type:'openTouch'}); },
        onOpenEditor: function(){ window.webkit.messageHandlers.nsBridge.postMessage({type:'openEditor'}); },
        onBack: function(){ window.webkit.messageHandlers.nsBridge.postMessage({type:'back'}); }
    };
    window.__bridge = {
        connect: function(url) {
            window.webkit.messageHandlers.nsBridge.postMessage({type:'connect',url:url});
        },
        send: function(data) {
            if (data instanceof ArrayBuffer) {
                window.webkit.messageHandlers.nsBridge.postMessage(
                    {type:'binary', data:Array.from(new Uint8Array(data))});
            }
        },
        close: function() {
            window.webkit.messageHandlers.nsBridge.postMessage({type:'close'});
        }
    };
    var OrigWS = window.WebSocket;
    window.WebSocket = function(url, protocols) {
        this.readyState = 0;
        this.binaryType = 'arraybuffer';
        this.onopen = null; this.onclose = null; this.onerror = null; this.onmessage = null;
        this.send = function(data) { window.__bridge.send(data); };
        this.close = function() { window.__bridge.close(); };
        window.__bridge.connect(url);
        setTimeout(function() {
            this.readyState = 1;
            if (this.onopen) this.onopen();
        }.bind(this), 0);
    };
    window.WebSocket.CONNECTING = 0;
    window.WebSocket.OPEN = 1;
    window.WebSocket.CLOSING = 2;
    window.WebSocket.CLOSED = 3;
    """

    // MARK: - Coordinator

    class Coordinator: NSObject, WKNavigationDelegate, WKScriptMessageHandler {
        let parent: WebViewContainer
        var lastPage: ContentView.Page? = nil
        init(parent: WebViewContainer) { self.parent = parent }

        private func intValue(_ value: Any?, _ fallback: Int) -> Int {
            if let n = value as? NSNumber { return n.intValue }
            if let i = value as? Int { return i }
            if let d = value as? Double { return Int(d) }
            return fallback
        }

        func userContentController(_: WKUserContentController, didReceive msg: WKScriptMessage) {
            guard msg.name == "nsBridge",
                  let dict = msg.body as? [String: Any],
                  let type = dict["type"] as? String else { return }
            switch type {
            case "connect":
                // Native-mobile v1 is touch-only. The main menu Connect button is ignored;
                // Touch Controls owns the only live backend session.
                guard self.parent.page == .touchControls else { return }
                BridgeManager.shared.connect(host: self.parent.host, mode: .touchControls)
            case "back":
                BridgeManager.shared.disconnect()
                DispatchQueue.main.async { self.parent.page = .mainMenu }
            case "close":
                BridgeManager.shared.disconnect()
            case "binary":
                guard let arr = dict["data"] as? [Int], arr.count >= NS_PROTOCOL_WEB_FRAME_SIZE else { return }
                // Legacy fallback: extract pad 0 bytes (24 bytes at offset 20 in the 116-byte frame).
                let padData = Data(arr[20..<44].map { UInt8($0) })
                BridgeManager.shared.bridgeTouchPad(padData)
            case "touchState":
                let buttons = UInt16(max(0, min(65535, intValue(dict["buttons"], 0))))
                let hat = UInt8(max(0, min(255, intValue(dict["hat"], Int(NS_HAT_NEUTRAL)))))
                let lx = UInt8(max(0, min(255, intValue(dict["lx"], 128))))
                let ly = UInt8(max(0, min(255, intValue(dict["ly"], 128))))
                let rx = UInt8(max(0, min(255, intValue(dict["rx"], 128))))
                let ry = UInt8(max(0, min(255, intValue(dict["ry"], 128))))
                BridgeManager.shared.bridgeTouchState(buttons: buttons, hat: hat, lx: lx, ly: ly, rx: rx, ry: ry)
            case "hubStart":
                guard self.parent.page == .mainMenu else { return }
                BridgeManager.shared.connect(host: self.parent.host, mode: .controllerHub)
            case "hubStop":
                BridgeManager.shared.disconnect()
            case "hubRefresh":
                // Discovery is handled by GameController; keep this as a harmless UI hook.
                break
            case "openTouch":
                BridgeManager.shared.disconnect()
                DispatchQueue.main.async { self.parent.page = .touchControls }
            case "openEditor":
                BridgeManager.shared.disconnect()
                DispatchQueue.main.async { self.parent.page = .editor }
            default:
                break
            }
        }

        func webView(_: WKWebView,
                     decidePolicyFor nav: WKNavigationAction,
                     decisionHandler: @escaping (WKNavigationActionPolicy) -> Void)
        {
            if let url = nav.request.url {
                let last = url.lastPathComponent.lowercased()
                if last == "mobile.html", self.parent.page != .touchControls {
                    BridgeManager.shared.disconnect()
                    DispatchQueue.main.async { self.parent.page = .touchControls }
                    decisionHandler(.cancel); return
                }
                if last == "editor.html", self.parent.page != .editor {
                    BridgeManager.shared.disconnect()
                    DispatchQueue.main.async { self.parent.page = .editor }
                    decisionHandler(.cancel); return
                }
            }
            decisionHandler(.allow)
        }

        func webView(_ wv: WKWebView, didFinish _: WKNavigation!) {
            // Touch Controls and Editor intentionally have no visible Back button.
            // Use the iOS back/edge gesture instead.
            if parent.page == .mainMenu {
                wv.evaluateJavaScript(Self.mobileMenuJS)
            }
        }

        static let mobileMenuJS = """
        (function(){
            var kb = document.getElementById('kbModeContainer');
            if (kb) kb.style.display = 'none';
            var bindings = document.getElementById('btnBindings');
            if (bindings) bindings.style.display = 'none';
            var macros = document.getElementById('btnMacros');
            if (macros) macros.style.display = 'none';
            var oldHub = document.getElementById('btnHubStart');
            if (oldHub) oldHub.remove();
            var oldStop = document.getElementById('btnHubStop');
            if (oldStop) oldStop.remove();
            var oldRefresh = document.getElementById('btnHubRefresh');
            if (oldRefresh) oldRefresh.remove();
            var connect = document.getElementById('btnConnect');
            if (connect) {
                connect.style.display = 'inline-block';
                connect.textContent = 'Connect';
                connect.dataset.nsHubRunning = '0';
                connect.onclick = function(ev){
                    if (ev) ev.preventDefault();
                    var running = connect.dataset.nsHubRunning === '1';
                    if (running) {
                        if (window.NSBridge && NSBridge.onHubStop) NSBridge.onHubStop();
                        connect.dataset.nsHubRunning = '0';
                        connect.textContent = 'Connect';
                    } else {
                        if (window.NSBridge && NSBridge.onHubStart) NSBridge.onHubStart();
                        connect.dataset.nsHubRunning = '1';
                        connect.textContent = 'Disconnect';
                    }
                    return false;
                };
            }
            var touch = document.getElementById('btnTouchControls');
            if (touch) {
                touch.style.display = 'inline-block';
                touch.onclick = function(ev){
                    if (ev) ev.preventDefault();
                    if (window.NSBridge && NSBridge.onOpenTouch) NSBridge.onOpenTouch();
                    else window.location.href='mobile.html';
                    return false;
                };
            }
            var editor = document.getElementById('btnEditor');
            if (editor) {
                editor.style.display = 'inline-block';
                editor.onclick = function(ev){
                    if (ev) ev.preventDefault();
                    if (window.NSBridge && NSBridge.onOpenEditor) NSBridge.onOpenEditor();
                    else window.location.href='editor.html';
                    return false;
                };
            }
        })();
        """

        static let brandingJS = """
        
        """
    }
}
