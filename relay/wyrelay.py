#!/usr/bin/env python3
"""
WyTerminal Relay v2 — Dynamic SSH relay
Runs permanently on Pi. No predefined targets.

Features:
- Dynamic targets: /target user@host or /target alias
- SSH auto-enable on plug, auto-disable on unplug (if we enabled it)
- Sudo password relay via Telegram
- Screenshot: SSH + scrot + scale to AMOLED width
- Persistent target list in ~/.wyterminal/targets.json

Endpoints:
  POST /shell        {cmd, target_id}  → {output, exit_code, needs_password}
  POST /shell/input  {input, session}  → {output, exit_code}  (password/stdin)
  POST /screenshot   {target_id}       → JPEG bytes
  POST /target/add   {alias, host}     → saves target
  GET  /targets                        → list saved targets
  POST /ssh/check    {target_id}       → {ssh_running, we_enabled}
  POST /ssh/enable   {target_id}       → enables SSH, records if we did it
  POST /ssh/disable  {target_id}       → disables SSH if we enabled it
  GET  /health
"""

from flask import Flask, request, jsonify, Response
import subprocess, os, json, io, threading, time, tempfile
from pathlib import Path
from PIL import Image

app = Flask(__name__)

CONFIG_DIR  = Path.home() / ".wyterminal"
TARGETS_FILE = CONFIG_DIR / "targets.json"
SSH_STATE_FILE = CONFIG_DIR / "ssh-state.json"  # tracks what we enabled
PREVIEW_W   = 240
DISPLAY     = os.environ.get("DISPLAY", ":0")
BOT_TOKEN   = os.environ.get("WYTERMINAL_TOKEN", "")
CHAT_ID     = os.environ.get("WYTERMINAL_CHAT",  "")

CONFIG_DIR.mkdir(exist_ok=True)

# ── Target store ──────────────────────────────────────────────────────
def load_targets():
    if TARGETS_FILE.exists():
        return json.loads(TARGETS_FILE.read_text())
    return {"local": None, "pi5": None}  # None = run locally

def save_targets(t):
    TARGETS_FILE.write_text(json.dumps(t, indent=2))

def load_ssh_state():
    if SSH_STATE_FILE.exists():
        return json.loads(SSH_STATE_FILE.read_text())
    return {}

def save_ssh_state(s):
    SSH_STATE_FILE.write_text(json.dumps(s, indent=2))

def resolve_target(target_id):
    """Returns SSH host string or None for local. Raises if unknown."""
    targets = load_targets()
    if target_id in targets:
        return targets[target_id]
    # Allow raw user@host directly
    if "@" in target_id:
        return target_id
    raise KeyError(f"Unknown target: {target_id}")

# ── Shell execution ───────────────────────────────────────────────────
def run_local(cmd, timeout=30, stdin_data=None, env_extra=None):
    env = {**os.environ}
    if env_extra: env.update(env_extra)
    try:
        r = subprocess.run(
            cmd, shell=True, executable="/bin/bash",
            input=stdin_data,
            capture_output=True, text=True, timeout=timeout, env=env
        )
        return (r.stdout + r.stderr).strip(), r.returncode
    except subprocess.TimeoutExpired:
        return "⏱ timed out", 1
    except Exception as e:
        return str(e), 1

def run_ssh(host, cmd, timeout=30, password=None):
    if password:
        # Use sshpass for password auth
        ssh_cmd = f'sshpass -p {json.dumps(password)} ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no {host} {json.dumps(cmd)}'
    else:
        ssh_cmd = f'ssh -o ConnectTimeout=8 -o BatchMode=yes -o StrictHostKeyChecking=no {host} {json.dumps(cmd)}'
    return run_local(ssh_cmd, timeout=timeout+10)

def run_on(target_id, cmd, timeout=30, password=None):
    host = resolve_target(target_id)
    if host:
        return run_ssh(host, cmd, timeout, password)
    else:
        return run_local(cmd, timeout, env_extra={"DISPLAY": DISPLAY})

