# WyTerminal — USB-NCM relay self-bootstrap

**Date:** 2026-04-24
**Status:** Design approved, ready for implementation plan
**Target firmware:** v3.x → v3.1

## Context

WyTerminal v3 firmware already supports a relay architecture for shell execution: ESP32 handles Telegram over WiFi, and POSTs `{cmd, target}` to an HTTP relay that runs shell and returns `{output, exit_code, error}`. `active_relay()` prefers a USB-NCM relay at `http://192.168.7.1:7799` over the WiFi relay URL.

The USB-NCM path has one gap: **nothing on the target machine listens on port 7799 unless a relay daemon has been pre-installed.** For typical use (a Pi or long-lived dev box) that's fine — the daemon runs permanently. For the rescue use case (plug WyTerminal into a machine whose network has collapsed and recover it via Telegram), there is no pre-install and no working internet to fetch one.

This feature closes that gap by having the firmware **self-bootstrap a minimal target-side daemon via HID keyboard injection** when `/health` does not respond.

## Goals

1. Plug WyTerminal into a Linux machine at an interactive shell prompt; within ~10 seconds, `/shell` commands from Telegram return structured results via the USB-NCM relay path.
2. Zero pre-installation on the target beyond stock Python 3 (already present on every modern Linux).
3. Zero target-side internet requirement — the daemon is embedded in firmware, not downloaded.
4. Graceful fallback: if bootstrap fails (user at login screen, Python missing, NCM driver absent), the firmware continues to operate via the existing WiFi relay with no regression.

## Non-goals

- Windows, macOS, or BSD target support. Linux only.
- Bypassing login prompts. User must be at an interactive shell when WyTerminal is plugged in; the HID sequence does not type credentials.
- Persistent installation. The daemon lives in `/tmp` and dies at reboot. Users who want permanence can install the full `relay/wyrelay.py` via the existing WiFi-fetch path.
- Screenshot, upload, clipboard, `/sysinfo`, `/ps` endpoints — the embedded daemon implements only `/health` + `/shell`. Extended endpoints can follow in v3.2.

## Architecture

The existing firmware relay flow is unchanged. One new step is inserted between "WiFi connected" and "start polling Telegram":

```
WiFi up
  ↓
check_relay(WIFI_URL)  →  s_wifi_relay_ok         [existing]
  ↓
bootstrap_usb_relay()                              [NEW]
  ├─ usb_ncm_connected()?      no → skip, s_usb_relay=false
  ├─ GET 192.168.7.1:7799/health
  │     200 OK → s_usb_relay=true (already installed or previous install survived replug)
  │     fail → HID-type daemon install, poll /health 5× @ 1.5s
  └─ still no /health → s_usb_relay=false, fall back to WiFi relay
  ↓
poll_telegram() loop                               [existing, unchanged]
```

`active_relay()`, `relay_shell()`, and `handle_update()` are all untouched. On success, header shows `USB` (orange); on fallback, `WIFI` (green). If neither relay is reachable, `WAIT` (red) — same as current behavior.

## Components

### 1. `firmware/embedded_relay.h` (new)

Header holding the target-side daemon source as a single `const char EMBEDDED_RELAY_PY[]` string literal. Separate file so the Python source stays readable and diffable, not buried inline in the `.ino` with escaped quotes.

Generated from `daemon/wyrelay-http.py` (see §3) by a small build helper so we edit Python, not a C string.

### 2. `firmware/WyTerminal.ino` (edit)

Add `bootstrap_usb_relay()` function (see Data Flow below). Call it once in `setup()` after the existing `usb_ncm_init()` call and after the WiFi-relay `check_relay()` call. No other edits.

### 3. `daemon/wyrelay-http.py` (new)

Canonical Python source for the embedded daemon. Constraints:

- **Stdlib only.** No pip, no venv. Uses `http.server`, `subprocess`, `json`.
- **Single file, under 2 KB.** At HID typing rate (~50 chars/s) the install should complete in under 30 seconds.
- **Binds `0.0.0.0:7799`.** Reachable via both the USB-NCM interface (`192.168.7.1`) and loopback (useful for local dev testing). Matches the existing `relay/wyrelay.py` bind precedent — physical USB access remains the auth factor per the project's stated trust model.
- **Endpoints:**
  - `GET /health` → `200 OK`, body `OK`
  - `POST /shell` → accepts `{cmd, target}`, returns `{output, exit_code, error}`. Ignores `target` (runs locally). Shell is `/bin/bash -c`. Timeout 8 seconds (firmware uses 10s, leaves margin). Output capped at 3800 bytes to match firmware truncation.
