#!/usr/bin/env python3
"""
wyrelay-daemon.py — WyTerminal companion daemon v1.0

Runs on the target Linux machine. Bridges WyTerminal (ESP32-S3 AMOLED) to:
  - Shell command execution (output → Telegram + AMOLED terminal)
  - Screenshot capture → scaled JPEG → Telegram photo + AMOLED inline preview
  - File upload to Telegram
  - Clipboard read
  - System info

Serial protocol (115200 baud):
  ESP32 → daemon:  CMD:<command>\n
  daemon → ESP32:  OUT:<text>\n          (terminal line, green)
                   ERR:<text>\n          (terminal line, red)
                   IMG:<len>\n<bytes>    (JPEG bytes, rendered inline on AMOLED)

Install:
  pip3 install pyserial requests Pillow
  apt install scrot   (or imagemagick for `import`)

Run:
  python3 wyrelay-daemon.py --port /dev/ttyACM0 --token BOT_TOKEN --chat CHAT_ID

Install as service:
  sudo bash install.sh --token TOKEN --chat CHAT_ID
"""

import serial, subprocess, requests, os, sys, time, argparse, io, threading
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("--port",  default="/dev/ttyACM0")
parser.add_argument("--baud",  default=115200, type=int)
parser.add_argument("--token", required=True)
parser.add_argument("--chat",  required=True)
parser.add_argument("--preview-width", default=240, type=int,
                    help="Width to scale screenshot for AMOLED preview (default: 240)")
parser.add_argument("--preview-quality", default=75, type=int,
                    help="JPEG quality for AMOLED preview (default: 75)")
args = parser.parse_args()

BOT_TOKEN    = args.token
ALLOWED_CHAT = str(args.chat)
TG_API       = f"https://api.telegram.org/bot{BOT_TOKEN}"
MAX_OUT      = 3800
SHELL        = "/bin/bash"
PREVIEW_W    = args.preview_width
PREVIEW_Q    = args.preview_quality

ser = None  # global serial connection

# ── Telegram helpers ──────────────────────────────────────────────────
def tg_send(text, chat_id=None):
    cid = chat_id or ALLOWED_CHAT
    try:
        requests.post(f"{TG_API}/sendMessage",
            json={"chat_id": cid, "text": text, "parse_mode": "HTML"},
            timeout=10)
    except Exception as e:
        print(f"[tg] send error: {e}")

def tg_photo(path_or_bytes, caption="", chat_id=None):
    cid = chat_id or ALLOWED_CHAT
    try:
        if isinstance(path_or_bytes, bytes):
            files = {"photo": ("screenshot.jpg", path_or_bytes, "image/jpeg")}
        else:
            files = {"photo": open(path_or_bytes, "rb")}
        requests.post(f"{TG_API}/sendPhoto",
            data={"chat_id": cid, "caption": caption},
            files=files, timeout=30)
    except Exception as e:
        print(f"[tg] photo error: {e}")

def tg_document(path, caption="", chat_id=None):
    cid = chat_id or ALLOWED_CHAT
    try:
        with open(path, "rb") as f:
            requests.post(f"{TG_API}/sendDocument",
                data={"chat_id": cid, "caption": caption},
                files={"document": f}, timeout=30)
    except Exception as e:
        print(f"[tg] doc error: {e}")

# ── Serial helpers ────────────────────────────────────────────────────
def serial_send_text(prefix, text):
    """Send OUT: or ERR: line to firmware terminal display."""
    if ser and ser.is_open:
        line = f"{prefix}{text[:60]}\n"
        try: ser.write(line.encode())
        except: pass