# ── SSH management ────────────────────────────────────────────────────
def check_ssh(host):
    """Returns True if SSH port 22 is open."""
    out, code = run_local(f"nc -z -w3 {host.split('@')[-1]} 22 2>/dev/null", timeout=5)
    return code == 0

def enable_ssh_on_target(target_id):
    """Try to enable SSH. Returns (success, we_enabled_it)."""
    host = resolve_target(target_id)
    if not host: return True, False  # local, always available

    ip = host.split("@")[-1]
    if check_ssh(host):
        return True, False  # already running

    # Not running — can't SSH in to start it... 
    # The HID keyboard path is the only option here.
    # Signal back to firmware to type the enable command.
    return False, False

def disable_ssh_if_we_enabled(target_id):
    state = load_ssh_state()
    if target_id not in state or not state[target_id].get("we_enabled"):
        return
    host = resolve_target(target_id)
    if host:
        run_ssh(host, "sudo systemctl stop ssh 2>/dev/null || sudo service ssh stop 2>/dev/null")
    else:
        run_local("sudo systemctl stop ssh 2>/dev/null")
    del state[target_id]
    save_ssh_state(state)

# ── Screenshot ────────────────────────────────────────────────────────
def screenshot_local():
    path = "/tmp/wyterm-shot.png"
    env = {**os.environ, "DISPLAY": DISPLAY}
    for c in [f"scrot {path}", f"gnome-screenshot -f {path}", f"import -window root {path}"]:
        r = subprocess.run(c, shell=True, env=env, capture_output=True)
        if r.returncode == 0 and os.path.exists(path):
            with open(path, "rb") as f: data = f.read()
            os.remove(path)
            return data
    return None

def screenshot_ssh(host):
    import base64
    cmd = 'DISPLAY=:0 scrot /tmp/wyterm-shot.png && base64 /tmp/wyterm-shot.png; rm -f /tmp/wyterm-shot.png'
    out, code = run_ssh(host, cmd, timeout=20)
    if code != 0: return None
    try:
        # base64 output may have leading/trailing non-b64 chars
        lines = [l for l in out.split('\n') if len(l) > 10]
        return base64.b64decode(''.join(lines))
    except: return None

def scale_jpeg(png_bytes):
    img = Image.open(io.BytesIO(png_bytes)).convert("RGB")
    w, h = img.size
    new_h = int(h * PREVIEW_W / w)
    img = img.resize((PREVIEW_W, new_h), Image.LANCZOS)
    out = io.BytesIO()
    img.save(out, format="JPEG", quality=75, optimize=True)
    return out.getvalue()

def tg_send(text):
    if not BOT_TOKEN or not CHAT_ID: return
    import requests
    try:
        requests.post(f"https://api.telegram.org/bot{BOT_TOKEN}/sendMessage",
            json={"chat_id": CHAT_ID, "text": text}, timeout=5)
    except: pass

def tg_photo(jpg_bytes, caption=""):
    if not BOT_TOKEN or not CHAT_ID: return
    import requests
    try:
        requests.post(f"https://api.telegram.org/bot{BOT_TOKEN}/sendPhoto",
            data={"chat_id": CHAT_ID, "caption": caption},
            files={"photo": ("shot.jpg", jpg_bytes, "image/jpeg")}, timeout=30)
    except: pass

# ── Pending password requests ─────────────────────────────────────────
pending_inputs = {}  # session_id → {"event": threading.Event, "value": str}

