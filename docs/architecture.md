# 🏗️ Architecture & Optimization

## ⚡ Latency Optimization
For the lowest possible latency in competitive or timing-strict games:

* Use a wired Ethernet connection whenever possible.
* Run the Raspberry Pi backend with maximum real-time priority: `sudo chrt -f 99 ./ns-backend`.
* Connect controllers directly to the PC via USB rather than Bluetooth.
* Keep the Raspberry Pi and PC on the same local network.
* Avoid Wi-Fi power-saving modes on your PC/Laptop.

## 🔒 Security
Each UDP datagram is authenticated with a truncated **HMAC-SHA256** tag derived from a compiled-in default key. The backend silently drops any packet with an invalid tag — preventing network attackers from injecting malicious controller inputs.

**No configuration needed.** The HMAC is always active on both sides. If you want a different key, edit `DEFAULT_SECRET` in `server/rpi/include/protocol.hpp` and in the `client/<OS>/ns-gamepad.cpp` or `client/<OS>/ns-gui.cpp` and recompile both the client and the server.

### Additional protection layers:
| Layer | Purpose |
|---|---|
| **Magic/version check** | Rejects random internet noise on the very first read |
| **Per-IP rate limiter** | Limits to 2000 pkts/sec to prevent UDP floods from saturating the Pi |
| **Multi-controller** | Up to 4 controllers per PC, mapped to fixed physical slots (P1–P4) |
| **Multi-client** | Up to 4 PCs can connect simultaneously, each contributing up to 4 controllers |
| **HMAC-SHA256** | Cryptographically authenticates every packet using a 16-byte truncated hash |
| **Sequence counter** | Prevents replay attacks of old, captured packets |
