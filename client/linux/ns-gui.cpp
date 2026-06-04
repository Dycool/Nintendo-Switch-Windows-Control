/// ns-gui.cpp  —  Dear ImGui frontend for the Switch gamepad bridge
///
/// Single-thread, 250Hz game-loop architecture. No mutexes required.

#include <iostream>
#include <string>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <thread>

#include <SDL2/SDL.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/resource.h>

#include "../../server/rpi/include/sha256.h"
#include "../../server/rpi/include/protocol.hpp"

// ── Global State ──
static SDL_GameController* g_pads[4] = {nullptr, nullptr, nullptr, nullptr};
static std::string g_hw_names[4];

enum { KB_OFF = 0, KB_SINGLE = 1, KB_OVERRIDE = 2 };
static int g_keyboardMode = KB_OFF;
static char g_ipBuf[64] = "192.168.1.100";
static bool g_connected = false;
static int g_sock = -1;
static struct sockaddr_in g_dest{};
static uint8_t g_hmacKey[32]{};
static uint32_t g_seq = 0;

// ── Helpers ──
uint8_t apply_deadzone(int16_t val, bool invert = false, int deadzone = 8000) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled;
    if (val >= deadzone) scaled = 128 + ((val - deadzone) * 127) / (32767 - deadzone);
    else scaled = 128 - ((std::abs(val) - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

// ── Hardware Discovery ──
void scan_for_gamepads() {
    static uint64_t last_scan = 0;
    uint64_t now = ns::now_us();
    if (now - last_scan < 1'000'000) return;
    last_scan = now;

    int num = SDL_NumJoysticks();
    for (int i = 0; i < num; ++i) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool found = false;
        for (int p = 0; p < 4; ++p) {
            if (g_pads[p]) {
                SDL_Joystick* js = SDL_GameControllerGetJoystick(g_pads[p]);
                if (js && SDL_JoystickInstanceID(js) == id) { found = true; break; }
            }
        }
        if (found) continue;

        for (int p = 0; p < 4; ++p) {
            if (!g_pads[p]) {
                g_pads[p] = SDL_GameControllerOpen(i);
                if (g_pads[p]) {
                    const char* name = SDL_GameControllerName(g_pads[p]);
                    g_hw_names[p] = name ? name : "Unknown Pad";
                }
                break;
            }
        }
    }
}

// ── Pad Reading ──
void read_pad(int index, ns::HIDReport& rep, bool& conn) {
    rep.reset();
    SDL_GameController* pad = g_pads[index];
    if (!pad) { conn = false; return; }

    if (!SDL_GameControllerGetAttached(pad)) {
        SDL_GameControllerClose(pad);
        g_pads[index] = nullptr;
        g_hw_names[index] = "";
        conn = false;
        return;
    }

    conn = true;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) rep.buttons |= ns::BTN_B;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) rep.buttons |= ns::BTN_A;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) rep.buttons |= ns::BTN_Y;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) rep.buttons |= ns::BTN_X;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) rep.buttons |= ns::BTN_L;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) rep.buttons |= ns::BTN_R;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)   > 16000) rep.buttons |= ns::BTN_ZL;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)  > 16000) rep.buttons |= ns::BTN_ZR;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK))    rep.buttons |= ns::BTN_MINUS;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START))   rep.buttons |= ns::BTN_PLUS;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK))  rep.buttons |= ns::BTN_LSTICK;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) rep.buttons |= ns::BTN_RSTICK;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_GUIDE))   rep.buttons |= ns::BTN_HOME;

    // Analog sticks
    rep.lx = apply_deadzone(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX));
    rep.ly = apply_deadzone(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY));
    rep.rx = apply_deadzone(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX));
    rep.ry = apply_deadzone(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY));
}

// ── Network Setup ──
void ConnectUDP() {
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int port = ns::DEFAULT_PORT;
    std::string host(g_ipBuf);
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        port = atoi(host.c_str() + colon + 1);
        host.resize(colon);
    }
    
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);
    
    if (getaddrinfo(host.c_str(), port_str, &hints, &res) == 0) {
        memcpy(&g_dest, res->ai_addr, sizeof(g_dest));
        freeaddrinfo(res);
        derive_key(ns::DEFAULT_SECRET, g_hmacKey);
        g_connected = true;
        g_seq = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Main Entry Point (The Game Loop)
// ─────────────────────────────────────────────────────────────────────────────
int main(int, char**) {
    setpriority(PRIO_PROCESS, 0, -20);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return -1;

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow("NS PC Control", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 450, 320, window_flags);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    bool done = false;
    auto next_tick = std::chrono::steady_clock::now();

    while (!done) {
        std::this_thread::sleep_until(next_tick);
        next_tick += std::chrono::milliseconds(4);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) done = true;
        }

        scan_for_gamepads();
        ns::HIDReport reports[4];
        bool connected[4] = {};
        int active_count = 0;

        for (int i = 0; i < 4; ++i) {
            read_pad(i, reports[i], connected[i]);
            if (connected[i]) active_count++;
        }

        if (g_keyboardMode == KB_SINGLE) {
            int write_idx = 3;
            for (int read_idx = 2; read_idx >= 0; --read_idx) {
                if (connected[read_idx]) {
                    reports[write_idx] = reports[read_idx];
                    connected[write_idx] = true;
                    write_idx--;
                }
            }
            reports[0].reset();
            reports[0].lx = 128; reports[0].ly = 128; reports[0].rx = 128; reports[0].ry = 128;
            connected[0] = true;
            active_count = std::max(active_count, 1);
        }

        if (g_connected) {
            ns::Packet pkt; memset(&pkt, 0, sizeof(ns::Packet));
            pkt.magic = ns::PROTO_MAGIC; pkt.version = ns::PROTO_VERSION;
            pkt.seq = g_seq++; pkt.ts_us = ns::now_us();
            pkt.report.p1 = reports[0]; pkt.report.p2 = reports[1];
            pkt.report.p3 = reports[2]; pkt.report.p4 = reports[3];

            uint8_t full_hmac[32];
            hmac_sha256(g_hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, full_hmac);
            memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
            sendto(g_sock, (const char*)&pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&g_dest, sizeof(g_dest));
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        ImGui::Text("Raspberry Pi IP:");
        ImGui::SameLine();
        ImGui::InputText("##ip", g_ipBuf, IM_ARRAYSIZE(g_ipBuf));

        ImGui::Text("Keyboard Mode:");
        ImGui::SameLine();
        const char* kb_items[] = { "OFF", "ON (Single)", "ON (Override)" };
        ImGui::Combo("##kb", &g_keyboardMode, kb_items, IM_ARRAYSIZE(kb_items));

        ImGui::Separator();

        if (g_connected) {
            if (ImGui::Button("Disconnect", ImVec2(120, 30))) {
                g_connected = false;
                close(g_sock); g_sock = -1;
            }
            ImGui::SameLine(); ImGui::TextColored(ImVec4(0,1,0,1), "Connected & Streaming");
        } else {
            if (ImGui::Button("Connect", ImVec2(120, 30))) ConnectUDP();
            ImGui::SameLine(); ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1), "Disconnected");
        }

        ImGui::Separator();

        for (int i = 0; i < 4; i++) {
            std::string disp = "Waiting...";
            if (i == 0 && g_keyboardMode == KB_SINGLE) disp = "Keyboard";
            else if (connected[i]) disp = g_hw_names[i];
            ImGui::Text("P%d: %s", i + 1, disp.c_str());
        }

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        if (!g_connected && active_count == 0) std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }

    if (g_sock != -1) close(g_sock);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
