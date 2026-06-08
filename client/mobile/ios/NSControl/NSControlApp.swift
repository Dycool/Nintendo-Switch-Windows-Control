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
                            Text(page == .mainMenu ? "NS Control" : page.rawValue.capitalized)
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
            Image(systemName: "gamecontroller.fill").font(.system(size: 64)).foregroundColor(.red)
            Text("NS Control").font(.largeTitle).bold()
            Text("Connect to your Raspberry Pi server").foregroundColor(.secondary)

            TextField("Server IP (e.g. 192.168.1.100)", text: $host)
                .textFieldStyle(.roundedBorder)
                .keyboardType(.numbersAndPunctuation)
                .disableAutocorrection(true)
                .onChange(of: host) { UserDefaults.standard.set($1, forKey: "host") }

            Button("Connect") {
                host = host.trimmingCharacters(in: .whitespaces)
                guard !host.isEmpty else { return }
                BridgeManager.shared.connect(host: host)
                connected = true
                page = .mainMenu
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
        let target: String = {
            switch page {
            case .mainMenu:      return "http://\(host):8080/"
            case .touchControls: return "http://\(host):8080/mobile"
            case .editor:        return "http://\(host):8080/editor"
            }
        }()
        guard wv.url?.absoluteString != target else { return }
        wv.load(URLRequest(url: URL(string: target)!))
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
            case "back":
                DispatchQueue.main.async { self.parent.page = .mainMenu }
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
                if url.path == "/mobile" {
                    DispatchQueue.main.async { self.parent.page = .touchControls }
                    decisionHandler(.cancel); return
                }
                if url.path == "/editor" {
                    DispatchQueue.main.async { self.parent.page = .editor }
                    decisionHandler(.cancel); return
                }
            }
            decisionHandler(.allow)
        }

        func webView(_ wv: WKWebView, didFinish _: WKNavigation!) {
            guard parent.page == .touchControls || parent.page == .editor else { return }
            wv.evaluateJavaScript(Self.backButtonJS)
        }

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
