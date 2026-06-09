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
                    .toolbar {
                        ToolbarItem(placement: .principal) {
                            HStack(spacing: 6) {
                                if page == .mainMenu {
                                    Image("AppLogo").resizable().scaledToFit().frame(width: 22, height: 22)
                                }
                                Text(page == .mainMenu ? "NS-mobile" : page.rawValue.capitalized)
                                    .font(.headline)
                            }
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
            Text("NS-mobile").font(.largeTitle).bold()
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
        wv.allowsBackForwardNavigationGestures = false
        wv.scrollView.contentInsetAdjustmentBehavior = .never
        return wv
    }

    func updateUIView(_ wv: WKWebView, context: Context) {
        guard let target = Self.localURL(for: page) else { return }

        // Merely opening the Touch Controls page must not bind a console player.
        // The real native WebSocket starts only when the embedded touch UI presses
        // its Connect button and creates its WebSocket. Leaving touch controls
        // releases the player immediately.
        if page != .touchControls {
            BridgeManager.shared.disconnect()
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
        init(parent: WebViewContainer) { self.parent = parent }

        func userContentController(_: WKUserContentController, didReceive msg: WKScriptMessage) {
            guard msg.name == "nsBridge",
                  let dict = msg.body as? [String: Any],
                  let type = dict["type"] as? String else { return }
            switch type {
            case "connect":
                // Same behavior as the browser page: Touch Controls binds only
                // after its Connect button creates the WebSocket. Ignore WebSocket
                // creations from other bundled pages.
                guard self.parent.page == .touchControls else { return }
                BridgeManager.shared.connect(host: self.parent.host)
            case "back":
                BridgeManager.shared.disconnect()
                DispatchQueue.main.async { self.parent.page = .mainMenu }
            case "close":
                BridgeManager.shared.disconnect()
            case "binary":
                guard let arr = dict["data"] as? [Int], arr.count >= 44 else { return }
                // Extract pad 0 bytes (24 bytes at offset 20 in the 116-byte frame)
                let padData = Data(arr[20..<44].map { UInt8($0) })
                BridgeManager.shared.bridgeTouchPad(padData)
            default:
                break
            }
        }

        func webView(_: WKWebView,
                     decidePolicyFor nav: WKNavigationAction,
                     decisionHandler: @escaping (WKNavigationActionPolicy) -> Void)
        {
            if let url = nav.request.url, nav.targetFrame?.isMainFrame == true {
                let last = url.lastPathComponent.lowercased()
                if url.path == "/mobile" || last == "mobile.html" {
                    DispatchQueue.main.async { self.parent.page = .touchControls }
                    decisionHandler(.cancel); return
                }
                if url.path == "/editor" || last == "editor.html" {
                    BridgeManager.shared.disconnect()
                    DispatchQueue.main.async { self.parent.page = .editor }
                    decisionHandler(.cancel); return
                }
            }
            decisionHandler(.allow)
        }

        func webView(_ wv: WKWebView, didFinish _: WKNavigation!) {
            if parent.page == .touchControls || parent.page == .editor {
                wv.evaluateJavaScript(Self.backButtonJS)
            } else if parent.page == .mainMenu {
                wv.evaluateJavaScript(Self.brandingJS)
            }
        }

        static let brandingJS = """
        (function(){
            var old = document.getElementById('ns-mobile-brand');
            if (old) old.remove();
            var b = document.createElement('div');
            b.id = 'ns-mobile-brand';
            b.style.cssText = 'position:fixed;top:12px;left:12px;z-index:99998;display:flex;align-items:center;gap:8px;background:rgba(255,255,255,0.92);color:#111;padding:7px 12px;border-radius:18px;font:700 15px system-ui,-apple-system,Segoe UI,sans-serif;box-shadow:0 3px 16px rgba(0,0,0,0.18);pointer-events:none;';
            var img = document.createElement('img');
            img.src = 'data:image/png;base64,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';
            img.style.cssText = 'width:24px;height:24px;border-radius:6px;';
            var t = document.createElement('span');
            t.textContent = 'NS-mobile';
            b.appendChild(img);
            b.appendChild(t);
            document.body.appendChild(b);
        })();
        """

        static let backButtonJS = """
        (function(){
            var b = document.createElement('div');
            b.id = 'nb';
            b.innerHTML = '\\u2190 Back';
            b.style.cssText = 'position:fixed;top:12px;left:12px;z-index:99999;background:rgba(0,0,0,0.55);color:#fff;padding:8px 18px;border-radius:20px;font-size:15px;cursor:pointer;-webkit-user-select:none;';
            b.onclick = function(){ window.webkit.messageHandlers.nsBridge.postMessage({type:'back'}); };
            document.body.appendChild(b);
        })();
        """
    }
}