# ── Routes ────────────────────────────────────────────────────────────
@app.route("/shell", methods=["POST"])
def shell():
    data      = request.get_json(force=True) or {}
    cmd       = data.get("cmd", "")
    target_id = data.get("target", "local")
    password  = data.get("password")

    if not cmd:
        return jsonify({"error": "no cmd"}), 400

    try:
        out, code = run_on(target_id, cmd, password=password)
    except KeyError as e:
        return jsonify({"error": str(e)}), 404

    # Detect sudo password prompt
    if "sudo" in cmd and ("password" in out.lower() or "Password" in out):
        session_id = f"sudo_{time.time()}"
        pending_inputs[session_id] = {"event": threading.Event(), "value": None}
        tg_send(f"🔐 sudo password required for: {cmd[:50]}\nReply with /input <password>")
        # Wait up to 60s for password
        pending_inputs[session_id]["event"].wait(timeout=60)
        pw = pending_inputs.pop(session_id, {}).get("value")
        if pw:
            # Re-run with password via stdin pipe
            host = resolve_target(target_id)
            if host:
                pipe_cmd = f'echo {json.dumps(pw)} | ssh -tt -o ConnectTimeout=8 -o StrictHostKeyChecking=no {host} "sudo -S {cmd}"'
            else:
                pipe_cmd = f'echo {json.dumps(pw)} | sudo -S {cmd}'
            out, code = run_local(pipe_cmd)
        else:
            return jsonify({"error": "password timeout"}), 408

    return jsonify({"output": out[:8000], "exit_code": code})

@app.route("/shell/input", methods=["POST"])
def shell_input():
    """Receive password/input from user."""
    data = request.get_json(force=True) or {}
    value = data.get("value", "")
    # Find the oldest pending input
    for sid, p in pending_inputs.items():
        if not p["event"].is_set():
            p["value"] = value
            p["event"].set()
            return jsonify({"ok": True})
    return jsonify({"error": "no pending input"}), 404

@app.route("/screenshot", methods=["POST"])
def screenshot_route():
    data      = request.get_json(force=True) or {}
    target_id = data.get("target", "local")
    try:
        host = resolve_target(target_id)
    except KeyError as e:
        return jsonify({"error": str(e)}), 404

    png = screenshot_ssh(host) if host else screenshot_local()
    if not png:
        return jsonify({"error": "screenshot failed — no display or scrot missing"}), 500
    try:
        jpg = scale_jpeg(png)
        # Also send to Telegram
        tg_photo(jpg, f"📸 {target_id}")
        return Response(jpg, mimetype="image/jpeg")
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route("/target/add", methods=["POST"])
def target_add():
    data  = request.get_json(force=True) or {}
    alias = data.get("alias", "").strip()
    host  = data.get("host", "").strip()  # user@ip or None for local
    if not alias:
        return jsonify({"error": "alias required"}), 400
    targets = load_targets()
    targets[alias] = host or None
    save_targets(targets)
    return jsonify({"ok": True, "targets": targets})

@app.route("/targets", methods=["GET"])
def targets_list():
    return jsonify(load_targets())

@app.route("/ssh/check", methods=["POST"])
def ssh_check():
    data      = request.get_json(force=True) or {}
    target_id = data.get("target", "local")
    try:
        host = resolve_target(target_id)
    except KeyError as e:
        return jsonify({"error": str(e)}), 404
    if not host:
        return jsonify({"ssh_running": True, "local": True})
    ip = host.split("@")[-1]
    running = check_ssh(host)
    state = load_ssh_state()
    we_enabled = state.get(target_id, {}).get("we_enabled", False)
    return jsonify({"ssh_running": running, "we_enabled": we_enabled, "host": ip})

@app.route("/ssh/restore", methods=["POST"])
def ssh_restore():
    """Called on unplug — disable SSH on any target we enabled."""
    data      = request.get_json(force=True) or {}
    target_id = data.get("target", "local")
    disable_ssh_if_we_enabled(target_id)
    return jsonify({"ok": True})

@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok", "targets": list(load_targets().keys())})

if __name__ == "__main__":
    print("WyTerminal Relay v2.0")
    print(f"Targets: {list(load_targets().keys())}")
    print("Listening on 0.0.0.0:7799")
    print(f"Bot token: {'set' if BOT_TOKEN else 'NOT SET — set WYTERMINAL_TOKEN env var'}")
    app.run(host="0.0.0.0", port=7799, debug=False, threaded=True)
