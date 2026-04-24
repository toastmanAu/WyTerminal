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
