#include "../../include/protocol.hpp"
#include "../../include/sha256.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <algorithm>
#include <vector>
#include <map>
#include <set>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <errno.h>

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

using namespace ns;
using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

// ── Global flags ──────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static bool g_verbose = false;

// HMAC authentication (key derived from DEFAULT_SECRET at startup)
static uint8_t  g_hmac_key[32];

// Per-IP rate limiting (token bucket, 1024-entry hash table to prevent collisions)
static constexpr uint64_t RATE_WINDOW_US = 1'000'000;  // 1 second
static constexpr uint32_t RATE_MAX_PKT   = 2000;       // max packets/sec per IP
static constexpr int      RATE_TABLE     = 1024;

struct RateSlot {
    uint32_t ip;           // IP in network byte order, 0 = empty
    uint32_t count;
    uint64_t window_start; // us
};
static RateSlot g_rate_table[RATE_TABLE];

// ── Multi-Client Session State ───────────────────────────────────────────────
static constexpr int MAX_CLIENTS    = 5; // 4 UDP + 1 web client
static constexpr int WEB_CLIENT_IDX = MAX_CLIENTS - 1; // Slot 4 reserved for web

struct ClientSession {
    bool        active = false;
    sockaddr_in addr{};
    uint64_t    last_rx_us = 0;
    uint32_t    expected_seq = 0;
    bool        first_pkt = true;
    MultiReport report{}; // The inputs coming from this specific PC
};

static std::mutex    g_mtx;
static ClientSession g_clients[MAX_CLIENTS];

// Diagnostics
static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};

// ── Signal ────────────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ── Smart Multiplexer HID Writer Thread ───────────────────────────────────────
static void writer_thread(int hz) {
    const auto tick = us(1'000'000 / hz);
    int fds[4] = {-1, -1, -1, -1};
    std::string devs[4] = {"/dev/hidg0", "/dev/hidg1", "/dev/hidg2", "/dev/hidg3"};
    bool was_connected = false;

    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[4];

    while (g_running.load(std::memory_order_relaxed)) {
        bool all_open = true;
        for(int i=0; i<4; ++i) {
            if (fds[i] < 0) {
                fds[i] = open(devs[i].c_str(), O_WRONLY);
                if (fds[i] < 0) all_open = false;
            }
        }

        if (!all_open) {
            std::this_thread::sleep_for(ms(500));
            continue;
        }
        
        if (g_verbose || !was_connected)
            std::puts("4x /dev/hidg* opened");
        was_connected = true;

        auto next = Clock::now() + tick;
        MultiReport prev{}; prev.p1.buttons = 0xFFFF;
        bool error_shown = false;

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now(); next = std::max(next + tick, now + tick);

            MultiReport r;
            r.reset();

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                uint64_t now_stamp = now_us();

                for (int c = 0; c < MAX_CLIENTS; ++c) {
                    if (g_clients[c].active && (now_stamp - g_clients[c].last_rx_us > WATCHDOG_MS * 1000ULL)) {
                        g_clients[c].active = false;
                        if (g_verbose) std::printf("PC/Web %d timed out.\n", c+1);
                    }
                }

                for (int h = 0; h < 4; ++h) {
                    if (hw_slots[h].client_idx != -1) {
                        if (!g_clients[hw_slots[h].client_idx].active) {
                            hw_slots[h].client_idx = -1;
                            hw_slots[h].sub_idx = -1;
                        }
                    }
                }

                for (int c = 0; c < MAX_CLIENTS; ++c) {
                    if (!g_clients[c].active) continue;
                    for (int s = 0; s < 4; ++s) {
                        bool mapped = false;
                        for (int h = 0; h < 4; ++h) {
                            if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                                mapped = true; break;
                            }
                        }
                        if (!mapped) {
                            for (int h = 0; h < 4; ++h) {
                                if (hw_slots[h].client_idx == -1) {
                                    hw_slots[h].client_idx = c;
                                    hw_slots[h].sub_idx = s;
                                    if (g_verbose) 
                                        std::printf("Map Slot %d Pad %d -> Port %d\n", c+1, s+1, h+1);
                                    break;
                                }
                            }
                        }
                    }
                }

                HIDReport* out_subs[4] = { &r.p1, &r.p2, &r.p3, &r.p4 };
                for (int h = 0; h < 4; ++h) {
                    if (hw_slots[h].client_idx != -1) {
                        int c = hw_slots[h].client_idx;
                        int s = hw_slots[h].sub_idx;
                        HIDReport* src_subs[4] = { &g_clients[c].report.p1, &g_clients[c].report.p2, 
                                                   &g_clients[c].report.p3, &g_clients[c].report.p4 };
                        *out_subs[h] = *src_subs[s];
                    }
                }
            }

            bool ok = true;
            if (r.p1 != prev.p1) { if(write(fds[0], &r.p1, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p2 != prev.p2) { if(write(fds[1], &r.p2, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p3 != prev.p3) { if(write(fds[2], &r.p3, 8) < 0 && errno != EAGAIN) ok = false; }
            if (r.p4 != prev.p4) { if(write(fds[3], &r.p4, 8) < 0 && errno != EAGAIN) ok = false; }

            if (!ok) {
                if (!error_shown) { std::puts("Switch disconnected — waiting..."); error_shown = true; }
                for(int i=0; i<4; ++i) { close(fds[i]); fds[i] = -1; }
                std::this_thread::sleep_for(ms(1000)); break;
            }
            prev = r;
            ++g_hid_writes;
        }
    }
    
    MultiReport neutral{}; neutral.reset();
    for(int i=0; i<4; ++i) { 
        if (fds[i] >= 0) { (void)write(fds[i], &neutral.p1, 8); close(fds[i]); }
    }
}

// ── Stats thread ──────────────────────────────────────────────────────────────
static void stats_thread() {
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(ms(5000));
        if (!g_verbose) continue;
        std::printf("pkts_rx=%-8llu  hid_writes=%-8llu\n",
            (unsigned long long)g_pkts_rx.load(),
            (unsigned long long)g_hid_writes.load());
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
static bool rate_allow(uint32_t ip) {
    uint64_t now = now_us();
    uint32_t idx = ip % RATE_TABLE;
    RateSlot &s = g_rate_table[idx];
    if (s.ip != ip) {
        s.ip = ip; s.count = 1; s.window_start = now; return true;
    }
    if (now - s.window_start > RATE_WINDOW_US) {
        s.count = 1; s.window_start = now; return true;
    }
    s.count++;
    return s.count <= RATE_MAX_PKT;
}

#ifdef USE_UPNP
// ── UPnP port forwarding ──
static bool g_upnp_active = false;
static UPNPUrls g_upnp_urls{};
static IGDdatas g_upnp_data{};
static char g_upnp_lan_addr[64]{};

static bool upnp_add_mapping(uint16_t port) {
    if (g_upnp_active) return false; 
    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
    if (!devlist) return false;
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
    freeUPNPDevlist(devlist);
    if (igd != 1 && igd != 2) return false;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    int r = UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype,
                                port_str, port_str, g_upnp_lan_addr, "ns-backend", "UDP", nullptr, "0");
    if (r != 0) { FreeUPNPUrls(&g_upnp_urls); return false; }
    g_upnp_active = true;
    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, external_ip) == 0) {
        std::printf("UPnP: UDP port %u forwarded. External IP: %s\n", port, external_ip);
    }
    return true;
}

