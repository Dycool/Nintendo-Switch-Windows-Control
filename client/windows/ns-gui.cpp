#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE

#include <windows.h>
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
static constexpr uint8_t  PROTO_VERSION = 1;
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
#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0; uint8_t hat = HAT_NEUTRAL;
    uint8_t lx=128, ly=128, rx=128, ry=128, vendor=0;
    void reset() noexcept { *this = HIDReport{}; }
};
enum Flags : uint8_t { FLAG_NONE=0x00, FLAG_RESET=0x01, FLAG_AUTOFIRE=0x02 };
struct Packet {
    uint32_t magic; uint8_t version; uint8_t flags; uint16_t autofire_mask;
    uint32_t seq; uint64_t ts_us; HIDReport report; uint8_t hmac[HMAC_TAG_SIZE];
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

// ── Controller info ──
struct ControllerEntry {
    int id;
    std::wstring name;
    DWORD xinputSlot;
    bool connected;
};

// ── Scan controllers via XInput ──
static std::vector<ControllerEntry> ScanControllers() {
    std::vector<ControllerEntry> entries;
    int nextId = 0;

    for (DWORD i = 0; i < 4; i++) {
        XINPUT_CAPABILITIES caps{};
        if (XInputGetCapabilities(i, 0, &caps) != ERROR_SUCCESS) continue;

        std::wstring name = L"Controller " + std::to_wstring(i + 1);
        // Try to get a friendly name from HID enumeration
        HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_HIDCLASS, nullptr, nullptr, DIGCF_PRESENT);
        if (hDevInfo != INVALID_HANDLE_VALUE) {
            int xinputIdx = 0;
            for (DWORD j = 0; ; j++) {
                SP_DEVINFO_DATA devInfo{ sizeof(SP_DEVINFO_DATA) };
                if (!SetupDiEnumDeviceInfo(hDevInfo, j, &devInfo)) break;
                BYTE hwIdBuf[512]{};
                if (!SetupDiGetDeviceRegistryProperty(hDevInfo, &devInfo, SPDRP_HARDWAREID,
                    nullptr, hwIdBuf, sizeof(hwIdBuf), nullptr))
                    continue;
                std::string hwId((const char*)hwIdBuf);
                if (hwId.find("IG_") == std::string::npos && hwId.find("VID_045E") == std::string::npos)
                    continue;
                if (xinputIdx++ != (int)i) continue;
                WCHAR friendlyName[256]{};
                if (SetupDiGetDeviceRegistryProperty(hDevInfo, &devInfo, SPDRP_FRIENDLYNAME,
                    nullptr, (PBYTE)friendlyName, sizeof(friendlyName), nullptr)) {
                    name = friendlyName;
                }
                break;
            }
            SetupDiDestroyDeviceInfoList(hDevInfo);
        }

        ControllerEntry entry;
        entry.id = nextId++;
        entry.name = name;
        entry.xinputSlot = i;
        entry.connected = true;
        entries.push_back(entry);
    }
    return entries;
}

// ── Abstract controller reader ──
class ControllerReader {
public:
    virtual ~ControllerReader() = default;
    virtual bool Read(ns::HIDReport& report) = 0;
    virtual std::wstring GetName() = 0;
};

// ── XInput reader ──
class XInputReader : public ControllerReader {
    DWORD slot;
    std::wstring name;
public:
    XInputReader(DWORD slot, const std::wstring& name) : slot(slot), name(name) {}
    bool Read(ns::HIDReport& report) override {
        XINPUT_STATE state{};
        if (XInputGetState(slot, &state) != ERROR_SUCCESS)
            return false;

        report.reset();
        auto& pad = state.Gamepad;
        if (pad.wButtons & XINPUT_GAMEPAD_A) report.buttons |= ns::BTN_B;
        if (pad.wButtons & XINPUT_GAMEPAD_B) report.buttons |= ns::BTN_A;
        if (pad.wButtons & XINPUT_GAMEPAD_X) report.buttons |= ns::BTN_Y;
        if (pad.wButtons & XINPUT_GAMEPAD_Y) report.buttons |= ns::BTN_X;
        if (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  report.buttons |= ns::BTN_L;
        if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) report.buttons |= ns::BTN_R;
        if (pad.bLeftTrigger > 128)  report.buttons |= ns::BTN_ZL;
        if (pad.bRightTrigger > 128) report.buttons |= ns::BTN_ZR;
        if (pad.wButtons & XINPUT_GAMEPAD_BACK)  report.buttons |= ns::BTN_MINUS;
        if (pad.wButtons & XINPUT_GAMEPAD_START) report.buttons |= ns::BTN_PLUS;
        if (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB)  report.buttons |= ns::BTN_LSTICK;
        if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) report.buttons |= ns::BTN_RSTICK;
        if ((pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) && (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)) {
            report.buttons |= ns::BTN_HOME;
            report.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
        }
        if ((pad.wButtons & XINPUT_GAMEPAD_BACK) && (pad.wButtons & XINPUT_GAMEPAD_START)) {
            report.buttons |= ns::BTN_CAPTURE;
            report.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);
        }
        bool up = (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP);
        bool down = (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
        bool left = (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
        bool right = (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);
        if (up && right)       report.hat = ns::HAT_NE;
        else if (up && left)   report.hat = ns::HAT_NW;
        else if (down && right)report.hat = ns::HAT_SE;
        else if (down && left) report.hat = ns::HAT_SW;
        else if (up)           report.hat = ns::HAT_N;
        else if (down)         report.hat = ns::HAT_S;
        else if (left)         report.hat = ns::HAT_W;
        else if (right)        report.hat = ns::HAT_E;

        auto apply = [](SHORT val, bool inv, int dz = 8000) -> uint8_t {
            if (val > -dz && val < dz) return 128;
            int s;
            if (val >= dz) s = 128 + ((val - dz) * 127) / (32767 - dz);
            else s = 128 - ((abs(val) - dz) * 128) / (32768 - dz);
            s = std::clamp(s, 0, 255);
            return (uint8_t)(inv ? (255 - s) : s);
        };
        report.lx = apply(pad.sThumbLX, false);
        report.ly = apply(pad.sThumbLY, true);
        report.rx = apply(pad.sThumbRX, false);
        report.ry = apply(pad.sThumbRY, true);
        return true;
    }
    std::wstring GetName() override { return name; }
};

