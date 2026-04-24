# WyTerminal USB-NCM Relay Self-Bootstrap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Plug WyTerminal into a Linux machine with no internet; within ~10 s of plug-in, `/shell` commands from Telegram return structured shell output via a Python HTTP daemon that the firmware HID-types into `/tmp` on first contact.

**Architecture:** The existing v3 firmware already HTTP-POSTs `/shell` requests to a relay, preferring `http://192.168.7.1:7799` (USB-NCM) over a WiFi relay. This plan adds the missing piece: a small Python HTTP daemon embedded in firmware as a C++ raw string, HID-typed into `/tmp/wyrd.py` and launched via `nohup python3` when `/health` does not respond. Plus one new `/password` command with AMOLED echo suppression and one edit to `/deploy` so the user can retry bootstrap after navigating past a login prompt with `/run <user>` and `/password <pass>`.

**Tech Stack:** ESP32-S3 Arduino core 3.x, TinyUSB (HID + NCM), lwIP (existing `usb_ncm.cpp`), `HTTPClient` + `WiFiClientSecure`, Python 3 stdlib (`http.server`, `subprocess`, `json`, `socketserver`), arduino-cli for scripted compile/flash.

**Spec:** `docs/superpowers/specs/2026-04-24-wyterminal-usb-relay-bootstrap-design.md` (commit `68d9b02`).

---

## File structure

| Path | Change | Responsibility |
|---|---|---|
| `daemon/wyrelay-http.py` | NEW | Canonical, stdlib-only HTTP daemon source. `/health` and `/shell` endpoints. Single source of truth — embedded into firmware by sync tool. |
| `daemon/test_wyrelay_http.sh` | NEW | Integration test: spawns daemon, curls `/health` and `/shell`, asserts responses. Run before every commit that touches the daemon. |
| `tools/sync-embedded-relay.sh` | NEW | Emits `firmware/embedded_relay.h` containing `daemon/wyrelay-http.py` wrapped in a C++ raw string literal. Run after editing the daemon. |
| `firmware/embedded_relay.h` | NEW (generated) | `const char EMBEDDED_RELAY_PY[] = R"PY(...)PY";` — consumed by `WyTerminal.ino`. Committed to repo so the firmware builds without needing the sync tool. |
| `firmware/WyTerminal.ino` | EDIT | Add `#include "embedded_relay.h"`. Add `bootstrap_usb_relay()` function. Call it once in `setup()`. Edit `/deploy` branch to call `bootstrap_usb_relay()` before falling back to the existing curl-install `try_deploy_relay()`. Add `/password` branch with AMOLED-echo suppression. |

Files NOT touched: `firmware/usb_ncm.cpp`, `firmware/usb_ncm.h`, `relay/wyrelay.py`, `daemon/wyrelay-daemon.py` (the legacy serial daemon).

---

## Task 0: Install arduino-cli

Required once per machine. Confirmed absent on driveThree; needed for every `arduino-cli compile` / `upload` step below. The Arduino IDE AppImage already installed will keep working, but the scripted plan uses CLI.

**Files:** none (installs to `~/.local/bin/arduino-cli`).

- [ ] **Step 1: Install arduino-cli**

```bash
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=$HOME/.local/bin sh
export PATH="$HOME/.local/bin:$PATH"
arduino-cli version
```

Expected: prints a version string like `arduino-cli Version: 1.x.x`.

- [ ] **Step 2: Add ESP32 board support index**

```bash
arduino-cli config init --overwrite
arduino-cli config add board_manager.additional_urls \
    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli core list | grep esp32
```

Expected: `esp32:esp32 <version> Espressif Systems` appears.

- [ ] **Step 3: Install required libraries**

```bash
arduino-cli lib install "ArduinoJson"
arduino-cli lib install "TJpg_Decoder"
# Arduino_GFX must come from GitHub (per README), not Library Manager
mkdir -p ~/Arduino/libraries
if [ ! -d ~/Arduino/libraries/Arduino_GFX ]; then
    git clone https://github.com/moononournation/Arduino_GFX ~/Arduino/libraries/Arduino_GFX
fi
arduino-cli lib list | grep -iE 'arduinojson|tjpg'
```

Expected: both ArduinoJson and TJpg_Decoder listed. `Arduino_GFX` directory exists under `~/Arduino/libraries/`.

- [ ] **Step 4: Record the LilyGO T-Display-S3 build FQBN**

The firmware README mentions `Board: LilyGo T-Display-S3`. LilyGO's variant is served through the esp32:esp32 core with `esp32s3` FQBN + a custom board label; in arduino-cli we use the generic `esp32:esp32:esp32s3` with matching build flags:

```bash
ARDUINO_FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,PartitionScheme=default_8MB,FlashMode=qio,FlashFreq=80,FlashSize=16M,LoopCore=1,EventsCore=1,DebugLevel=none"
echo "$ARDUINO_FQBN" > /tmp/wyterm-fqbn
cat /tmp/wyterm-fqbn
```

Expected: the FQBN string is printed. Save to `/tmp/wyterm-fqbn` so later tasks reference it with `FQBN=$(cat /tmp/wyterm-fqbn)`.

