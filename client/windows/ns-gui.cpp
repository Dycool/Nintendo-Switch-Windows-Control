#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <mmsystem.h>
#include <windowsx.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <xinput.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <dbt.h>

#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <memory>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ── Protocol ──
namespace ns {
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;
static constexpr uint8_t  PROTO_VERSION = 4;
static constexpr uint16_t DEFAULT_PORT  = 7331;
static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
static constexpr size_t HMAC_TAG_SIZE = 16;

enum Button : uint16_t {
    BTN_Y=1<<0, BTN_B=1<<1, BTN_A=1<<2, BTN_X=1<<3,
    BTN_L=1<<4, BTN_R=1<<5, BTN_ZL=1<<6, BTN_ZR=1<<7,
    BTN_MINUS=1<<8, BTN_PLUS=1<<9, BTN_LSTICK=1<<10, BTN_RSTICK=1<<11,
    BTN_HOME=1<<12, BTN_CAPTURE=1<<13,
};
enum Hat : uint8_t { HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3, HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8 };
enum Flags : uint8_t { FLAG_NONE=0x00, FLAG_RESET=0x01, FLAG_AUTOFIRE=0x02 };
#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0; uint8_t hat = HAT_NEUTRAL;
    uint8_t lx=128, ly=128, rx=128, ry=128, vendor=0;
    void reset() noexcept { *this = HIDReport{}; }
};
struct MultiReport {
    HIDReport p1, p2, p3, p4;
    void reset() noexcept { p1.reset(); p2.reset(); p3.reset(); p4.reset(); }
};
struct Packet {
    uint32_t magic; uint8_t version; uint8_t flags; uint16_t autofire_mask;
    uint32_t seq; uint64_t ts_us; MultiReport report; uint8_t hmac[HMAC_TAG_SIZE];
};
#pragma pack(pop)
static constexpr size_t PACKET_SIZE = sizeof(Packet);
static constexpr size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
}

#include "../../server/rpi/include/sha256.h"

// ── Throttled XInput polling (4-Player) ──
static uint64_t g_last_check_us[4] = {0, 0, 0, 0};
static bool     g_is_connected[4]  = {false, false, false, false};

static uint8_t apply_deadzone(SHORT val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    else                 scaled = 128 - ((abs(val) - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

static ns::HIDReport map_xinput_to_switch(const XINPUT_GAMEPAD& pad) {
    ns::HIDReport r; r.reset();
    if (pad.wButtons & XINPUT_GAMEPAD_A) r.buttons |= ns::BTN_B;
    if (pad.wButtons & XINPUT_GAMEPAD_B) r.buttons |= ns::BTN_A;
    if (pad.wButtons & XINPUT_GAMEPAD_X) r.buttons |= ns::BTN_Y;
    if (pad.wButtons & XINPUT_GAMEPAD_Y) r.buttons |= ns::BTN_X;
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  r.buttons |= ns::BTN_L;
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) r.buttons |= ns::BTN_R;
    if (pad.bLeftTrigger > 128)  r.buttons |= ns::BTN_ZL;
    if (pad.bRightTrigger > 128) r.buttons |= ns::BTN_ZR;
    if (pad.wButtons & XINPUT_GAMEPAD_BACK)  r.buttons |= ns::BTN_MINUS;
    if (pad.wButtons & XINPUT_GAMEPAD_START) r.buttons |= ns::BTN_PLUS;
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB)  r.buttons |= ns::BTN_LSTICK;
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) r.buttons |= ns::BTN_RSTICK;
    if ((pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) && (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)) {
        r.buttons |= ns::BTN_HOME; r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
    }
    if ((pad.wButtons & XINPUT_GAMEPAD_BACK) && (pad.wButtons & XINPUT_GAMEPAD_START)) {
        r.buttons |= ns::BTN_CAPTURE; r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);
    }
    bool up = (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP), down = (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
    bool left = (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT), right = (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);
    if (up && right) r.hat = ns::HAT_NE; else if (up && left) r.hat = ns::HAT_NW;
    else if (down && right) r.hat = ns::HAT_SE; else if (down && left) r.hat = ns::HAT_SW;
    else if (up) r.hat = ns::HAT_N; else if (down) r.hat = ns::HAT_S;
    else if (left) r.hat = ns::HAT_W; else if (right) r.hat = ns::HAT_E;
    r.lx = apply_deadzone(pad.sThumbLX, false); r.ly = apply_deadzone(pad.sThumbLY, true);
    r.rx = apply_deadzone(pad.sThumbRX, false); r.ry = apply_deadzone(pad.sThumbRY, true);
    return r;
}