// ── Global UI state ──
static HINSTANCE g_hInst = nullptr;
static HWND g_hWnd = nullptr;
static HWND g_hIpEdit = nullptr;
static HWND g_hCtrlCombo = nullptr;
static HWND g_hConnectBtn = nullptr;
static HWND g_hStatusText = nullptr;
static HWND g_hCtrlNameText = nullptr;

static HWND g_hRefreshBtn = nullptr;

static std::atomic<bool> g_senderRunning{false};
static std::atomic<bool> g_connected{false};
static std::thread g_senderThread;
static SOCKET g_sock = INVALID_SOCKET;
static uint8_t g_hmacKey[32]{};
static uint32_t g_packetCount = 0;
static std::string g_targetHost;
static uint16_t g_targetPort = ns::DEFAULT_PORT;
static std::vector<ControllerEntry> g_controllers;
static std::unique_ptr<ControllerReader> g_reader;

// ── Registry helpers ──
static const wchar_t* REG_KEY = L"Software\\NintendoSwitchPCControl";
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
enum { IDC_IP = 101, IDC_CTRL, IDC_CONNECT, IDC_REFRESH };

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

// ── Sender thread ──
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
        ns::HIDReport report;
        if (g_reader && g_reader->Read(report)) {
            ns::Packet pkt{};
            pkt.magic = ns::PROTO_MAGIC;
            pkt.version = ns::PROTO_VERSION;
            pkt.flags = ns::FLAG_NONE;
            pkt.seq = seq++;
            pkt.ts_us = ns::now_us();
            pkt.report = report;
            {
                uint8_t fullHmac[32];
                hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, fullHmac);
                memcpy(pkt.hmac, fullHmac, ns::HMAC_TAG_SIZE);
            }
            sendto(sock, (const char*)&pkt, (int)ns::PACKET_SIZE, 0, (const sockaddr*)&dest, sizeof(dest));
            g_packetCount++;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } else {
            // Send neutral
            ns::Packet pkt{};
            pkt.magic = ns::PROTO_MAGIC;
            pkt.version = ns::PROTO_VERSION;
            pkt.flags = ns::FLAG_NONE;
            pkt.seq = seq++;
            pkt.ts_us = ns::now_us();
            pkt.report.reset();
            {
                uint8_t fullHmac[32];
                hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, fullHmac);
                memcpy(pkt.hmac, fullHmac, ns::HMAC_TAG_SIZE);
            }
            sendto(sock, (const char*)&pkt, (int)ns::PACKET_SIZE, 0, (const sockaddr*)&dest, sizeof(dest));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    closesocket(sock);
    g_sock = INVALID_SOCKET;
}