- [ ] **Step 5: Commit nothing — this is environment setup only**

No files to commit. Move to Task 1.

---

## Task 1: Write the Python HTTP daemon (TDD)

Minimal `/health` + `/shell` daemon in stdlib-only Python. Integration-tested via shell script.

**Files:**
- Create: `daemon/wyrelay-http.py`
- Create: `daemon/test_wyrelay_http.sh`

- [ ] **Step 1: Write the failing integration test**

```bash
cat > /home/phill/WyTerminal/daemon/test_wyrelay_http.sh <<'EOF'
#!/bin/bash
# Integration test for wyrelay-http.py. Spawns daemon on :7799, curls endpoints, kills daemon.
set -e
cd "$(dirname "$0")"

PORT=7799
python3 wyrelay-http.py &
DAEMON_PID=$!
trap 'kill -9 $DAEMON_PID 2>/dev/null || true' EXIT

# Wait for daemon to bind
for i in 1 2 3 4 5; do
    sleep 0.3
    if curl -s -m 1 http://127.0.0.1:$PORT/health > /dev/null; then break; fi
done

# /health
H=$(curl -s http://127.0.0.1:$PORT/health)
[[ "$H" == "OK" ]] || { echo "FAIL /health: got '$H'"; exit 1; }
echo "PASS /health"

# /shell success
R=$(curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"cmd":"echo hello && exit 0","target":"local"}' \
    http://127.0.0.1:$PORT/shell)
echo "$R" | jq -e '.output == "hello\n" and .exit_code == 0 and .error == ""' > /dev/null || {
    echo "FAIL /shell success: got $R"; exit 1; }
echo "PASS /shell success"

# /shell non-zero exit
R=$(curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"cmd":"false","target":"local"}' http://127.0.0.1:$PORT/shell)
echo "$R" | jq -e '.exit_code == 1' > /dev/null || { echo "FAIL /shell exit: $R"; exit 1; }
echo "PASS /shell non-zero exit"

# /shell stderr capture
R=$(curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"cmd":"echo out; echo err >&2; exit 3","target":"local"}' \
    http://127.0.0.1:$PORT/shell)
echo "$R" | jq -e '.output | contains("out") and contains("err")' > /dev/null || {
    echo "FAIL /shell stderr: $R"; exit 1; }
echo "PASS /shell stderr capture"

# /shell timeout (command runs 20s, daemon should cut off at 8s)
START=$(date +%s)
R=$(curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"cmd":"sleep 20","target":"local"}' http://127.0.0.1:$PORT/shell)
ELAPSED=$(( $(date +%s) - START ))
[[ $ELAPSED -lt 12 ]] || { echo "FAIL /shell timeout: took ${ELAPSED}s"; exit 1; }
echo "$R" | jq -e '.error | test("timeout")' > /dev/null || { echo "FAIL /shell timeout err: $R"; exit 1; }
echo "PASS /shell timeout (${ELAPSED}s)"

# /shell bad json
R=$(curl -s -X POST -H 'Content-Type: application/json' \
    -d 'not-json' http://127.0.0.1:$PORT/shell)
echo "$R" | jq -e '.error | test("bad json")' > /dev/null || { echo "FAIL bad json: $R"; exit 1; }
echo "PASS /shell bad json"

# unknown path
CODE=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$PORT/nope)
[[ "$CODE" == "404" ]] || { echo "FAIL 404: got $CODE"; exit 1; }
echo "PASS unknown path 404"

echo "ALL PASS"
EOF
chmod +x /home/phill/WyTerminal/daemon/test_wyrelay_http.sh
```

- [ ] **Step 2: Run the test to confirm it fails**

```bash
cd /home/phill/WyTerminal && bash daemon/test_wyrelay_http.sh
```

Expected: fails. The daemon doesn't exist yet. Likely error: `python3: can't open file 'wyrelay-http.py'` or the test hangs waiting for `/health`.

- [ ] **Step 3: Write the daemon**

```bash
cat > /home/phill/WyTerminal/daemon/wyrelay-http.py <<'EOF'
#!/usr/bin/env python3
"""wyrelay-http — stdlib-only HTTP relay for WyTerminal USB-NCM path.

Endpoints:
  GET  /health   -> 200 "OK"
  POST /shell    -> {cmd, target} -> {output, exit_code, error}

Trust model: physical USB access == auth (see WyTerminal README).
Binds 0.0.0.0:7799 to match existing relay/wyrelay.py precedent.
"""
import http.server, json, socketserver, subprocess

