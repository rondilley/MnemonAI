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
# Cache the initialize request so we can transparently re-establish a session
# if the server reaps ours (idle timeout / daemon restart). Without this, every
# request after expiry returns 404 "session expired" until Desktop restarts.
init_line = None


def _do_post(line):
    """POST one JSON-RPC line with the current session header. Returns the
    requests.Response or raises requests.exceptions.RequestException."""
    headers = {}
    if mcp_session_id:
        headers["Mcp-Session-Id"] = mcp_session_id
    return session.post(SERVER, data=line, headers=headers, timeout=TIMEOUT)


def _is_session_expired(resp):
    if resp.status_code != 404:
        return False
    try:
        return "session" in resp.content.decode("utf-8", "replace").lower()
    except Exception:
        return True  # any 404 on a non-initialize request -> treat as expired


def _reinitialize():
    """Replay the cached initialize to get a fresh session id. Returns True on
    success."""
    global mcp_session_id
    if not init_line:
        return False
    try:
        # Send initialize with no session header so the server mints a new one.
        r = session.post(SERVER, data=init_line,
                         headers={}, timeout=TIMEOUT)
    except requests.exceptions.RequestException as e:
        _log(f"reinitialize failed: {type(e).__name__}: {e}")
        return False
    if r.status_code < 400 and "Mcp-Session-Id" in r.headers:
        mcp_session_id = r.headers["Mcp-Session-Id"]
        _log(f"session re-established: {mcp_session_id}")
        return True
    _log(f"reinitialize got status={r.status_code} (no new session)")
    return False


for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        msg = json.loads(line)  # validate it's JSON
        req_id = msg.get("id")
        method = msg.get("method")

        # Remember the initialize request so we can replay it on session loss.
        if method == "initialize":
            init_line = line

        # Time the round trip; a timeout returns a JSON-RPC error rather than
        # blocking the whole shim (and Desktop) indefinitely.
        t0 = time.monotonic()
        try:
            r = _do_post(line)
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

        # If our session was reaped, transparently re-establish it and retry the
        # original request once -- Desktop never sees the disruption.
        if _is_session_expired(r) and method != "initialize" and _reinitialize():
            try:
                r = _do_post(line)
            except requests.exceptions.RequestException as e:
                _log(f"retry after reinit failed: {type(e).__name__}: {e}")

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