def serial_send_image(jpeg_bytes):
    """Send IMG:<len>\n<bytes> to firmware for AMOLED inline preview."""
    if not ser or not ser.is_open:
        return
    try:
        header = f"IMG:{len(jpeg_bytes)}\n".encode()
        ser.write(header)
        time.sleep(0.05)  # let firmware switch to binary mode
        # Send in chunks to avoid serial buffer overflow
        chunk = 512
        for i in range(0, len(jpeg_bytes), chunk):
            ser.write(jpeg_bytes[i:i+chunk])
            time.sleep(0.01)
        print(f"[serial] sent image {len(jpeg_bytes)} bytes")
    except Exception as e:
        print(f"[serial] image send error: {e}")

# ── Screenshot ────────────────────────────────────────────────────────
def take_screenshot():
    """Capture screen, return raw PNG bytes or None."""
    path = "/tmp/wyterminal-shot.png"
    for cmd in [f"scrot {path}", f"import -window root {path}",
                f"gnome-screenshot -f {path}"]:
        r, c = run_shell(cmd, timeout=8)
        if c == 0 and os.path.exists(path):
            with open(path, "rb") as f: data = f.read()
            os.remove(path)
            return data
    return None

def scale_for_amoled(png_bytes):
    """Scale PNG to PREVIEW_W wide, return JPEG bytes for AMOLED."""
    try:
        img = Image.open(io.BytesIO(png_bytes)).convert("RGB")
        w, h = img.size
        new_h = int(h * PREVIEW_W / w)
        img = img.resize((PREVIEW_W, new_h), Image.LANCZOS)
        out = io.BytesIO()
        img.save(out, format="JPEG", quality=PREVIEW_Q, optimize=True)
        return out.getvalue(), (PREVIEW_W, new_h)
    except Exception as e:
        print(f"[img] scale error: {e}")
        return None, (0, 0)

# ── Shell ─────────────────────────────────────────────────────────────
def run_shell(cmd, timeout=30):
    try:
        r = subprocess.run(cmd, shell=True, executable=SHELL,
                           capture_output=True, text=True, timeout=timeout)
        return (r.stdout + r.stderr).strip(), r.returncode
    except subprocess.TimeoutExpired:
        return "⏱ timed out", 1
    except Exception as e:
        return f"❌ {e}", 1