PORT = 7799
TIMEOUT_S = 8
OUT_MAX = 3800

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, body, ctype='text/plain'):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def _json(self, code, obj):
        self._send(code, json.dumps(obj), 'application/json')
    def do_GET(self):
        if self.path == '/health':
            self._send(200, 'OK')
        else:
            self._send(404, 'not found')
    def do_POST(self):
        if self.path != '/shell':
            self._send(404, 'not found'); return
        n = int(self.headers.get('Content-Length', 0) or 0)
        raw = self.rfile.read(n) if n else b'{}'
        try:
            d = json.loads(raw or b'{}')
        except Exception as e:
            self._json(200, {'output':'','exit_code':-1,'error':f'bad json: {e}'}); return
        cmd = d.get('cmd', '')
        try:
            r = subprocess.run(
                ['/bin/bash','-c',cmd],
                capture_output=True, text=True, timeout=TIMEOUT_S)
            out = (r.stdout + r.stderr)[:OUT_MAX]
            self._json(200, {'output':out, 'exit_code':r.returncode, 'error':''})
        except subprocess.TimeoutExpired:
            self._json(200, {'output':'','exit_code':-1,'error':f'timeout ({TIMEOUT_S}s)'})
        except Exception as e:
            self._json(200, {'output':'','exit_code':-1,'error':str(e)})

class TS(socketserver.ThreadingMixIn, http.server.HTTPServer):
    allow_reuse_address = True
    daemon_threads = True

if __name__ == '__main__':
    TS(('0.0.0.0', PORT), H).serve_forever()
EOF
chmod +x /home/phill/WyTerminal/daemon/wyrelay-http.py
```

- [ ] **Step 4: Run the test to confirm it passes**

```bash
cd /home/phill/WyTerminal && bash daemon/test_wyrelay_http.sh
```

Expected output ends with:
```
PASS /health
PASS /shell success
PASS /shell non-zero exit
PASS /shell stderr capture
PASS /shell timeout (8s or 9s)
PASS /shell bad json
PASS unknown path 404
ALL PASS
```

- [ ] **Step 5: Verify daemon size budget**

```bash
wc -c /home/phill/WyTerminal/daemon/wyrelay-http.py
```

Expected: under 2048 bytes (the spec's 2 KB budget). If over, strip comments/docstring and retest.

- [ ] **Step 6: Commit**

```bash
cd /home/phill/WyTerminal
git add daemon/wyrelay-http.py daemon/test_wyrelay_http.sh
git commit -m "feat(daemon): stdlib-only HTTP relay for USB-NCM bootstrap

Minimal /health + /shell endpoints, no external deps. Binds :7799,
matches existing firmware relay protocol (JSON cmd/target in,
output/exit_code/error out). Integration test covers success,
non-zero exit, stderr capture, 8s timeout, bad JSON, 404."
```

---

## Task 2: Write the sync tool (py → C++ raw string)

Converts `daemon/wyrelay-http.py` into `firmware/embedded_relay.h` using a C++11 raw string literal so no escaping is required. Tiny shell script.

**Files:**
- Create: `tools/sync-embedded-relay.sh`

- [ ] **Step 1: Write the sync script**

```bash
mkdir -p /home/phill/WyTerminal/tools
cat > /home/phill/WyTerminal/tools/sync-embedded-relay.sh <<'EOF'
#!/bin/bash
# sync-embedded-relay.sh — generate firmware/embedded_relay.h from daemon/wyrelay-http.py.
# Run from repo root after editing the Python daemon.
set -e
cd "$(dirname "$0")/.."

SRC=daemon/wyrelay-http.py
DST=firmware/embedded_relay.h

if [ ! -f "$SRC" ]; then
    echo "ERROR: $SRC not found"; exit 1
fi

# Reject if source contains the raw-string terminator sentinel.
if grep -q ')PY"' "$SRC"; then
    echo "ERROR: source contains ')PY\"' — would break C++ raw string terminator. Rename the sentinel."
    exit 1
fi

cat > "$DST" <<HDR
// embedded_relay.h — AUTO-GENERATED from $SRC by tools/sync-embedded-relay.sh
// DO NOT EDIT BY HAND. Edit $SRC and re-run the sync tool.
#pragma once

const char EMBEDDED_RELAY_PY[] = R"PY(
HDR

cat "$SRC" >> "$DST"

cat >> "$DST" <<FTR
)PY";
FTR

echo "Wrote $DST ($(wc -c < "$DST") bytes from $(wc -c < "$SRC") bytes source)"
EOF
chmod +x /home/phill/WyTerminal/tools/sync-embedded-relay.sh
```

- [ ] **Step 2: Run the sync script**

```bash
cd /home/phill/WyTerminal && bash tools/sync-embedded-relay.sh
```

Expected: `Wrote firmware/embedded_relay.h (XXXX bytes from YYYY bytes source)`.

- [ ] **Step 3: Verify the generated header compiles as C++**

```bash
cat > /tmp/wy_header_test.cpp <<'EOF'
#include "embedded_relay.h"
#include <cstdio>
int main() { std::printf("len=%zu\n", sizeof(EMBEDDED_RELAY_PY)); return 0; }
EOF
g++ -I /home/phill/WyTerminal/firmware /tmp/wy_header_test.cpp -o /tmp/wy_header_test
/tmp/wy_header_test
```

Expected: prints `len=XXXX` where XXXX is ~2 KB. No compile errors. If g++ errors on unterminated raw string, the `)PY"` sentinel collided — rename the sentinel in the sync script and retry.

- [ ] **Step 4: Verify the raw string round-trips back to the original**

