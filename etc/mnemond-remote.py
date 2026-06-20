import sys
import json
import time
import requests

SERVER = "http://tars.uberadmin.com:3847/mcp"
#TOKEN = "yourkey"
#CA_CERT = r"path_to_your_pem"

session = requests.Session()
session.headers.update({
    "Content-Type": "application/json"
#    "Authorization": f"Bearer {TOKEN}"
})
#session.verify = CA_CERT

# (connect, read) timeout in seconds. The read timeout stays under Claude
# Desktop's ~4-minute ceiling so a stalled call surfaces a real JSON-RPC error
# instead of an opaque "local MCP server may be unresponsive" hang.
TIMEOUT = (5, 120)


def _log(msg):
    sys.stderr.write("mnemond shim: " + msg + "\n")
    sys.stderr.flush()


mcp_session_id = None

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        msg = json.loads(line)  # validate it's JSON

        # Include Mcp-Session-Id on all requests after initialize
        headers = {}
        if mcp_session_id:
            headers["Mcp-Session-Id"] = mcp_session_id

        req_id = msg.get("id")
        method = msg.get("method")

        # Time the round trip; a timeout returns a JSON-RPC error rather than
        # blocking the whole shim (and Desktop) indefinitely.
        t0 = time.monotonic()
        try:
            r = session.post(SERVER, data=line, headers=headers,
                             timeout=TIMEOUT)
        except requests.exceptions.RequestException as e:
            elapsed = (time.monotonic() - t0) * 1000.0
            _log(f"POST FAILED method={method} id={req_id} "
                 f"after={elapsed:.0f}ms err={type(e).__name__}: {e}")
            if req_id is not None:
                sys.stdout.write(json.dumps({
                    "jsonrpc": "2.0",
                    "id": req_id,
                    "error": {"code": -32001,
                              "message": f"shim transport error: {e}"}
                }) + "\n")
                sys.stdout.flush()
            continue

        # Read body as bytes (avoids requests' charset auto-detection on .text,
        # which scans the whole body) and record round-trip time + size.
        body = r.content
        elapsed = (time.monotonic() - t0) * 1000.0
        _log(f"method={method} id={req_id} status={r.status_code} "
             f"bytes={len(body)} elapsed={elapsed:.0f}ms")

        # Capture Mcp-Session-Id from initialize response
        if "Mcp-Session-Id" in r.headers:
            mcp_session_id = r.headers["Mcp-Session-Id"]

        # Convert HTTP errors to JSON-RPC error responses
        if r.status_code >= 400:
            err_resp = {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {
                    "code": -32000,
                    "message": f"HTTP {r.status_code}: "
                               f"{body.decode('utf-8', 'replace').strip()}"
                }
            }
            # Only send error for requests (not notifications)
            if req_id is not None:
                sys.stdout.write(json.dumps(err_resp) + "\n")
                sys.stdout.flush()
            continue

        response = body.decode("utf-8", "replace").strip()
        if response:
            sys.stdout.write(response + "\n")
            sys.stdout.flush()
    except Exception as e:
        sys.stderr.write(f"mnemond shim error: {e}\n")
        sys.stderr.flush()
