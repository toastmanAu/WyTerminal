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