static void upnp_remove_mapping(uint16_t port) {
    if (!g_upnp_active) return;
    char port_str[8]; snprintf(port_str, sizeof(port_str), "%u", port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str, "UDP", nullptr);
    std::puts("UPnP: port mapping removed");
    FreeUPNPUrls(&g_upnp_urls); g_upnp_active = false;
}
#else
static bool upnp_add_mapping(uint16_t) { return false; }
static void upnp_remove_mapping(uint16_t) {}
#endif

// ── Web Server (HTTP + WebSocket) ─────────────────────────────────────────────
static const char* WEB_PAGE = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NS PC Control</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f0f0f0;color:#333;display:flex;justify-content:center;padding:30px 20px}
.container{max-width:420px;width:100%;background:#fff;border-radius:8px;padding:24px 20px;box-shadow:0 2px 12px rgba(0,0,0,.15)}
h1{font-size:22px;font-weight:700;color:#c00;margin:0 0 16px 0;text-align:center}
.row{margin-bottom:10px;display:flex;align-items:center;gap:8px}
.row label{display:flex;align-items:center;gap:6px;font-size:14px;min-width:110px;text-align:right}
.row input[type=text], .row select{flex:1;padding:6px 10px;border:1px solid #ccc;border-radius:4px;font-size:14px;font-family:Consolas,monospace;background:#f9f9f9}
.row button{padding:6px 18px;border:none;border-radius:4px;cursor:pointer;font-size:14px;font-weight:600}
.btn-primary{background:#c00;color:#fff;width:100%;padding:10px}
.btn-primary.active{background:#2e7d32}
.btn-secondary{background:#e0e0e0;color:#333;padding:6px 14px;font-size:12px}
.btn-secondary:disabled{opacity:.4;cursor:default}
#status{font-size:13px;color:#666;text-align:center;padding:8px;background:#f5f5f5;border-radius:4px;display:block;margin-top:4px}
.player-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-top:6px}
.player{font-size:13px;padding:6px 10px;background:#f5f5f5;border-radius:4px;border-left:3px solid #ccc}
.player.active{border-left-color:#2e7d32;background:#e8f5e9}
.pkt-info{font-size:12px;color:#999;text-align:center;margin-top:8px}
.modal-overlay{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.4);justify-content:center;align-items:center;z-index:100}
.modal-box{background:#fff;border-radius:8px;padding:12px;box-shadow:0 4px 20px rgba(0,0,0,.3);max-height:90vh;overflow-y:auto;width:560px}
.modal-box h2{font-size:16px;margin:0 0 8px 0;color:#c00}
#edit-table{width:100%;border-collapse:collapse}
#edit-table td{padding:2px 3px;font-size:12px}
#edit-table td.el{text-align:center;font-weight:600;width:95px}
#edit-table td.ek{font-family:Consolas,monospace;background:#f5f5f5;padding:2px 6px;border-radius:3px;width:115px;text-align:center}
#edit-table td button{padding:2px 6px;font-size:11px;border:1px solid #bbb;border-radius:3px;background:#fff;cursor:pointer}
#edit-table td button:hover{background:#eee}
.modal-buttons{display:flex;justify-content:space-between;margin:8px 4px 0}
.modal-buttons .group{display:flex;flex-direction:column;gap:6px}
.modal-buttons button{padding:6px 20px;border:none;border-radius:4px;cursor:pointer;font-size:13px;font-weight:600}
</style>
</head>
<body>
<div class="container">
<h1>NS PC Control</h1>
<div class="row"><label>Server IP:</label><input type="text" id="ip" placeholder="Leave empty for auto"></div>
<div class="row"><label>Keyboard Mode:</label><select id="kb-mode"><option value="0">OFF</option><option value="1">ON (single)</option><option value="2">ON (override)</option></select><button id="bindings-btn" class="btn-secondary" disabled>Bindings...</button></div>
<div class="row"><button id="connect-btn" class="btn-primary">Connect</button></div>
<span id="status">Disconnected</span>
<div class="player-grid"><div class="player" id="p1">P1: Disconnected</div><div class="player" id="p2">P2: Disconnected</div><div class="player" id="p3">P3: Disconnected</div><div class="player" id="p4">P4: Disconnected</div></div>
<div class="pkt-info" id="pkt-info">Packets rx by Pi: 0</div>
</div>
<div class="modal-overlay" id="edit-modal" tabindex="0">
<div class="modal-box">
<h2>Edit Key Bindings</h2>
<table id="edit-table"><tbody></tbody></table>
<div class="modal-buttons">
<div class="group"><button onclick="saveEditor()" style="background:#c00;color:#fff;">Save</button><button onclick="closeEditor()" style="background:#e0e0e0;">Cancel</button></div>
<div class="group"><button onclick="startSetup()" style="background:#e0e0e0;">Setup</button><button onclick="resetEditor()" style="background:#e0e0e0;">Reset</button></div>
</div></div></div>
<script>
let ws=null, connected=false, kbMode=0, pollId=null;
let keys={}, kbdBindings={};
const DEF={Y:'KeyZ',B:'KeyX',A:'KeyC',X:'KeyV',L:'KeyQ',R:'KeyE',ZL:'Digit1',ZR:'Digit2',MINUS:'Digit3',PLUS:'Digit4',LSTICK:'ShiftLeft',RSTICK:'ShiftRight',HOME:'Home',CAPTURE:'PrintScreen',LSTICK_UP:'KeyW',LSTICK_DOWN:'KeyS',LSTICK_LEFT:'KeyA',LSTICK_RIGHT:'KeyD',RSTICK_UP:'KeyI',RSTICK_DOWN:'KeyK',RSTICK_LEFT:'KeyJ',RSTICK_RIGHT:'KeyL',DPAD_UP:'ArrowUp',DPAD_DOWN:'ArrowDown',DPAD_LEFT:'ArrowLeft',DPAD_RIGHT:'ArrowRight'};
Object.assign(kbdBindings,DEF);
try{
    let sip=localStorage.getItem('nsLastIP'); if (sip) document.getElementById('ip').value = sip;
    let skm=localStorage.getItem('nsKbMode'); if(skm){ kbMode=parseInt(skm); document.getElementById('kb-mode').value=skm; }
    let sb=localStorage.getItem('nsBindings'); if(sb) Object.assign(kbdBindings, JSON.parse(sb));
}catch(e){}
let editBindings={}, listeningIdx=-1, setupMode=false;
const BKEYS=['Y','B','A','X','L','R','ZL','ZR','MINUS','PLUS','LSTICK','RSTICK','HOME','CAPTURE','LSTICK_UP','LSTICK_DOWN','LSTICK_LEFT','LSTICK_RIGHT','RSTICK_UP','RSTICK_DOWN','RSTICK_LEFT','RSTICK_RIGHT','DPAD_UP','DPAD_DOWN','DPAD_LEFT','DPAD_RIGHT'];
function openEditor(){
    editBindings=JSON.parse(JSON.stringify(kbdBindings)); listeningIdx=-1; setupMode=false;
    const tb=document.getElementById('edit-table').tBodies[0]; tb.innerHTML='';
    let half = Math.ceil(BKEYS.length/2);
    for(let i=0;i<half;i++){
        let li=i, ri=i+half;
        let tr=document.createElement('tr');
        let r_html = ri<BKEYS.length ? `<td class="el">${BKEYS[ri]}</td><td class="ek" id="ek-${ri}">${editBindings[BKEYS[ri]]||''}</td><td><button class="eb" data-idx="${ri}">Change</button></td>` : `<td colspan="3"></td>`;
        tr.innerHTML=`<td class="el">${BKEYS[li]}</td><td class="ek" id="ek-${li}">${editBindings[BKEYS[li]]||''}</td><td><button class="eb" data-idx="${li}">Change</button></td>` + r_html;
        tb.appendChild(tr);
    }
    document.querySelectorAll('.eb').forEach(b=>{
        b.onclick=function(){setupMode=false;listeningIdx=parseInt(this.dataset.idx);document.getElementById('ek-'+listeningIdx).textContent='...';document.getElementById('edit-modal').focus();}
    });
    document.getElementById('edit-modal').style.display='flex'; document.getElementById('edit-modal').focus();
}
function closeEditor(){document.getElementById('edit-modal').style.display='none';listeningIdx=-1;setupMode=false;}
function saveEditor(){kbdBindings=JSON.parse(JSON.stringify(editBindings));try{localStorage.setItem('nsBindings',JSON.stringify(kbdBindings))}catch(e){}closeEditor()}
function resetEditor(){for(let i=0;i<BKEYS.length;i++){editBindings[BKEYS[i]]=DEF[BKEYS[i]];document.getElementById('ek-'+i).textContent=DEF[BKEYS[i]];}}
function startSetup(){setupMode=true;for(let i=0;i<BKEYS.length;i++){editBindings[BKEYS[i]]='';document.getElementById('ek-'+i).textContent=i===0?'...':'';} listeningIdx=0; document.getElementById('edit-modal').focus();}
window.addEventListener('keydown',function(e){
    if(listeningIdx<0||document.getElementById('edit-modal').style.display!=='flex')return;
    e.preventDefault(); const k=BKEYS[listeningIdx];
    if(e.code==='Escape'){
        editBindings[k]=''; document.getElementById('ek-'+listeningIdx).textContent='';
        if(setupMode){listeningIdx++;if(listeningIdx<BKEYS.length){document.getElementById('ek-'+listeningIdx).textContent='...';return;}}
        listeningIdx=-1;setupMode=false;return;
    }
    if(setupMode){let ab=false;for(let i=0;i<BKEYS.length;i++){if(i!==listeningIdx&&editBindings[BKEYS[i]]===e.code){ab=true;break;}}if(ab)return;}
    for(let i=0;i<BKEYS.length;i++){if(i!==listeningIdx&&editBindings[BKEYS[i]]===e.code){editBindings[BKEYS[i]]='';document.getElementById('ek-'+i).textContent='';}}
    editBindings[k]=e.code; document.getElementById('ek-'+listeningIdx).textContent=e.code;
    if(setupMode){listeningIdx++;if(listeningIdx<BKEYS.length){document.getElementById('ek-'+listeningIdx).textContent='...';return;}}
    listeningIdx=-1;setupMode=false;
});
function applyKB(rep, override){
    const k=(b)=>keys[kbdBindings[b]];
    if(k('Y'))rep.b|=1<<0;if(k('B'))rep.b|=1<<1;if(k('A'))rep.b|=1<<2;if(k('X'))rep.b|=1<<3;
    if(k('L'))rep.b|=1<<4;if(k('R'))rep.b|=1<<5;if(k('ZL'))rep.b|=1<<6;if(k('ZR'))rep.b|=1<<7;
    if(k('MINUS'))rep.b|=1<<8;if(k('PLUS'))rep.b|=1<<9;if(k('LSTICK'))rep.b|=1<<10;if(k('RSTICK'))rep.b|=1<<11;
    if(k('HOME'))rep.b|=1<<12;if(k('CAPTURE'))rep.b|=1<<13;
    let u=k('DPAD_UP'),d=k('DPAD_DOWN'),l=k('DPAD_LEFT'),r=k('DPAD_RIGHT');
    if(u&&r)rep.h=1;else if(u&&l)rep.h=7;else if(d&&r)rep.h=3;else if(d&&l)rep.h=5;else if(u)rep.h=0;else if(d)rep.h=4;else if(l)rep.h=6;else if(r)rep.h=2;
    let lsu=k('LSTICK_UP'),lsd=k('LSTICK_DOWN'),lsl=k('LSTICK_LEFT'),lsr=k('LSTICK_RIGHT');
    if(lsl&&!lsr)rep.lx=0;else if(lsr&&!lsl)rep.lx=255;else if(!override)rep.lx=128;
    if(lsu&&!lsd)rep.ly=0;else if(lsd&&!lsu)rep.ly=255;else if(!override)rep.ly=128;
    let rsu=k('RSTICK_UP'),rsd=k('RSTICK_DOWN'),rsl=k('RSTICK_LEFT'),rsr=k('RSTICK_RIGHT');
    if(rsl&&!rsr)rep.rx=0;else if(rsr&&!rsl)rep.rx=255;else if(!override)rep.rx=128;
    if(rsu&&!rsd)rep.ry=0;else if(rsd&&!rsu)rep.ry=255;else if(!override)rep.ry=128;
}
function sendR(){
    if(!ws||ws.readyState!==1||!connected)return;
    let pads=[null,null,null,null], gps=navigator.getGamepads?navigator.getGamepads():[];
    for(let i=0;i<gps.length;i++)if(gps[i]&&gps[i].connected)pads[i]=gps[i];
    let reps=[{b:0,h:8,lx:128,ly:128,rx:128,ry:128},{b:0,h:8,lx:128,ly:128,rx:128,ry:128},{b:0,h:8,lx:128,ly:128,rx:128,ry:128},{b:0,h:8,lx:128,ly:128,rx:128,ry:128}];
    let aPad=(p,r)=>{
        if(!p)return false; let b=0;
        if(p.buttons[0]?.pressed)b|=1<<1;if(p.buttons[1]?.pressed)b|=1<<2;if(p.buttons[2]?.pressed)b|=1<<0;if(p.buttons[3]?.pressed)b|=1<<3;
        if(p.buttons[4]?.pressed)b|=1<<4;if(p.buttons[5]?.pressed)b|=1<<5;if(p.buttons[6]?.pressed)b|=1<<6;if(p.buttons[7]?.pressed)b|=1<<7;
        if(p.buttons[8]?.pressed)b|=1<<8;if(p.buttons[9]?.pressed)b|=1<<9;if(p.buttons[10]?.pressed)b|=1<<10;if(p.buttons[11]?.pressed)b|=1<<11;if(p.buttons[16]?.pressed)b|=1<<12;
        let u=p.buttons[12]?.pressed,d=p.buttons[13]?.pressed,l=p.buttons[14]?.pressed,rt=p.buttons[15]?.pressed,h=8;
        if(u&&rt)h=1;else if(u&&l)h=7;else if(d&&rt)h=3;else if(d&&l)h=5;else if(u)h=0;else if(d)h=4;else if(l)h=6;else if(rt)h=2;
        r.b=b; r.h=h;
        r.lx=Math.round(((p.axes[0]??0)+1)*127.5);r.ly=Math.round(((p.axes[1]??0)+1)*127.5);
        r.rx=Math.round(((p.axes[2]??0)+1)*127.5);r.ry=Math.round(((p.axes[3]??0)+1)*127.5);
        return true;
    };
    let c1=aPad(pads[0],reps[0]),c2=aPad(pads[1],reps[1]),c3=aPad(pads[2],reps[2]),c4=aPad(pads[3],reps[3]);
    let ac=c1||c2||c3||c4;
    if(kbMode===1){
        if(c1){if(!c2){reps[1]=Object.assign({},reps[0]);c2=true;}else if(!c3){reps[2]=Object.assign({},reps[0]);c3=true;}else if(!c4){reps[3]=Object.assign({},reps[0]);c4=true;}}
        reps[0]={b:0,h:8,lx:128,ly:128,rx:128,ry:128}; applyKB(reps[0],false); ac=true;
    }else if(kbMode===2){applyKB(reps[0],true); ac=true;}
    const dv=new DataView(new ArrayBuffer(32));
    for(let i=0;i<4;i++){let o=i*8;dv.setUint16(o,reps[i].b,true);dv.setUint8(o+2,reps[i].h);dv.setUint8(o+3,reps[i].lx);dv.setUint8(o+4,reps[i].ly);dv.setUint8(o+5,reps[i].rx);dv.setUint8(o+6,reps[i].ry);dv.setUint8(o+7,0);}
    ws.send(dv.buffer);
}
function conn(url){
    console.log("Connecting to:", url);
    document.getElementById('status').textContent = "Connecting...";
    ws = new WebSocket(url, 'ns-protocol');
    ws.onopen = () => {
        console.log("WebSocket connected");
        document.getElementById('status').textContent = "Connected";
        connected = true;
        ui();
        if(pollId) clearInterval(pollId);
        pollId = setInterval(sendR, 4);
    };
    ws.onmessage = (e) => {
        try {
            const d = JSON.parse(e.data);
            if (d.type === 'status' && d.clients){
                for(let i=0;i<4;i++){
                    const el=document.getElementById(`p${i+1}`),c=d.clients[i];
                    el.textContent = c&&c.active ? `P${i+1}: Connected` : `P${i+1}: Idle`;
                    el.className = c&&c.active ? 'player active' : 'player';
                }
                if(d.pkts_rx!==undefined) document.getElementById('pkt-info').textContent = `Packets rx by Pi: ${d.pkts_rx}`;
            }
        } catch(err) { console.error("Parse error", err); }
    };
    ws.onclose = (e) => {
        console.log("WebSocket closed:", e.code, e.reason);
        document.getElementById('status').textContent = "Disconnected (Code "+e.code+")";
        connected = false;
        if(pollId) clearInterval(pollId);
        pollId = null;
        ui();
    };
    ws.onerror = (e) => { console.error("WebSocket error", e); };
}
function ui(){
    const b=document.getElementById('connect-btn');
    b.textContent=connected?'Disconnect':'Connect';
    b.className=connected?'btn-primary active':'btn-primary';
    document.getElementById('ip').disabled=connected;
    document.getElementById('kb-mode').disabled=connected;
    document.getElementById('bindings-btn').disabled=(connected||kbMode===0);
}
document.getElementById('connect-btn').onclick=()=>{
    if(!connected){
        let inputIp=document.getElementById('ip').value.trim();
        try{localStorage.setItem('nsLastIP',inputIp)}catch(e){}
        let wsProto = location.protocol === "https:" ? "wss://" : "ws://";
        let url = !inputIp ? `${wsProto}${location.host}/` : `${wsProto}${inputIp.includes(':')?inputIp:inputIp+':7331'}/`;
        conn(url);
    }else{ ws.close(); }
};
document.getElementById('kb-mode').onchange=e=>{
    kbMode=parseInt(e.target.value);
    try{localStorage.setItem('nsKbMode',kbMode)}catch(err){}
    const bb=document.getElementById('bindings-btn');
    bb.disabled=(kbMode===0);
    bb.onclick=kbMode!==0?openEditor:null;
};
if(kbMode!==0) document.getElementById('bindings-btn').onclick=openEditor;
window.addEventListener('keydown',e=>{
    if(kbMode===0||!connected||listeningIdx>=0) return;
    if(e.target.tagName==='INPUT'||e.target.tagName==='SELECT') return;
    keys[e.code]=true;
    if(!e.code.startsWith('F')) e.preventDefault();
});
window.addEventListener('keyup',e=>{
    if(kbMode===0) return;
    if(e.target.tagName==='INPUT'||e.target.tagName==='SELECT') return;
    keys[e.code]=false;
    if(!e.code.startsWith('F')) e.preventDefault();
});
window.addEventListener('beforeunload',()=>{ if(ws && ws.readyState===1) ws.close(); });
ui();
</script>
</body>
</html>)html";

// --- SHA1 for WebSocket handshake ---
struct SHA1Ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];

    static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void init() {
        state[0] = 0x67452301; state[1] = 0xEFCDAB89;
        state[2] = 0x98BADCFE; state[3] = 0x10325476; state[4] = 0xC3D2E1F0;
        count = 0;
    }

    void transform(const uint8_t* block) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)block[i*4] << 24 | (uint32_t)block[i*4+1] << 16 |
                   (uint32_t)block[i*4+2] << 8 | block[i*4+3];
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)       { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40)  { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
            else if (i < 60)  { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;          k = 0xCA62C1D6; }
            uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    }

    void update(const uint8_t* data, size_t len) {
        size_t idx = (size_t)(count & 63);
        count += len;
        size_t free = 64 - idx;
        if (len >= free) {
            memcpy(buffer + idx, data, free);
            transform(buffer);
            data += free; len -= free;
            while (len >= 64) { transform(data); data += 64; len -= 64; }
            idx = 0;
        }
        memcpy(buffer + idx, data, len);
    }

    void final(uint8_t* out) {
        size_t idx = (size_t)(count & 63);
        size_t pad = (idx < 56) ? (56 - idx) : (120 - idx);
        uint64_t bits = count * 8;
        uint8_t padding[128] = {0x80};
        update(padding, pad);
        uint8_t len_buf[8];
        for (int i = 0; i < 8; ++i) len_buf[i] = (uint8_t)(bits >> (56 - i*8));
        update(len_buf, 8);
        for (int i = 0; i < 5; ++i) {
            out[i*4]   = (uint8_t)(state[i] >> 24);
            out[i*4+1] = (uint8_t)(state[i] >> 16);
            out[i*4+2] = (uint8_t)(state[i] >> 8);
            out[i*4+3] = (uint8_t)(state[i]);
        }
    }
};

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64enc(const uint8_t* data, size_t len) {
    std::string r; r.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i+1 < len) v |= (uint32_t)data[i+1] << 8;
        if (i+2 < len) v |= data[i+2];
        r += B64[(v >> 18) & 0x3F];
        r += B64[(v >> 12) & 0x3F];
        r += (i+1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        r += (i+2 < len) ? B64[v & 0x3F] : '=';
    }
    return r;
}

static std::string ws_accept_key(const std::string& key) {
    std::string s = key + "258EAFA5-E914-47DA-95CA-5AB9DC11B85B";
    uint8_t hash[20];
    SHA1Ctx sha1;
    sha1.init();
    sha1.update((const uint8_t*)s.data(), s.size());
    sha1.final(hash);
    return b64enc(hash, 20);
}

// ── WebSocket frame helpers ───────────────────────────────────────────────────
static bool ws_send_frame(int fd, const void* data, size_t len, uint8_t opcode) {
    uint8_t hdr[14]; size_t hl;
    hdr[0] = 0x80 | opcode;
    if (len < 126) { hdr[1] = (uint8_t)len; hl = 2; }
    else if (len < 65536) {
        hdr[1] = 126; hdr[2] = (uint8_t)(len >> 8); hdr[3] = (uint8_t)len; hl = 4;
    } else {
        hdr[1] = 127;
        uint64_t n = (uint64_t)len;
        for (int i = 7; i >= 0; --i) { hdr[2+i] = (uint8_t)(n & 0xFF); n >>= 8; }
        hl = 10;
    }
    struct iovec iov[2] = {{hdr, hl}, {(void*)data, len}};
    struct msghdr msg = {}; msg.msg_iov = iov; msg.msg_iovlen = 2;
    return sendmsg(fd, &msg, MSG_NOSIGNAL) == (ssize_t)(hl + len);
}

static int ws_recv_frame(int fd, uint8_t* buf, size_t cap) {
    uint8_t hdr[2];
    ssize_t n = recv(fd, hdr, 2, MSG_WAITALL);
    if (n != 2) return -1;
    bool mask = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
        uint8_t ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2) return -1;
        len = (uint64_t)ext[0] << 8 | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (recv(fd, ext, 8, MSG_WAITALL) != 8) return -1;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
    }
    uint8_t mk[4] = {0};
    if (mask && recv(fd, mk, 4, MSG_WAITALL) != 4) return -1;
    if (len > cap) return -1;
    if (len > 0 && recv(fd, buf, len, MSG_WAITALL) != (ssize_t)len) return -1;
    if (mask) for (uint64_t i = 0; i < len; ++i) buf[i] ^= mk[i & 3];
    uint8_t opcode = hdr[0] & 0x0F;
    if (opcode == 0x8) return -2; // close
    if (opcode == 0x9) { ws_send_frame(fd, buf, len, 0xA); return 0; } // ping->pong
    if (opcode == 0xA) return 0; // pong
    return (int)len;
}

// ── Web Server Thread (fixed, using select) ───────────────────────────────────
static void webserver_thread(int port) {
    std::puts("WebSocket server starting...");
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return; }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return;
    }
    if (listen(listen_fd, 10) < 0) {
        perror("listen"); close(listen_fd); return;
    }
    std::printf("Web server listening on port %d\n", port);

    struct Client {
        int fd;
        bool websocket;
        std::string buffer;
    };
    std::vector<Client> clients;
    uint64_t last_status = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = listen_fd;
        FD_SET(listen_fd, &readfds);
        for (auto& c : clients) {
            FD_SET(c.fd, &readfds);
            if (c.fd > max_fd) max_fd = c.fd;
        }
        struct timeval tv = {0, 100000}; // 100ms
        int activity = select(max_fd + 1, &readfds, nullptr, nullptr, &tv);
        if (activity < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // Accept new connections
        if (FD_ISSET(listen_fd, &readfds)) {
            struct sockaddr_in ca;
            socklen_t sl = sizeof(ca);
            int cfd = accept(listen_fd, (struct sockaddr*)&ca, &sl);
            if (cfd >= 0) {
                clients.push_back({cfd, false, ""});
                if (g_verbose) std::printf("New client fd=%d from %s\n", cfd, inet_ntoa(ca.sin_addr));
            }
        }

        // Process existing clients
        std::vector<int> dead;
        for (size_t i = 0; i < clients.size(); ++i) {
            int fd = clients[i].fd;
            if (!FD_ISSET(fd, &readfds)) continue;

            if (clients[i].websocket) {
                uint8_t buf[4096];
                int r = ws_recv_frame(fd, buf, sizeof(buf));
                if (r == -2 || r == -1) {
                    dead.push_back(i);
                    continue;
                }
                if (r == 0) continue;
                if (g_verbose) std::printf("WS frame %d bytes from fd=%d\n", r, fd);
                if (r >= (int)sizeof(MultiReport)) {
                    MultiReport report;
                    memcpy(&report, buf, sizeof(MultiReport));
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_clients[WEB_CLIENT_IDX].active = true;
                    g_clients[WEB_CLIENT_IDX].report = report;
                    g_clients[WEB_CLIENT_IDX].last_rx_us = now_us();
                } else if (r >= (int)sizeof(HIDReport)) {
                    HIDReport report;
                    memcpy(&report, buf, sizeof(HIDReport));
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_clients[WEB_CLIENT_IDX].active = true;
                    g_clients[WEB_CLIENT_IDX].report.p1 = report;
                    g_clients[WEB_CLIENT_IDX].report.p2.reset();
                    g_clients[WEB_CLIENT_IDX].report.p3.reset();
                    g_clients[WEB_CLIENT_IDX].report.p4.reset();
                    g_clients[WEB_CLIENT_IDX].last_rx_us = now_us();
                }
            } else {
                char buf[8192];
                ssize_t n = recv(fd, buf, sizeof(buf)-1, 0);
                if (n <= 0) {
                    dead.push_back(i);
                    continue;
                }
                buf[n] = '\0';
                clients[i].buffer.append(buf, n);
                if (clients[i].buffer.find("\r\n\r\n") == std::string::npos)
                    continue; // wait for full headers

                std::string req = clients[i].buffer;
                std::string lower = req;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

                if (lower.find("upgrade: websocket") != std::string::npos) {
                    size_t key_pos = lower.find("sec-websocket-key:");
                    if (key_pos != std::string::npos) {
                        key_pos = req.find(':', key_pos) + 1;
                        while (key_pos < req.size() && req[key_pos] == ' ') key_pos++;
                        size_t end = req.find("\r\n", key_pos);
                        if (end != std::string::npos) {
                            std::string key = req.substr(key_pos, end - key_pos);
                            key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
                            std::string accept = ws_accept_key(key);
                            std::string response =
                                "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: " + accept + "\r\n"
                                "Sec-WebSocket-Protocol: ns-protocol\r\n\r\n";
                            if (send(fd, response.data(), response.size(), 0) == (ssize_t)response.size()) {
                                clients[i].websocket = true;
                                clients[i].buffer.clear();
                                std::puts("WebSocket handshake OK");
                            } else {
                                dead.push_back(i);
                            }
                            continue;
                        }
                    }
                    dead.push_back(i);
                } else if (req.find("GET ") == 0) {
                    std::string page(WEB_PAGE);
                    std::string response =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: " + std::to_string(page.size()) + "\r\n"
                        "Connection: close\r\n\r\n" + page;
                    send(fd, response.data(), response.size(), 0);
                    dead.push_back(i);
                } else {
                    dead.push_back(i);
                }
            }
        }

        // Remove dead clients
        std::sort(dead.begin(), dead.end(), std::greater<int>());
        for (int idx : dead) {
            if (idx >= 0 && idx < (int)clients.size()) {
                if (clients[idx].websocket) {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    g_clients[WEB_CLIENT_IDX].active = false;
                }
                close(clients[idx].fd);
                clients.erase(clients.begin() + idx);
            }
        }

        // Broadcast status to WebSocket clients
        uint64_t now = now_us();
        if (now - last_status > 100000) {
            last_status = now;
            std::string json;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                json = "{\"type\":\"status\",\"clients\":[";
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (i) json += ",";
                    json += "{\"active\":" + std::string(g_clients[i].active ? "true" : "false") + "}";
                }
                json += "],\"pkts_rx\":" + std::to_string(g_pkts_rx.load()) + "}";
            }
            for (auto& c : clients) {
                if (c.websocket) ws_send_frame(c.fd, json.data(), json.size(), 1);
            }
        }
    }

    for (auto& c : clients) close(c.fd);
    close(listen_fd);
    std::puts("WebSocket server stopped");
}