- **Logs silently.** No stdout/stderr noise — HID-install pipes to `/dev/null`.

A tiny build step (make target or pre-commit) copies `daemon/wyrelay-http.py` into `firmware/embedded_relay.h` as a C string so the two stay in sync. We edit Python, not C.

## Data flow — bootstrap sequence

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
    // Install daemon via heredoc
    Keyboard.print("cat > /tmp/wyrd.py <<'WYEOF'\n");
    Keyboard.print(EMBEDDED_RELAY_PY);  // the embedded Python source
    Keyboard.print("\nWYEOF\n");
    Keyboard.print("nohup python3 /tmp/wyrd.py >/dev/null 2>&1 &\n");
    // Poll for health
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
}
```

The heredoc approach avoids all quoting complexity (no shell escapes needed inside `EMBEDDED_RELAY_PY`) and handles multi-line Python cleanly. `WYEOF` is unlikely to collide with anything the daemon source contains.

## Error handling

| Scenario | Detection | Behaviour |
|---|---|---|
| User not at shell prompt (login screen, sleep, etc.) | 5× `/health` probes all fail | `s_usb_relay=false`, fall back to WiFi relay, header shows `WIFI` |
| Python 3 not in PATH | Same as above (daemon never binds) | Same fallback |
| Port 7799 already bound | First `/health` probe returns 200 | Skip HID install, reuse existing daemon (idempotent) |
| Daemon crashes mid-session | `relay_shell()` receives `"relay unreachable"` | Existing code clears `s_usb_relay`, retries via WiFi (line 235 in current .ino) |
| USB-NCM driver not loaded on host | `usb_ncm_connected()` returns false | Skip bootstrap entirely, use WiFi relay |
| `cdc_ncm` kernel module absent (very old kernel) | Same as above | Same fallback |
| AMOLED feedback | Always visible on device header | `WAIT` → `WIFI` → `USB` state machine |

## Testing

Manual only. No unit-test infrastructure exists in this firmware today, and adding one for a one-function change isn't justified.

| Test | Setup | Expected |
|---|---|---|
| Cold bootstrap | Freshly-flashed firmware, plug into driveThree at terminal prompt | AMOLED: `WAIT` → `WIFI` → `USB` within 10s. `curl localhost:7799/health` from target returns `OK`. |
| Idempotent replug | Unplug, replug into same machine (daemon still in `/tmp`) | Straight to `USB` in <2s, no HID typing. |
| No interactive shell | Plug into machine at GDM/login screen | AMOLED stays `WIFI` after retries, no crash, WiFi relay still works. |
| Telegram end-to-end | `/shell uname -a` from Telegram after `USB` appears | Response in Telegram, correct exit code, AMOLED shows command + first lines of output. |
| Fallback on daemon kill | `pkill -f wyrd.py` on target, then `/shell` | First `/shell` fails on USB, firmware clears `s_usb_relay`, retries via WiFi, response via WiFi path. |
| NucBox rescue (live) | Plug freshly-flashed unit into network-less NucBox at login shell | Full roundtrip works, shell commands execute and return to Telegram. Confirms original mission. |

## Open questions / known risks

1. **HID timing reliability.** Arduino `Keyboard.print()` can drop characters on some USB hosts when piping large strings. We mitigate by using `Keyboard.write()` with small inter-character delays if drops appear during testing. Worst case: break the heredoc into smaller chunks.
2. **Terminal-application side effects from heredoc typing.** If the shell has rich line-editing (e.g., fish, or bash with an exotic `.bashrc`), tab-completion could fire on partial paths. We mitigate by including a leading `set +o histexpand; ` if issues appear, but start without it and add only if testing reveals a problem.
3. **Auto-deploy path collision with existing `try_deploy_relay()`.** The existing function (line 202 of WyTerminal.ino) types a `curl … install.sh | bash` command. That path needs internet. `bootstrap_usb_relay()` is the no-internet equivalent. They are alternative code paths called in different branches; no collision.
4. **Sync between `daemon/wyrelay-http.py` and `firmware/embedded_relay.h`.** The build-time sync step is plan-level detail, not design-level. Resolved in the implementation plan.

## Out of scope — deferred to later

- `/screenshot`, `/upload`, `/clipboard`, `/sysinfo`, `/ps` endpoints in the embedded daemon. These depend on target-side binaries (scrot, xclip) and are best served by the full WiFi-relay path, not the minimal HID-bootstrap daemon.
- Windows / macOS / BSD target support.
- Persistent systemd installation of the embedded daemon.
- Multi-user / security hardening. The physical USB access is the auth factor, matching the existing WyTerminal trust model.