static void fetch_pad_throttled(DWORD index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    uint64_t now = ns::now_us();
    if (!g_is_connected[index] && (now - g_last_check_us[index] < 1'000'000)) {
        conn = false; return;
    }
    XINPUT_STATE state; ZeroMemory(&state, sizeof(XINPUT_STATE));
    if (XInputGetState(index, &state) != ERROR_SUCCESS) {
        g_is_connected[index] = false;
        g_last_check_us[index] = now;
        conn = false; return;
    }
    g_is_connected[index] = true; conn = true;
    rep = map_xinput_to_switch(state.Gamepad);
}

// ── Global UI state ──
static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;
static HWND g_hIpEdit = nullptr;
static HWND g_hConnectBtn = nullptr;
static HWND g_hStatusText = nullptr;
static HWND g_hP1Text = nullptr;
static HWND g_hP2Text = nullptr;
static HWND g_hP3Text = nullptr;
static HWND g_hP4Text = nullptr;

static std::atomic<bool> g_senderRunning{false};
static std::atomic<bool> g_connected{false};
static std::thread g_senderThread;
static SOCKET g_sock = INVALID_SOCKET;
static uint8_t g_hmacKey[32]{};
static uint32_t g_packetCount = 0;
static std::string g_targetHost;
static uint16_t g_targetPort = ns::DEFAULT_PORT;

// ── Registry helpers ──
static const wchar_t* REG_KEY = L"Software\\NSPCControl";
static const wchar_t* REG_VAL_IP = L"LastIP";

static std::wstring LoadSavedIP() {
    HKEY hKey = nullptr;
    std::wstring ip;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[64]{};
        DWORD len = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueEx(hKey, REG_VAL_IP, nullptr, &type, (LPBYTE)buf, &len) == ERROR_SUCCESS && type == REG_SZ)
            ip = buf;
        RegCloseKey(hKey);
    }
    return ip;
}