```bash
# Extract the Python source between the R"PY( and )PY" markers, compare to original.
cd /home/phill/WyTerminal
sed -n '/R"PY(/,/)PY";/p' firmware/embedded_relay.h | sed '1d;$d' > /tmp/wy_rt.py
diff -q daemon/wyrelay-http.py /tmp/wy_rt.py
```

Expected: `diff` produces no output (files identical). If diff shows a difference, the sync script mangled something — fix and retry.

- [ ] **Step 5: Commit the sync tool and the generated header**

```bash
cd /home/phill/WyTerminal
git add tools/sync-embedded-relay.sh firmware/embedded_relay.h
git commit -m "build: sync tool to embed daemon into firmware

tools/sync-embedded-relay.sh wraps daemon/wyrelay-http.py in a C++11
raw string literal at firmware/embedded_relay.h. Raw strings avoid
all escaping; sync checks for ')PY\"' sentinel collision.

Generated header is committed so the firmware builds standalone
without running the sync tool — re-run only after editing the
Python source."
```

---

## Task 3: Add `bootstrap_usb_relay()` to firmware

Implements the function per the spec, with setup() hook. No edits to `/deploy` or `/password` yet — those are later tasks so we can compile-test each change in isolation.

**Files:**
- Modify: `firmware/WyTerminal.ino` (add `#include`, add function, add `setup()` call)

- [ ] **Step 1: Add `#include "embedded_relay.h"` near the top of WyTerminal.ino**

Find the existing include block (around line 29-34):
```c
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <TJpg_Decoder.h>
#include "secrets.h"
```

Add `#include "embedded_relay.h"` on a new line after `#include "secrets.h"`:
```c
#include "secrets.h"
#include "embedded_relay.h"
```

- [ ] **Step 2: Add `bootstrap_usb_relay()` function**

Insert immediately after the `try_deploy_relay()` function definition (currently ends around line 213). Full function body:

```c
void bootstrap_usb_relay() {
    if (!usb_ncm_connected()) {
        term_info("no usb net");
        s_usb_relay = false;
        return;
    }
    if (check_relay(RELAY_USB_URL)) {
        s_usb_relay = true;
        term_ok("usb relay ready");
        return;
    }
    term_info("installing relay...");
    // Focus a terminal
    Keyboard.press(KEY_LEFT_CTRL); Keyboard.press(KEY_LEFT_ALT);
    Keyboard.press('t'); delay(150); Keyboard.releaseAll(); delay(1500);
    // Install daemon via heredoc (raw-string-safe)
    Keyboard.print("cat > /tmp/wyrd.py <<'WYEOF'\n");
    Keyboard.print(EMBEDDED_RELAY_PY);
    Keyboard.print("\nWYEOF\n");
    Keyboard.print("nohup python3 /tmp/wyrd.py >/dev/null 2>&1 &\n");
    // Poll /health up to 5× @ 1.5s = 7.5s
    for (int i = 0; i < 5; i++) {
        delay(1500);
        if (check_relay(RELAY_USB_URL)) {
            s_usb_relay = true;
            term_ok("relay installed");
            return;
        }
    }
    s_usb_relay = false;
    term_err("relay install failed");
    tg_send(ALLOWED_CHAT_ID,
        "\xE2\x9A\xA0 USB relay bootstrap failed.\n"
        "If target is at a login prompt, send:\n"
        "  /run <user>         (username + Enter)\n"
        "  /password <pass>    (password + Enter, hidden on AMOLED)\n"
        "  /deploy             (retry bootstrap)");
}
```

Note: `\xE2\x9A\xA0` is the UTF-8 encoding of the ⚠ character, used instead of a literal emoji to keep the `.ino` file ASCII-clean and avoid encoding surprises during Arduino preprocessing.

- [ ] **Step 3: Add the `setup()` call**

Find `setup()` around line 540-560. After the existing line:
```c
    usb_ncm_init();
    term_ok("HID + NCM ready");
```

And after the existing WiFi `check_relay(RELAY_URL)` call (which sets `s_relay_ok`), add a new line calling `bootstrap_usb_relay()`. The setup ordering must be: WiFi up → WiFi relay check → USB-NCM bootstrap → start Telegram polling.

Search for the WiFi check in setup. If the structure is:
```c
    if (check_relay(RELAY_URL)) { s_relay_ok = true; ... }
```
add immediately after that block:
```c
    bootstrap_usb_relay();
    if (s_usb_relay) s_relay_ok = true;
    draw_header();
```

The `draw_header()` repaint ensures the AMOLED header updates from `WIFI` to `USB` without waiting for the next footer redraw tick.

- [ ] **Step 4: Compile**

```bash
export PATH="$HOME/.local/bin:$PATH"
FQBN=$(cat /tmp/wyterm-fqbn)
cd /home/phill/WyTerminal/firmware
arduino-cli compile --fqbn "$FQBN" --warnings default .
```

Expected: compile succeeds with no errors. Warnings about unused statics are fine. Typical successful output ends with `Sketch uses NNNNNN bytes`.

