# WyTerminal

**ESP32-S3 AMOLED — Linux terminal over Telegram.**

Plug into any Linux machine. Get a full interactive shell in Telegram. Commands you send appear on the AMOLED display. Screenshot output renders inline on the screen. No SSH. No open ports.

Built by [Wyltek Industries](https://wyltekindustries.com).

---

## What It Does

```
You: /shell df -h /
Bot: ✅ df -h /
     /dev/mmcblk0p2   58G   22%
[AMOLED shows the same output live]

You: /screenshot  
Bot: [sends full screenshot as Telegram photo]
[AMOLED shows scaled preview inline in terminal]

You: /key ctrl+alt+t
[terminal opens on target machine]

You: /run sudo systemctl restart ckb
[types command + Enter]
```

---

## Hardware

**[LilyGO T-Display S3 AMOLED](https://lilygo.cc/products/t-display-s3-amoled)**
- ESP32-S3 (native USB HID)
- 1.91" AMOLED RM67162 (240×536, QSPI)
- Black background = true black pixels = perfect terminal aesthetic

---

## How It Works

```
Telegram ──→ WyTerminal (WiFi)
                  │
                  ├──→ USB HID keyboard injection → target machine
                  │
                  └──→ USB CDC serial ←──→ wyrelay-daemon.py
                                                │
                                          bash / screenshot
                                                │
                                      output → Telegram + AMOLED
```

**Without daemon:** HID keyboard only (type commands, key combos)  
**With daemon:** Full two-way shell — output comes back to Telegram and renders on AMOLED

---

## Quick Start

### 1. Flash firmware

```bash
git clone https://github.com/toastmanAu/WyTerminal
```

Edit `firmware/WyTerminal.ino`:
```cpp
#define WIFI_SSID        "your-wifi"
#define WIFI_PASSWORD    "your-password"
#define BOT_TOKEN        "your-bot-token"
#define ALLOWED_CHAT_ID  123456789LL
```

Arduino IDE settings:
- Board: `LilyGo T-Display-S3`
- **USB Mode: `USB-OTG (TinyUSB)`** ← critical
- USB CDC On Boot: `Enabled`
- Flash: 16MB

Library: [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) (install from GitHub, not Library Manager)

### 2. Install daemon on target machine

```bash
pip3 install pyserial requests Pillow
sudo apt install scrot

python3 daemon/wyrelay-daemon.py \
    --port /dev/ttyACM0 \
    --token YOUR_BOT_TOKEN \
    --chat YOUR_CHAT_ID
```

### 3. Use it

Send commands to your Telegram bot:

| Command | Action |
|---|---|
| `/shell <cmd>` | Run command → output to TG + AMOLED |
| `/screenshot` | Capture screen → TG photo + AMOLED preview |
| `/upload <path>` | Send file to Telegram |
| `/clipboard` | Read clipboard |
| `/sysinfo` | CPU/RAM/disk/IP |
| `/ps` | Top processes |
| `/run <cmd>` | HID: type + Enter (no daemon needed) |
| `/key ctrl+alt+t` | HID: open terminal |
| `/key ctrl+c` | HID: interrupt process |
| `/clear` | Clear AMOLED display |
| `/status` | IP + uptime |
| `/help` | Command list |

---

## AMOLED Display

The display shows a live scrolling terminal log:

```
┌──────────────────────┐
│ WyTerminal      LIVE │  ← header (dark green)
│──────────────────────│
│ > /shell df -h /     │  ← your command (cyan)
│   /dev/mm  58G  22%  │  ← output (green)
│ > /screenshot        │
│   240x135 → TG+AMOLED│
│  [  screenshot jpeg  ]│  ← inline image preview
│ > /key ctrl+alt+t    │
│   ctrl+alt+t         │
│──────────────────────│
│ 192.168.1.x -65dBm   │  ← footer (dark blue)
└──────────────────────┘
```

Screenshot previews render inline — scaled to 240px wide, full AMOLED width.

---

## Security

- Only your Telegram chat ID accepted (hardcoded in firmware)
- Physical USB access required — the stick IS the auth factor
- No open ports, works behind any firewall

---

## Repo Structure

```
WyTerminal/
├── firmware/
│   └── WyTerminal.ino       # ESP32-S3 AMOLED firmware
├── daemon/
│   ├── wyrelay-daemon.py    # Companion daemon
│   └── install.sh           # systemd installer
├── hardware/
│   └── HARDWARE.md          # Board guide
└── README.md
```

---

## Roadmap

- [x] USB HID keyboard injection
- [x] AMOLED scrolling terminal display
- [x] /shell — command output to TG + AMOLED
- [x] /screenshot — TG photo + AMOLED inline preview
- [x] /upload, /clipboard, /sysinfo
- [ ] /mouse — USB HID mouse control
- [ ] /macro — save/replay command sequences
- [ ] T-Dongle S3 variant (USB-A dongle form factor)
- [ ] Windows daemon support

---

*See also: [WyRelay](https://github.com/toastmanAu/WyRelay) — the minimal USB HID keyboard-only variant.*

*Part of [Wyltek Industries](https://wyltekindustries.com).*