static void SaveLastIP(const wchar_t* ip) {
    HKEY hKey = nullptr;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueEx(hKey, REG_VAL_IP, 0, REG_SZ, (const BYTE*)ip, (DWORD)((wcslen(ip) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

// ── Control IDs ──
enum { IDC_IP = 101, IDC_CONNECT };

// ── Create a modern button with theme support ──
static HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id) {
    HWND hw = CreateWindow(L"BUTTON", text, WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    HFONT hBtnFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessage(hw, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
    return hw;
}

// ── Sender thread (4-Player) ──
static void SenderThread() {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) return;

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char portBuf[8];
    snprintf(portBuf, sizeof(portBuf), "%u", g_targetPort);
    if (getaddrinfo(g_targetHost.c_str(), portBuf, &hints, &res) != 0 || !res) {
        closesocket(sock);
        return;
    }
    sockaddr_in dest{};
    memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    g_sock = sock;
    g_senderRunning = true;
    uint32_t seq = 0;

    while (g_senderRunning) {
        ns::Packet pkt{}; memset(&pkt, 0, sizeof(ns::Packet));
        pkt.magic   = ns::PROTO_MAGIC;
        pkt.version = ns::PROTO_VERSION;
        pkt.flags   = ns::FLAG_NONE;
        pkt.seq     = seq++;
        pkt.ts_us   = ns::now_us();

        bool c1, c2, c3, c4;
        fetch_pad_throttled(0, pkt.report.p1, c1);
        fetch_pad_throttled(1, pkt.report.p2, c2);
        fetch_pad_throttled(2, pkt.report.p3, c3);
        fetch_pad_throttled(3, pkt.report.p4, c4);

        bool any_connected = (c1 || c2 || c3 || c4);
        if (!any_connected) pkt.report.reset();

        {
            uint8_t fullHmac[32];
            hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, fullHmac);
            memcpy(pkt.hmac, fullHmac, ns::HMAC_TAG_SIZE);
        }

        sendto(sock, (const char*)&pkt, (int)ns::PACKET_SIZE, 0, (const sockaddr*)&dest, sizeof(dest));
        g_packetCount++;

        if (any_connected) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        else std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    closesocket(sock);
    g_sock = INVALID_SOCKET;
}

// ── Update P1-P4 status display ──
static void UpdateControllerStatus() {
    wchar_t buf[64];
    HWND hText[4] = { g_hP1Text, g_hP2Text, g_hP3Text, g_hP4Text };
    for (DWORD i = 0; i < 4; i++) {
        XINPUT_CAPABILITIES caps{};
        bool present = (XInputGetCapabilities(i, 0, &caps) == ERROR_SUCCESS);
        if (g_connected) {
            swprintf(buf, 64, L"P%d: %s", i + 1, g_is_connected[i] ? L"Connected" : L"Idle");
        } else {
            swprintf(buf, 64, L"P%d: %s", i + 1, present ? L"Available" : L"Not connected");
        }
        SetWindowText(hText[i], buf);
    }
}

// ── Connect ──
static void DoConnect(HWND hWnd) {
    if (g_connected) return;

    wchar_t ipBuf[64]{};
    GetWindowText(g_hIpEdit, ipBuf, 64);

    if (ipBuf[0] == 0) {
        MessageBox(hWnd, L"Please enter a Raspberry Pi IP address.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    int port = ns::DEFAULT_PORT;
    wchar_t* colon = wcschr(ipBuf, L':');
    if (colon) {
        *colon = L'\0';
        port = (int)_wtoi(colon + 1);
        if (port <= 0 || port > 65535) port = ns::DEFAULT_PORT;
    }

    derive_key(ns::DEFAULT_SECRET, g_hmacKey);

    SaveLastIP(ipBuf);

    char hostA[64]{};
    WideCharToMultiByte(CP_UTF8, 0, ipBuf, -1, hostA, sizeof(hostA), nullptr, nullptr);
    g_targetHost = hostA;
    g_targetPort = (uint16_t)port;
    g_packetCount = 0;

    for (int i = 0; i < 4; i++) {
        g_is_connected[i] = false;
        g_last_check_us[i] = 0;
    }

    g_connected = true;

    if (g_senderThread.joinable()) g_senderThread.join();
    g_senderThread = std::thread(SenderThread);

    SetWindowText(g_hConnectBtn, L"Disconnect");
    EnableWindow(g_hIpEdit, FALSE);

    std::wstring status = L"Connected to " + std::wstring(ipBuf) + L":" + std::to_wstring(port);
    SetWindowText(g_hStatusText, status.c_str());
}

// ── Disconnect ──
static void DoDisconnect() {
    if (!g_connected) return;
    g_connected = false;
    g_senderRunning = false;
    if (g_senderThread.joinable()) g_senderThread.join();

    SetWindowText(g_hConnectBtn, L"Connect");
    EnableWindow(g_hIpEdit, TRUE);
    SetWindowText(g_hStatusText, L"Disconnected");
    SetWindowText(g_hP1Text, L"");
    SetWindowText(g_hP2Text, L"");
    SetWindowText(g_hP3Text, L"");
    SetWindowText(g_hP4Text, L"");
    UpdateControllerStatus();
}

// ── Theme colors (white background) ──
static const COLORREF ACCENT_RED  = RGB(0xCC, 0x00, 0x00);
static const COLORREF TEXT_BLACK  = RGB(0x1A, 0x1A, 0x1A);
static const COLORREF GRAY_LINE   = RGB(0xDD, 0xDD, 0xDD);

// ── Window procedure ──
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Enable visual styles
            INITCOMMONCONTROLSEX icc{ sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES };
            InitCommonControlsEx(&icc);

            int x = 16, y = 12;

            // ── Title ──
            HFONT hTitleFont = CreateFont(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HWND hTitle = CreateWindow(L"STATIC", L"NS PC Control",
                WS_VISIBLE | WS_CHILD, x, y, 380, 30, hWnd, nullptr, g_hInst, nullptr);
            SendMessage(hTitle, WM_SETFONT, (WPARAM)hTitleFont, TRUE);
            y += 40;

            HFONT hLabelFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT hFieldFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");

            auto makeLabel = [&](const wchar_t* text, int x, int y, int w, int h) {
                HWND hw = CreateWindow(L"STATIC", text, WS_VISIBLE | WS_CHILD | SS_RIGHT,
                    x, y, w, h, hWnd, nullptr, g_hInst, nullptr);
                SendMessage(hw, WM_SETFONT, (WPARAM)hLabelFont, TRUE);
                return hw;
            };
            auto makeEdit = [&](int id, int x, int y, int w, int h, const wchar_t* def) {
                HWND hw = CreateWindow(L"EDIT", def, WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
                    x, y, w, h, hWnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
                SendMessage(hw, WM_SETFONT, (WPARAM)hFieldFont, TRUE);
                return hw;
            };
            // ── IP row ──
            makeLabel(L"Raspberry Pi IP:", x, y + 4, 110, 22);
            std::wstring savedIp = LoadSavedIP();
            if (savedIp.empty()) savedIp = L"192.168.1.100";
            g_hIpEdit = makeEdit(IDC_IP, x + 115, y, 265, 24, savedIp.c_str());
            y += 36;

            // ── Connect / Quit buttons ──
            g_hConnectBtn = CreateButton(hWnd, L"Connect", x + 115, y, 100, 30, IDC_CONNECT);
            CreateButton(hWnd, L"Quit", x + 285, y, 100, 30, 1002);
            y += 42;

            // ── Separator ──
            CreateWindow(L"STATIC", nullptr, WS_VISIBLE | WS_CHILD | SS_OWNERDRAW,
                x, y, 370, 2, hWnd, (HMENU)1003, g_hInst, nullptr);
            y += 14;

            // ── Status ──
            HFONT hStatusFont = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            auto makeStatus = [&](const wchar_t* text, int y) {
                HWND hw = CreateWindow(L"STATIC", text, WS_VISIBLE | WS_CHILD,
                    x + 4, y, 360, 20, hWnd, nullptr, g_hInst, nullptr);
                SendMessage(hw, WM_SETFONT, (WPARAM)hStatusFont, TRUE);
                return hw;
            };

            g_hStatusText = makeStatus(L"Ready", y);
            y += 22;

            g_hP1Text = makeStatus(L"", y); y += 18;
            g_hP2Text = makeStatus(L"", y); y += 18;
            g_hP3Text = makeStatus(L"", y); y += 18;
            g_hP4Text = makeStatus(L"", y);

            UpdateControllerStatus();
            EnableWindow(g_hConnectBtn, TRUE);
            break;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_CONNECT) {
                if (!g_connected) DoConnect(hWnd);
                else DoDisconnect();
            } else if (id == 1002) {
                PostMessage(hWnd, WM_CLOSE, 0, 0);
            }
            break;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
            if (dis->CtlID == 1003) {
                HBRUSH hLine = CreateSolidBrush(GRAY_LINE);
                FillRect(dis->hDC, &dis->rcItem, hLine);
                DeleteObject(hLine);
                return TRUE;
            }
            break;
        }

        case WM_TIMER: {
            if (g_connected) UpdateControllerStatus();
            break;
        }

        case WM_DEVICECHANGE: {
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
                UpdateControllerStatus();
            }
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            HWND hCtrl = (HWND)lParam;
            SetTextColor(hdc, TEXT_BLACK);
            SetBkMode(hdc, TRANSPARENT);
            // Title label gets red accent
            wchar_t buf[64];
            GetWindowText(hCtrl, buf, 64);
            if (wcscmp(buf, L"NS PC Control") == 0) {
                SetTextColor(hdc, ACCENT_RED);
            }
            static HBRUSH hWhiteBrush = []{ return CreateSolidBrush(RGB(255,255,255)); }();
            return (LRESULT)hWhiteBrush;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, TEXT_BLACK);
            SetBkColor(hdc, RGB(255,255,255));
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }

        case WM_CLOSE:
            DoDisconnect();
            DestroyWindow(hWnd);
            break;

        case WM_DESTROY:
            DoDisconnect();
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ── Entry point ──
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    timeBeginPeriod(1);
    g_hInst = hInst;

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Load icon from embedded resource (ID 1 from ns-gui.rc)
    HICON hIcon = LoadIcon(hInst, MAKEINTRESOURCE(1));

    const wchar_t CLASS_NAME[] = L"NSGamepadWindow";
    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hIcon = hIcon;
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);

    RECT rc{0, 0, 410, 280};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);

    HWND hWnd = CreateWindowEx(0, CLASS_NAME, L"NS PC Control",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    if (!hWnd) return 1;

    g_hWnd = hWnd;
    // Set the icon for the title bar and taskbar
    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    SetTimer(hWnd, 1, 100, nullptr);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DoDisconnect();
    WSACleanup();
    timeEndPeriod(1); return 0;
}