Common failure modes to fix:
- `'KEY_LEFT_CTRL' was not declared`: `USBHIDKeyboard.h` include missing — but existing firmware already uses it, so won't happen.
- `'term_info' was not declared in this scope`: our function is placed before `term_info` is defined. Move `bootstrap_usb_relay()` below all the `term_*` helpers (around line 213 should work).
- `'ALLOWED_CHAT_ID' was not declared`: check `secrets.h` — it should define this. If named differently, match the existing reference.

- [ ] **Step 5: Commit**

```bash
cd /home/phill/WyTerminal
git add firmware/WyTerminal.ino
git commit -m "feat(firmware): bootstrap_usb_relay() with HID heredoc install

On plug-in, checks USB-NCM connected + /health on 192.168.7.1:7799.
If absent, HID-types the embedded Python daemon into /tmp/wyrd.py
via a heredoc, launches via nohup, polls /health 5× at 1.5s.

On success, sets s_usb_relay=true. On failure, sends a Telegram
hint explaining the /run /password /deploy recovery sequence.

Called once in setup() after the existing WiFi relay check.
Bootstrap does not auto-retry — user drives via /deploy to avoid
PAM lockouts from repeated typing into login prompts."
```

---

## Task 4: Edit `/deploy` command to call `bootstrap_usb_relay()` first

Existing `/deploy` (line 460) calls `try_deploy_relay()` which HID-types a `curl | bash` install — useless on a network-less target. Compose: try USB path first, fall back to curl path.

**Files:**
- Modify: `firmware/WyTerminal.ino` (edit `/deploy` branch)

- [ ] **Step 1: Locate the existing `/deploy` branch**

```bash
grep -n '/deploy' /home/phill/WyTerminal/firmware/WyTerminal.ino
```

Expected: matches around line 460 (`} else if (t=="/deploy") {`) and the help text around line 503.

- [ ] **Step 2: Replace the `/deploy` branch**

Original (current code):
```c
    } else if (t=="/deploy") {
        tg_send(chat_id,"🚀 deploying relay on target...");
        try_deploy_relay();
        // Check if it came up
        if(check_relay(RELAY_USB_URL)){
            s_usb_relay=true; s_relay_ok=true; draw_header();
            tg_send(chat_id,"✅ USB relay online!");
        } else if(check_relay(RELAY_URL)){
            s_relay_ok=true;
            tg_send(chat_id,"✅ WiFi relay reachable");
        } else {
            tg_send(chat_id,"⏳ still deploying — try /status in 30s");
        }
```

Replace with:
```c
    } else if (t=="/deploy") {
        tg_send(chat_id,"\xF0\x9F\x9A\x80 deploying relay on target...");
        // Try USB-NCM bootstrap first (offline-safe)
        bootstrap_usb_relay();
        // Fall back to curl install (WiFi-fetch) if USB path failed
        if (!s_usb_relay) try_deploy_relay();
        // Check what came up
        if(check_relay(RELAY_USB_URL)){
            s_usb_relay=true; s_relay_ok=true; draw_header();
            tg_send(chat_id,"\xE2\x9C\x85 USB relay online!");
        } else if(check_relay(RELAY_URL)){
            s_relay_ok=true;
            tg_send(chat_id,"\xE2\x9C\x85 WiFi relay reachable");
        } else {
            tg_send(chat_id,"\xE2\x8F\xB3 still deploying \xE2\x80\x94 try /status in 30s");
        }
```

The emoji bytes: 🚀 = `\xF0\x9F\x9A\x80`, ✅ = `\xE2\x9C\x85`, ⏳ = `\xE2\x8F\xB3`, — = `\xE2\x80\x94`. Matches the prior UTF-8 escape pattern from Task 3.

If the existing file uses literal UTF-8 emoji bytes (check with `file firmware/WyTerminal.ino` — should say `UTF-8 Unicode text`), you can keep the literal emoji characters and only add the one new line. Skip the UTF-8 escaping in that case.

- [ ] **Step 3: Compile**

```bash
export PATH="$HOME/.local/bin:$PATH"
FQBN=$(cat /tmp/wyterm-fqbn)
cd /home/phill/WyTerminal/firmware
arduino-cli compile --fqbn "$FQBN" .
```

Expected: compile succeeds. If `bootstrap_usb_relay` is referenced before its definition in the file, add a forward declaration near the top of the `.ino` after the other `static` declarations:
```c
void bootstrap_usb_relay();
```

- [ ] **Step 4: Commit**

```bash
cd /home/phill/WyTerminal
git add firmware/WyTerminal.ino
git commit -m "feat(firmware): /deploy tries USB-NCM bootstrap before curl fallback

Previously /deploy only ran try_deploy_relay() (HID-types a
curl|bash install which needs target-side internet). Now it calls
bootstrap_usb_relay() first — the offline-safe embedded-daemon
path — and only falls back to curl if USB-NCM is unavailable or
bootstrap fails."
```

---

## Task 5: Add `/password` command with AMOLED echo suppression

New branch in `handle_update()`. Differs from `/run` in two ways: suppresses the AMOLED echo of the argument (renders `> /password ••••` instead of `> /password hunter2`) and replies with a delete-me reminder.