// ── Refresh controller list ──
static void RefreshControllerList() {
    int prevSel = ComboBox_GetCurSel(g_hCtrlCombo);
    DWORD prevSlot = (prevSel >= 0) ? (DWORD)ComboBox_GetItemData(g_hCtrlCombo, prevSel) : (DWORD)-1;

    g_controllers = ScanControllers();
    ComboBox_ResetContent(g_hCtrlCombo);

    int newSel = -1;
    for (size_t i = 0; i < g_controllers.size(); i++) {
        int idx = (int)ComboBox_AddString(g_hCtrlCombo, g_controllers[i].name.c_str());
        ComboBox_SetItemData(g_hCtrlCombo, idx, g_controllers[i].id);
        if (g_controllers[i].id == (int)prevSlot)
            newSel = (int)i;
    }
    if (newSel >= 0)
        ComboBox_SetCurSel(g_hCtrlCombo, newSel);
    else if (g_controllers.size() > 0)
        ComboBox_SetCurSel(g_hCtrlCombo, 0);

    if (g_controllers.empty()) {
        SetWindowText(g_hCtrlNameText, L"No controllers detected");
        EnableWindow(g_hConnectBtn, FALSE);
    } else {
        SetWindowText(g_hCtrlNameText, L"");
        EnableWindow(g_hConnectBtn, TRUE);
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

    int sel = ComboBox_GetCurSel(g_hCtrlCombo);
    if (sel < 0) return;

    int controllerId = (int)ComboBox_GetItemData(g_hCtrlCombo, sel);
    // Find the selected controller
    const ControllerEntry* selected = nullptr;
    for (const auto& c : g_controllers) {
        if (c.id == controllerId) { selected = &c; break; }
    }
    if (!selected) return;

    // Create the XInput reader
    g_reader = std::make_unique<XInputReader>(selected->xinputSlot, selected->name);

    derive_key(ns::DEFAULT_SECRET, g_hmacKey);

    SaveLastIP(ipBuf);

    char hostA[64]{};
    WideCharToMultiByte(CP_UTF8, 0, ipBuf, -1, hostA, sizeof(hostA), nullptr, nullptr);
    g_targetHost = hostA;
    g_targetPort = (uint16_t)port;
    g_packetCount = 0;
    g_connected = true;

    if (g_senderThread.joinable()) g_senderThread.join();
    g_senderThread = std::thread(SenderThread);

    SetWindowText(g_hConnectBtn, L"Disconnect");
    EnableWindow(g_hIpEdit, FALSE);
    EnableWindow(g_hCtrlCombo, FALSE);
    EnableWindow(g_hRefreshBtn, FALSE);

    std::wstring status = L"Connected to " + std::wstring(ipBuf) + L":" + std::to_wstring(port);
    SetWindowText(g_hStatusText, status.c_str());
}

// ── Disconnect ──
static void DoDisconnect() {
    if (!g_connected) return;
    g_connected = false;
    g_senderRunning = false;
    if (g_senderThread.joinable()) g_senderThread.join();
    g_reader.reset();

    SetWindowText(g_hConnectBtn, L"Connect");
    EnableWindow(g_hIpEdit, TRUE);
    EnableWindow(g_hCtrlCombo, TRUE);
    EnableWindow(g_hRefreshBtn, TRUE);
    SetWindowText(g_hStatusText, L"Disconnected");
    RefreshControllerList();
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
            HWND hTitle = CreateWindow(L"STATIC", L"Nintendo Switch PC Control",
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
            auto makeCombo = [&](int id, int x, int y, int w, int h) {
                HWND hw = CreateWindow(L"COMBOBOX", nullptr, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_HASSTRINGS,
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

            // ── Controller row ──
            makeLabel(L"Controller:", x, y + 4, 110, 22);
            g_hCtrlCombo = makeCombo(IDC_CTRL, x + 115, y, 210, 120);
            g_hRefreshBtn = CreateButton(hWnd, L"Refresh", x + 332, y, 52, 24, IDC_REFRESH);
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
            g_hCtrlNameText = makeStatus(L"No controllers detected", y);

            // Initial scan
            g_controllers = ScanControllers();
            for (const auto& c : g_controllers) {
                int idx = (int)ComboBox_AddString(g_hCtrlCombo, c.name.c_str());
                ComboBox_SetItemData(g_hCtrlCombo, idx, c.id);
            }
            if (!g_controllers.empty()) {
                ComboBox_SetCurSel(g_hCtrlCombo, 0);
                SetWindowText(g_hCtrlNameText, L"");
                EnableWindow(g_hConnectBtn, TRUE);
            } else {
                EnableWindow(g_hConnectBtn, FALSE);
            }

            if (g_controllers.empty())
                SetWindowText(g_hCtrlNameText, L"No controllers detected");

            // Refresh button callback
            break;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_CONNECT) {
                if (!g_connected) DoConnect(hWnd);
                else DoDisconnect();
            } else if (id == 1002) {
                PostMessage(hWnd, WM_CLOSE, 0, 0);
            } else if (id == IDC_REFRESH && !g_connected) {
                RefreshControllerList();
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
            if (g_connected && g_reader)
                SetWindowText(g_hCtrlNameText, g_reader->GetName().c_str());
            break;
        }

        case WM_DEVICECHANGE: {
            if ((wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) && !g_connected) {
                RefreshControllerList();
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
            if (wcscmp(buf, L"Nintendo Switch PC Control") == 0) {
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

    RECT rc{0, 0, 410, 230};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);

    HWND hWnd = CreateWindowEx(0, CLASS_NAME, L"Nintendo Switch PC Control",
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
    return 0;
}