# ── Command processor ─────────────────────────────────────────────────
def process_command(cmd):
    cmd = cmd.strip()
    print(f"[cmd] {cmd}")

    if cmd == "/screenshot":
        serial_send_text("OUT:", "📸 Capturing screen...")
        tg_send("📸 Capturing screen...")
        png = take_screenshot()
        if not png:
            serial_send_text("ERR:", "Screenshot failed")
            tg_send("❌ No screenshot tool found (apt install scrot)")
            return
        # Scale for AMOLED preview
        jpg, (pw, ph) = scale_for_amoled(png)
        # Send full-size to Telegram as photo
        tg_photo(jpg, f"📸 Screenshot ({pw}x{ph} preview)")
        serial_send_text("OUT:", f"📸 {pw}x{ph} → TG + AMOLED")
        # Send scaled JPEG to AMOLED
        if jpg:
            serial_send_image(jpg)
        return

    if cmd.startswith("/shell "):
        shell_cmd = cmd[7:].strip()
        serial_send_text("OUT:", f"$ {shell_cmd}")
        out, code = run_shell(shell_cmd)
        emoji = "✅" if code == 0 else "❌"
        # Send output lines to AMOLED (first 5 lines)
        lines = out.split('\n') if out else []
        for line in lines[:5]:
            if line.strip():
                prefix = "OUT:" if code == 0 else "ERR:"
                serial_send_text(prefix[:-1] + ":", line)
        if len(lines) > 5:
            serial_send_text("OUT:", f"...+{len(lines)-5} more lines")
        # Send full output to Telegram
        if out:
            chunks = [out[i:i+MAX_OUT] for i in range(0, len(out), MAX_OUT)]
            for i, chunk in enumerate(chunks[:3]):
                header = f"{emoji} <code>{shell_cmd}</code>" if i == 0 else f"(cont. {i+1})"
                tg_send(f"{header}\n<code>{chunk}</code>")
            if len(chunks) > 3:
                tg_send(f"⚠️ Truncated ({len(out)} chars)")
        else:
            tg_send(f"{emoji} <code>{shell_cmd}</code>\n(no output, exit {code})")
            serial_send_text("OUT:" if code==0 else "ERR:", f"exit {code}")
        return

    if cmd.startswith("/upload "):
        path = cmd[8:].strip()
        if os.path.exists(path):
            sz = os.path.getsize(path)
            if sz > 50*1024*1024:
                tg_send(f"❌ Too large ({sz//1024//1024}MB)")
                serial_send_text("ERR:", "file too large")
            else:
                tg_send(f"📤 Uploading {os.path.basename(path)}...")
                serial_send_text("OUT:", f"uploading {os.path.basename(path)}")
                tg_document(path, f"📄 {os.path.basename(path)}")
                serial_send_text("OUT:", "✅ uploaded")
        else:
            tg_send(f"❌ Not found: {path}")
            serial_send_text("ERR:", "file not found")
        return

    if cmd == "/clipboard":
        out, _ = run_shell("xclip -selection clipboard -o 2>/dev/null || xsel --clipboard --output 2>/dev/null || wl-paste 2>/dev/null")
        if out:
            tg_send(f"📋 Clipboard:\n<code>{out[:MAX_OUT]}</code>")
            serial_send_text("OUT:", out[:40])
        else:
            tg_send("❌ Clipboard empty or no tool (xclip/xsel/wl-paste)")
            serial_send_text("ERR:", "clipboard empty")
        return

    if cmd == "/sysinfo":
        out, _ = run_shell("echo '=CPU=' && uptime && echo '=RAM=' && free -h && echo '=DISK=' && df -h / && echo '=IP=' && hostname -I")
        tg_send(f"💻 Sysinfo:\n<code>{out[:MAX_OUT]}</code>")
        for line in out.split('\n')[:6]:
            if line.strip(): serial_send_text("OUT:", line)
        return

    if cmd == "/ps":
        out, _ = run_shell("ps aux --sort=-%cpu | head -12")
        tg_send(f"⚙️ Processes:\n<code>{out[:MAX_OUT]}</code>")
        serial_send_text("OUT:", "ps sent to TG")
        return

    tg_send(f"❓ Unknown: {cmd}")
    serial_send_text("ERR:", f"unknown: {cmd[:30]}")

# ── Serial listener ───────────────────────────────────────────────────
def serial_listener():
    global ser
    was_connected = False
    while True:
        try:
            print(f"[serial] connecting {args.port}...")
            ser = serial.Serial(args.port, args.baud, timeout=1,
                                dsrdtr=False, rtscts=False)
            ser.dtr = False
            ser.rts = False
            print(f"[serial] connected")
            if not was_connected:
                tg_send(f"🟢 WyTerminal daemon online\n{os.uname().nodename}")
            else:
                tg_send(f"🟢 WyTerminal reconnected")
            serial_send_text("OUT:", f"daemon: {os.uname().nodename}")
            was_connected = True
            while True:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if line and line.startswith("CMD:"):
                    process_command(line[4:])
        except serial.SerialException as e:
            print(f"[serial] error: {e}")
            if was_connected:
                tg_send(f"🔴 WyTerminal unplugged")
                was_connected = False
            ser = None
            time.sleep(3)
        except Exception as e:
            print(f"[serial] unexpected: {e}")
            ser = None
            time.sleep(3)

if __name__ == "__main__":
    print(f"WyTerminal daemon v1.0 | port={args.port} | chat={ALLOWED_CHAT}")
    tg_send(f"🚀 WyTerminal daemon starting...\nHost: {os.uname().nodename}")
    try:
        serial_listener()
    except KeyboardInterrupt:
        tg_send("🔴 Daemon stopped")