**Files:**
- Modify: `firmware/WyTerminal.ino` (add `/password` branch; tweak the command echo at the top of `handle_update`)

- [ ] **Step 1: Modify the echo-at-top logic in handle_update**

Find the existing block (around line 281-283):
```c
    String t(text);
    String disp = t.length() > 30 ? t.substring(0, 29) + ">" : t;
    term_cmd(disp.c_str());
```

Replace with:
```c
    String t(text);
    String disp;
    if (t.startsWith("/password ")) {
        disp = "/password ****";
    } else {
        disp = t.length() > 30 ? t.substring(0, 29) + ">" : t;
    }
    term_cmd(disp.c_str());
```

Using ASCII `****` rather than the U+2022 bullet so the `.ino` file stays ASCII-safe without escape dances. The AMOLED renders both identically in the terminal font.

- [ ] **Step 2: Add the `/password` branch**

Place it BEFORE the `/run` branch (around line 427) since `/run` is now the last of the HID-typing commands. Find:
```c
    if (t.startsWith("/run ")) {
```

Insert immediately above:
```c
    if (t.startsWith("/password ")) {
        String pw = t.substring(10);
        hid_type(pw.c_str(), true);
        tg_send(chat_id, "\xF0\x9F\x94\x90 typed (delete this message)");
        term_ok("password typed");
        return;
    }
```

🔐 = `\xF0\x9F\x94\x90`.

- [ ] **Step 3: Add `/password` to the /help output**

Find the `/help` handler (search for `/type <text>\n` — line 501). Add `/password <pw>` to the command list next to `/run`:

Find:
```c
            "/type <text>\n"
```

Insert after it:
```c
            "/password <pw> (hidden echo, types+Enter)\n"
```

- [ ] **Step 4: Compile**

```bash
export PATH="$HOME/.local/bin:$PATH"
FQBN=$(cat /tmp/wyterm-fqbn)
cd /home/phill/WyTerminal/firmware
arduino-cli compile --fqbn "$FQBN" .
```

Expected: compile succeeds.

- [ ] **Step 5: Commit**

```bash
cd /home/phill/WyTerminal
git add firmware/WyTerminal.ino
git commit -m "feat(firmware): /password command with AMOLED echo suppression

Types password + Enter via HID for login-prompt flow. Unlike /run,
the AMOLED renders '> /password ****' instead of the plaintext —
shoulder-surf protection without needing a masked-input API in
Telegram (which doesn't exist).

Telegram reply reminds the user to delete the chat message since
the bot protocol can't mask user-side input."
```

---

## Task 6: Flash firmware and do boot sanity check

Writes the compiled firmware to the ESP32-S3 connected at `/dev/ttyACM0` and verifies the display shows the expected header.

**Files:** none (hardware interaction).

- [ ] **Step 1: Confirm device is in boot mode on driveThree**

```bash
ls /dev/ttyACM*
lsusb | grep -i 303a
```

Expected: `/dev/ttyACM0` exists, `Bus ... Device ...: ID 303a:1001 Espressif ...` visible. If the device disconnects mid-flash you may need to put it in BOOT mode by holding BOOT + tapping RESET on the T-Display-S3. Current state is already in bootable mode per prior session.

- [ ] **Step 2: Upload the compiled firmware**

```bash
export PATH="$HOME/.local/bin:$PATH"
FQBN=$(cat /tmp/wyterm-fqbn)
cd /home/phill/WyTerminal/firmware
arduino-cli upload -p /dev/ttyACM0 --fqbn "$FQBN" .
```

Expected: `esptool.py` output ending with `Leaving...` and `Hard resetting via RTS pin...`. Total upload time 30-60 seconds for a ~1 MB sketch.

- [ ] **Step 3: Verify boot by watching the serial console**

```bash
timeout 15 arduino-cli monitor -p /dev/ttyACM0 --config baudrate=115200 || true
```

Expected: see boot messages — WiFi connecting, `HID + NCM ready`, `WAIT` → `WIFI` status transitions. Kill the monitor after 15 seconds (the `timeout` handles this).

- [ ] **Step 4: Verify AMOLED display**

Look at the WyTerminal screen. Expected:
- Dark green header with `WyTerminal [local]` on the left
- Status indicator on the right: `WIFI` (green) if WiFi relay reachable, otherwise `WAIT` (red)
- Footer shows the local IP address and uptime seconds

If the AMOLED is blank or showing garbage, the display init failed. Verify flashing finished cleanly; re-flash if needed.

- [ ] **Step 5: No commit** — this is a deployment action, no file changes.

---

## Task 7: Smoke test on driveThree

Full end-to-end verification using driveThree itself as the target. Plug WyTerminal in, let bootstrap run, verify `/health` + Telegram `/shell`.

**Files:** none (manual test).

- [ ] **Step 1: Open a terminal on driveThree and leave it focused**

