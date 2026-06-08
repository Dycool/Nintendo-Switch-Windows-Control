# Raspberry Pi Server Setup

To use your Raspberry Pi as a controller emulator, configure it to act as a USB gadget.

## Prerequisite: Enable USB Gadget Mode (Boot Settings)

Before the Raspberry Pi can emulate a USB controller, enable the USB OTG drivers at the system level. Run the following commands in your Pi's terminal:

```bash
# 1. Enable the dwc2 driver in config.txt
echo "dtoverlay=dwc2" | sudo tee -a /boot/firmware/config.txt

# 2. Add required modules to cmdline.txt
sudo sed -i 's/rootwait/rootwait modules-load=dwc2,libcomposite/' /boot/firmware/cmdline.txt

# 3. Reboot the system to apply changes
sudo reboot
```

## Connecting to the Console

Connect the Raspberry Pi to the console dock via USB:
* **Raspberry Pi 4:** Use the USB-C port.
* **Raspberry Pi Zero / Zero 2 W:** Use the inner Micro-USB data port.

---

## Running the Server (Manual Method)

The server handles USB gadget setup **automatically** on startup - no separate script needed. Just run:

```bash
sudo chrt -f 99 ./ns-backend
```

> **Note:** `chrt -f 99` gives the process maximum real-time priority for lowest possible latency.

To bind a custom UDP address or port, pass it through `-b`:

```bash
sudo chrt -f 99 ./ns-backend -b 0.0.0.0:7332
sudo chrt -f 99 ./ns-backend -b :7332
```

---

## Automate on Boot (Optional Systemd Service)

If you want the Raspberry Pi to automatically set up the USB gadget and start the backend every time you turn it on, create a systemd service.

1. Create a new service file:
```bash
sudo nano /etc/systemd/system/ns-control.service
```

2. Paste the following configuration. Adjust the `/home/YOUR_USER/...` paths to match the exact location of your downloaded or cloned repository files.
```ini
[Unit]
Description=NS PC Control Backend
After=network.target

[Service]
# The server handles USB gadget setup automatically.
ExecStart=/usr/bin/chrt -f 99 /home/YOUR_USER/NS-PC-Control/server/rpi/ns-backend
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
```

3. Enable and start the service:
```bash
sudo systemctl daemon-reload
sudo systemctl enable ns-control.service
sudo systemctl start ns-control.service
```