// ── UDP receive loop (main thread) ────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
    std::string bind_addr = "0.0.0.0";
    bool        do_upnp   = false;
    bool        do_web    = false;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if      (a == "-p" && i+1 < argc) port      = (uint16_t)std::atoi(argv[++i]);
        else if (a == "-b" && i+1 < argc) bind_addr = argv[++i];
        else if (a == "-v")               g_verbose  = true;
        else if (a == "-w")               do_web     = true;
        else if (a == "--upnp")           do_upnp    = true;
        else if (a == "-h") {
            puts("ns-backend  [-p PORT] [-b ADDR] [-w] [--upnp] [-v]");
            return 0;
        }
    }

    derive_key(DEFAULT_SECRET, g_hmac_key);
    signal(SIGINT,  on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (do_upnp) upnp_add_mapping(port);

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int rbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }
    std::printf("UDP %s:%u writer=%d Hz\n", bind_addr.c_str(), port, WRITER_HZ);

    std::thread wt(writer_thread, WRITER_HZ);
    std::thread st(stats_thread);
    std::thread web;
    if (do_web) web = std::thread(webserver_thread, port);

    int ep = epoll_create1(0);
    epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = sock;
    epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);
    Packet pkt{};
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 4, 200);
        if (n <= 0) continue;
        sockaddr_in sender{};
        socklen_t slen = sizeof(sender);
        ssize_t bytes = recvfrom(sock, &pkt, sizeof(pkt), 0, (sockaddr*)&sender, &slen);
        if (bytes != (ssize_t)PACKET_SIZE) continue;
        uint32_t src_ip = sender.sin_addr.s_addr;
        if (!rate_allow(src_ip)) { if (g_verbose) puts("rate limit"); continue; }
        if (!packet_ok(pkt)) { if (g_verbose) puts("bad magic"); continue; }
        if (hmac_verify(g_hmac_key, 32, (const uint8_t *)&pkt, PACKET_AUTH_SIZE, pkt.hmac, HMAC_TAG_SIZE) != 0) {
            if (g_verbose) puts("bad HMAC"); continue;
        }
        int client_idx = -1;
        uint64_t now = now_us();
        bool is_reset = (pkt.flags & FLAG_RESET);
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            constexpr int UDP_SLOTS = MAX_CLIENTS - 1;
            for (int i = 0; i < UDP_SLOTS; ++i) {
                if (g_clients[i].active && g_clients[i].addr.sin_addr.s_addr == src_ip && g_clients[i].addr.sin_port == sender.sin_port) {
                    client_idx = i; break;
                }
            }
            if (client_idx == -1) {
                for (int i = 0; i < UDP_SLOTS; ++i) {
                    if (!g_clients[i].active || (now - g_clients[i].last_rx_us > WATCHDOG_MS * 1000ULL)) {
                        client_idx = i;
                        g_clients[i].active = true;
                        g_clients[i].addr = sender;
                        g_clients[i].first_pkt = true;
                        g_clients[i].report.reset();
                        if (g_verbose) std::printf("New PC slot %d\n", i+1);
                        break;
                    }
                }
            }
            if (client_idx != -1) {
                bool jump = (g_clients[client_idx].expected_seq > pkt.seq) && ((g_clients[client_idx].expected_seq - pkt.seq) > 100);
                if (!g_clients[client_idx].first_pkt && pkt.seq < g_clients[client_idx].expected_seq && !is_reset && !jump) {
                    if (g_verbose) std::printf("Out-of-order seq %u\n", pkt.seq);
                } else {
                    g_clients[client_idx].first_pkt = false;
                    g_clients[client_idx].expected_seq = pkt.seq + 1;
                    if (is_reset) g_clients[client_idx].report.reset();
                    else g_clients[client_idx].report = pkt.report;
                    g_clients[client_idx].last_rx_us = now;
                }
            }
        }
        if (client_idx == -1) { if (g_verbose) puts("server full"); continue; }
        ++g_pkts_rx;
    }

    puts("[backend] shutting down");
    upnp_remove_mapping(port);
    close(ep); close(sock);
    wt.join(); st.join();
    if (web.joinable()) web.join();
    return 0;
}