Open a regular desktop terminal (the firmware's HID will send `Ctrl+Alt+T` to open a new one, but starting with one already focused makes the test deterministic). The terminal window must have keyboard focus when WyTerminal is plugged in.

- [ ] **Step 2: Unplug WyTerminal from the flash-mode USB, plug back in as HID+NCM**

WyTerminal reconnects as its composite USB device (HID keyboard + CDC-NCM ethernet). Wait ~3 seconds for Linux to enumerate.

- [ ] **Step 3: Verify the USB-NCM interface came up**

```bash
ip -br addr show | grep -E '192\.168\.7|enx'
```

Expected: a line like `enx02deadbeef01 UP 192.168.7.1/24`. If no such interface, USB-NCM isn't enumerating — skip to troubleshooting in the notes section below.

- [ ] **Step 4: Watch the AMOLED during bootstrap**

Expected sequence (first 10 seconds after plug-in):
1. `WAIT` (red) — pre-WiFi
2. `WIFI` (green) — WiFi relay reachable
3. `installing relay...` on the terminal log line
4. Commands HID-type into your focused terminal: `Ctrl+Alt+T` fires (maybe opens a new terminal), then the heredoc block
5. `relay installed` (green) and header switches to `USB` (orange)

- [ ] **Step 5: Confirm daemon responds on target**

```bash
curl -s http://127.0.0.1:7799/health
echo
curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"cmd":"uname -a","target":"local"}' \
    http://127.0.0.1:7799/shell | jq .
```

Expected:
- `/health` → `OK`
- `/shell` → `{"output": "Linux driveThree ...", "exit_code": 0, "error": ""}`

- [ ] **Step 6: Telegram roundtrip**

From your Telegram chat with the WyTerminal bot, send:
```
/shell whoami
```

Expected reply in Telegram: `✅ whoami\nphill`. AMOLED log line shows `> /shell whoami` and `phill`.

- [ ] **Step 7: Idempotent replug test**

Unplug WyTerminal, wait 3 seconds, replug. Watch AMOLED.

Expected: `WAIT` → `WIFI` → `USB` within 2 seconds (no HID typing — daemon already running in `/tmp/wyrd.py` from before, so first `/health` probe returns 200 immediately).

- [ ] **Step 8: No commit** — manual test.

**If Step 3 shows no NCM interface:**

```bash
lsmod | grep cdc_ncm
dmesg | grep -i 'cdc_ncm\|02:de:ad'  # BOARD_MAC from usb_ncm.cpp
```

If `cdc_ncm` module not loaded, `sudo modprobe cdc_ncm`. If the interface appears but no IP assigned, the firmware's minimal DHCP server may not be responding — this is a firmware bug beyond the scope of this plan, but check `dmesg` for DHCP timeout errors and file an issue against `firmware/usb_ncm.cpp`.

---

## Task 8: NucBox rescue (the real test)

Primary motivation for this whole feature. Plug freshly-flashed WyTerminal into the network-less NucBox at `192.168.68.79` (currently dead, sitting at a terminal or login prompt per last session), recover shell access via Telegram, diagnose what broke the network.

**Files:** none (hardware operation).

- [ ] **Step 1: Prepare NucBox for WyTerminal**

Confirm state of the NucBox monitor:
- If at a logged-in shell prompt: easy mode, bootstrap should succeed immediately.
- If at a getty login prompt: harder mode, use `/run phill` + `/password …` + `/deploy`.
- If at GDM/LightDM: use WyTerminal's `/key ctrl+alt+F3` first to drop to a TTY, then the getty flow.

- [ ] **Step 2: Plug WyTerminal into the NucBox**

Use a direct USB-A or USB-C cable between WyTerminal and one of the NucBox's USB ports. Wait 3-5 seconds for USB enumeration and NCM DHCP handshake.

- [ ] **Step 3: Watch AMOLED for bootstrap state**

Three possible outcomes after ~10 s:

**a) AMOLED shows `USB` (orange):** bootstrap succeeded, skip to Step 6.
**b) AMOLED shows `WIFI` (green) and Telegram receives the bootstrap-failed hint:** target was at a login prompt, proceed to Step 4.
**c) AMOLED stays `WAIT` (red):** USB-NCM didn't come up on the NucBox. Try `sudo modprobe cdc_ncm` on NucBox via `/run` (but without bootstrap working, you can't get output back — just type it and hope). If still stuck, fall through to curl-install path via `/deploy`, but that needs NucBox internet which is currently dead, so the real recovery is to physically attend to the NucBox.

- [ ] **Step 4: Drive the login flow from Telegram (case b)**

Send, one at a time:
```
/run phill
/password <your-nucbox-password>
/deploy
```

After the third message, watch AMOLED. Expected: `WAIT` → `installing relay...` → `relay installed` → `USB`. Delete the `/password` message from Telegram chat after confirming bootstrap succeeded.

- [ ] **Step 5: Verify shell round-trip from Telegram**

```
/shell hostname
/shell ip link
/shell systemctl is-enabled hermes-gateway ollama ollama-rag ollama-vision
```

Expected: replies return in Telegram. `hostname` should be `phill-NucBox-K8-Plus`. `ip link` should show the `enx…` NCM interface we just brought up plus the broken wifi/LAN. `is-enabled` should show all four units in `disabled` state (confirmed from the last session).

- [ ] **Step 6: Diagnose the network**

