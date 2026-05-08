import sys
import json
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

        r = session.post(SERVER, data=line, headers=headers)

        # Capture Mcp-Session-Id from initialize response
        if "Mcp-Session-Id" in r.headers:
            mcp_session_id = r.headers["Mcp-Session-Id"]

        # Convert HTTP errors to JSON-RPC error responses
        if r.status_code >= 400:
            req_id = msg.get("id")
            err_resp = {
                "jsonrpc": "2.0",
                "id": req_id,
                "error": {
                    "code": -32000,
                    "message": f"HTTP {r.status_code}: {r.text.strip()}"
                }
            }
            # Only send error for requests (not notifications)
            if req_id is not None:
                sys.stdout.write(json.dumps(err_resp) + "\n")
                sys.stdout.flush()
            continue

        response = r.text.strip()
        if response:
            sys.stdout.write(response + "\n")
            sys.stdout.flush()
    except Exception as e:
        sys.stderr.write(f"mnemond shim error: {e}\n")
        sys.stderr.flush()
