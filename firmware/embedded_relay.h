// embedded_relay.h — AUTO-GENERATED from daemon/wyrelay-http.py by tools/sync-embedded-relay.sh
// DO NOT EDIT BY HAND. Edit daemon/wyrelay-http.py and re-run the sync tool.
#pragma once

const char EMBEDDED_RELAY_PY[] = R"PY(
#!/usr/bin/env python3
"""wyrelay-http - stdlib-only HTTP relay for WyTerminal USB-NCM path.

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
)PY";
