# Web App & Mobile Touch Controls

The Raspberry Pi backend includes an **embedded web server** (HTTP + WebSocket) that serves
a full control interface accessible from any browser — no native client installation needed.

## Enabling the Web Server

The web server is **disabled by default**. Start the backend with the `-w` flag:

```bash
sudo chrt -f 99 ./ns-backend -w
```

This serves on **port 8080**. To use a different port:

```bash
sudo chrt -f 99 ./ns-backend -w 3000
```

Once running, open the URLs below in a browser on the same network.

> **Note:** The web server bypasses the native UDP client — it communicates directly with
> the backend via WebSocket. Latency is still low, but a wired USB controller on the PC
> client will always be faster.

---

## Pages

### Main Control Panel — `http://<pi-ip>:8080/`

Desktop-oriented interface with:

- **Virtual gamepad buttons** — click A/B/X/Y, d-pad, L/R/ZL/ZR, START/BACK, HOME, CAPTURE, L3/R3
- **Left joystick** — click-and-drag or arrow keys
- **Right joystick** — click-and-drag or `I`/`J`/`K`/`L`
- **Customizable keyboard bindings** — click a button then press a key to rebind; saved to `localStorage`
- **Physical gamepad support** — connect a USB gamepad to your computer and the browser's
  [Gamepad API](https://developer.mozilla.org/en-US/docs/Web/API/Gamepad_API) will pick it up
- **Connection status indicator** — shows connected/disconnected state

### Mobile Touch Controls — `http://<pi-ip>:8080/mobile`

Touch-optimized controller overlay designed for phones and tablets:

- **On-screen buttons** — A/B/X/Y, d-pad, L/R/ZL/ZR, START/BACK, HOME, CAPTURE, L3/R3
- **Virtual joysticks** — left and right analog sticks with throw distance mapped to axis range
- **Auto-hide UI** — controls fade after a moment of inactivity; tap to reveal
- **Fullscreen support** — tap the fullscreen icon (or use the browser's fullscreen) for immersive play
- **Landscape orientation** — best experienced in landscape mode; `viewport-fit=cover` plus `safe-area-inset` support for modern phones (notch & rounded corners)
- **Low-latency WebSocket** — same binary protocol as the native client

### Layout Editor — `http://<pi-ip>:8080/editor`

Customize the position and size of every mobile touch control element:

- **Drag** buttons/joysticks to reposition them
- **Pinch/zoom** to resize elements
- **Save** — layout is persisted to `localStorage` and loaded when you return to `/mobile`
- **Reset** — revert to the default layout
- Use this to account for different screen sizes, grip preferences, or one-handed play

---

## Keyboard Shortcuts (Desktop Control Panel)

| Key | Action |
|-----|--------|
| Arrow keys / WASD | D-pad (left stick also) |
| `I`, `J`, `K`, `L` | Right joystick |
| `Z`, `X`, `A`, `S` | Y, B, A, X |
| `Q`, `E` | L, R |
| `1`, `2` | ZL, ZR |
| `Enter` / `Space` | START |
| `Backspace` | BACK |
| `H` | HOME |
| `C` | CAPTURE |
| `F` | L3 |
| `G` | R3 |

All bindings are **reassignable** by clicking a button in the UI and pressing a new key.

---

## Browser Compatibility

| Feature | Chrome | Firefox | Safari | Edge |
|---------|--------|---------|--------|------|
| WebSocket | ✓ | ✓ | ✓ | ✓ |
| Touch controls | ✓ | ✓ | ✓ | ✓ |
| Gamepad API | ✓ | ✓ | ✓ | ✓ |
| Fullscreen API | ✓ | ✓ | ✓ | ✓ |
| localStorage | ✓ | ✓ | ✓ | ✓ |

Mobile Safari (iOS) has full support — including Gamepad API for MFi controllers
and touch controls for on-screen play.

---

## Security Notes

- The web server binds to **`0.0.0.0`** (all interfaces). Consider firewall rules if
  you don't want other devices on your LAN accessing the control panel.
- WebSocket connections use the same HMAC-SHA256 protocol as native clients —
  authentication is handled transparently by the backend.
- There is **no TLS** (HTTPS/WSS) built in. For use over untrusted networks, pair
  with a reverse proxy (e.g., `nginx` + `certbot`).

---

## Technical Overview

The web interface is **compiled directly into the `ns-backend` binary** as C++ string
constants — there are no separate HTML, CSS, or JS files to deploy. The embedded web
server:

1. Listens on a configurable TCP port alongside the main UDP socket
2. Serves static HTML/CSS/JS for all three pages on `GET /`, `GET /mobile`, and `GET /editor`
3. Upgrades WebSocket connections on the same port
4. Treats incoming WebSocket binary frames as NS-PC-Control protocol packets, writing
   controller state into the shared backend state alongside physical controllers

This means the web client acts as a **virtual controller** — no code changes to the
protocol or HID output path were needed.