From Telegram (each as a separate `/shell` message):
```
/shell ip -br addr
/shell systemctl status NetworkManager --no-pager | head -15
/shell journalctl -u NetworkManager -b --no-pager | tail -25
/shell dmesg | grep -iE 'eth|wlan|r8169|igc|iwl|mt7' | tail -20
```

Read the replies; plan the fix based on what NetworkManager reports.

- [ ] **Step 7: Apply the fix**

Likely candidates (from last session's theory list): NetworkManager state corruption (`rm /var/lib/NetworkManager/NetworkManager.state` + restart), driver reload (`modprobe -r <drv> && modprobe <drv>`), or dhclient lease clear. Issue the fix via `/shell` and verify with `/shell ip addr` that an interface got an IP.

- [ ] **Step 8: Confirm NucBox is reachable from driveThree again**

From driveThree:
```bash
ssh -o ConnectTimeout=5 phill@192.168.68.79 'uptime'
```

Expected: the usual `load average: ...` line. If this works, the rescue is complete and you can unplug WyTerminal.

- [ ] **Step 9: Record the root cause in memory**

Write a concise note to memory about what actually broke the NucBox's networking and what the fix was. No file commit in this repo — just memory.

---

## Notes for the implementing engineer

- **Why C++11 raw strings instead of escaping**: the C++11 `R"DELIM(...)DELIM"` raw string literal needs no escaping of any character except the exact sequence `)DELIM"`. The sync tool checks for `)PY"` in the source and aborts — so you can write any Python code inside the daemon without worrying about backslashes, quotes, or newlines.
- **Why `nohup python3 … &` instead of a systemd service**: the spec deliberately scopes persistent installation out. `/tmp/wyrd.py` dies at reboot — correct for a rescue daemon. If a user wants permanence, they install the full `relay/wyrelay.py` via the existing WiFi-curl path.
- **Why `bootstrap_usb_relay()` doesn't auto-retry**: repeated HID-typing into a login prompt looks like a brute-force attempt and triggers PAM lockouts (default: 3 failures = 30 min lockout, per `pam_faillock`). The user drives retries manually via `/deploy` after authenticating with `/run` + `/password`.
- **Why `/password` hides the AMOLED echo but not the Telegram echo**: Telegram doesn't have a "masked input" API for user-sent messages. The secret lands in chat history either way; the best we can do is remind the user to delete it and protect the physically-visible AMOLED display.
- **What to do if arduino-cli compile fails with a cryptic esp32 error**: the board package version must match what the firmware's existing code expects. If issues, `arduino-cli core install esp32:esp32@3.0.7` (a known-good pinned version as of this spec's date). Update this note if a newer known-good version ships.

---

## Spec coverage self-review

Walking through each §/requirement of the spec and the task that implements it:

| Spec requirement | Task |
|---|---|
| Goal 1: within ~10 s of plug-in, `/shell` works via USB-NCM | Tasks 3+7 (bootstrap fn + smoke test timing) |
| Goal 2: no pre-install beyond Python 3 | Task 1 (stdlib-only daemon) |
| Goal 3: no target-side internet | Tasks 1-3 (embedded not downloaded) |
| Goal 4: graceful WiFi-relay fallback | Task 3 Step 2 (s_usb_relay=false path) |
| Goal 5: login-prompt flow via /run /password /deploy | Tasks 4+5 (/deploy edit, /password add) |
| Non-goal: no credential auto-typing | Task 3 bootstrap function contains no credential handling |
| Non-goal: no prompt visibility | Not implemented (explicit out-of-scope) |
| Non-goal: no persistent install | Task 1 daemon lives in `/tmp`, not `/usr/local` |
| Components §1: embedded_relay.h | Task 2 (sync tool generates it) |
| Components §2: WyTerminal.ino edits | Tasks 3, 4, 5 |
| Components §3: wyrelay-http.py | Task 1 |
| Data flow bootstrap sequence | Task 3 Step 2 (verbatim pseudo-code) |
| Data flow login-prompt flow | Task 8 Step 4 (operational test of the flow) |
| Error: user at login | Task 8 Step 4 tests this |
| Error: port already bound | Task 3 Step 2 first `check_relay` handles this |
| Error: daemon crashes mid-session | Task 4's composed `/deploy` lets user re-bootstrap |
| Error: NCM driver not loaded | Task 3 Step 2 `usb_ncm_connected()` guard |
| Testing: cold bootstrap | Task 7 Step 4 |
| Testing: idempotent replug | Task 7 Step 7 |
| Testing: no interactive shell | Task 8 covers (NucBox may be at login) |
| Testing: Telegram E2E | Task 7 Step 6 |
| Testing: daemon-kill fallback | Not explicitly tested in the plan — add as a followup note if time permits. |
| Testing: NucBox rescue | Task 8 (the whole task) |

**Gap found:** daemon-kill fallback (spec Testing row 5) is documented in the smoke-test table of the spec but not explicitly executed in this plan. It's a 2-minute test the engineer should do ad-hoc during Task 7 — noting here rather than adding a task because the existing code path (lines 232-236 of WyTerminal.ino) already implements it and hasn't changed.
