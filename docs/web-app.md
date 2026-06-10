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

## Browser Compatibility

| Feature | Chrome | Firefox | Safari | Edge |
|---------|--------|---------|--------|------|
| WebSocket | ✓ | ✓ | ✓ | ✓ |
| Touch controls | ✓ | ✓ | ✓ | ✓ |
| Gamepad API | ✓ | ✓ | ✓ | ✓ |
| Fullscreen API | ✓ | ✓ | ✓ | ✓ |
| localStorage | ✓ | ✓ | ✓ | ✓ |

---

## Security Notes

- The web server binds to **`0.0.0.0`** (all interfaces).
- WebSocket connections does not use the same HMAC-SHA256 protocol as native clients.
- There is **no TLS** (HTTPS/WSS) built in. For use over untrusted networks, pair
  with a reverse proxy such as **[Caddy](https://caddyserver.com/)**.

---

## Technical Overview

The web interface is **compiled directly into the `ns-backend` binary** as C++ string
constants — there are no separate HTML, CSS, or JS files to deploy. The embedded web
server:

1. Listens on a configurable TCP port alongside the main UDP socket
2. Serves static HTML/CSS/JS for all three pages on `GET /`, `GET /mobile.html`, and `GET /editor.html`
3. Upgrades WebSocket connections on the same port
4. Treats incoming WebSocket binary frames as NS-PC-Control protocol packets, writing
   controller state into the shared backend state alongside physical controllers

This means the web client acts as a **virtual controller** — no code changes to the
protocol or HID output path were needed.
