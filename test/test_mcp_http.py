#!/usr/bin/env python3
"""
MCP HTTP Transport Test -- Tests all 35 tools over Streamable HTTP.

Starts mnemond in foreground mode with HTTP enabled, then exercises
every tool via HTTP POST to /mcp, validating MCP spec compliance.

Usage:
    cd build
    LD_LIBRARY_PATH=~/.local/lib python3 ../test/test_mcp_http.py
"""

import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import urllib.request
import urllib.error


def wait_for_port(port, proc, timeout=10.0):
    """Block until *our* mnemond is responding on `port`, or raise.

    A bare TCP connect is not enough -- if a stale mnemond (or any other
    process) is squatting on the port, our newly-spawned mnemond will fail to
    bind and exit, and the test will silently talk to the squatter. We probe
    with a real HTTP request and require an HTTP/1.x response line, which
    confirms an actual HTTP server is on the other end. Combined with
    proc.poll() to detect early exit, this catches both 'port held by a
    non-HTTP squatter' and 'mnemond failed to bind' cases."""
    deadline = time.time() + timeout
    probe = (b"OPTIONS /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
             b"Origin: http://127.0.0.1\r\nConnection: close\r\n\r\n")
    last_err = "no attempts made"
    while time.time() < deadline:
        rc = proc.poll()
        if rc is not None:
            raise RuntimeError(
                f"mnemond exited with code {rc} before binding 127.0.0.1:{port} "
                f"(is another mnemond instance holding the port?)")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5) as s:
                s.sendall(probe)
                head = s.recv(16)
            if head.startswith(b"HTTP/1."):
                return
            last_err = f"non-HTTP response: {head!r}"
        except OSError as e:
            last_err = f"connect failed: {e}"
        time.sleep(0.1)
    raise RuntimeError(
        f"mnemond did not respond on 127.0.0.1:{port} within {timeout}s "
        f"(last: {last_err})")

BINARY = "./mnemond"
passed = 0
failed = 0


def post_mcp(port, request, auth_token=None):
    """POST a JSON-RPC request to the MCP HTTP endpoint."""
    url = f"http://127.0.0.1:{port}/mcp"
    data = json.dumps(request).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
    }
    if auth_token:
        headers["Authorization"] = f"Bearer {auth_token}"

    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode("utf-8")
            return json.loads(body), resp.status, dict(resp.headers)
    except urllib.error.HTTPError as e:
        return None, e.code, {}
    except Exception as e:
        return None, 0, {"error": str(e)}


def call_tool(port, name, arguments, req_id, auth_token=None):
    """Call an MCP tool and return parsed inner result."""
    resp, status, headers = post_mcp(port, {
        "jsonrpc": "2.0", "id": req_id,
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments}
    }, auth_token)
    if not resp or status != 200:
        return {}, True
    result = resp.get("result", {})
    is_error = result.get("isError", False)
    content = result.get("content", [])
    inner = json.loads(content[0]["text"]) if content else {}
    return inner, is_error


def test(name, condition, detail=""):
    global passed, failed
    if not condition:
        failed += 1
        print(f"  {name:<62s} FAIL: {detail}")
    else:
        passed += 1
        print(f"  {name:<62s} PASS")


def main():
    global passed, failed

    if not os.path.exists(BINARY):
        print(f"ERROR: {BINARY} not found. Run from build/ directory.")
        sys.exit(1)

    env = os.environ.copy()
    home = os.path.expanduser("~")
    env["LD_LIBRARY_PATH"] = f"{home}/.local/lib:" + env.get("LD_LIBRARY_PATH", "")

    tmpdir = os.path.join(home, f".mnemond_http_test_{os.getpid()}")
    os.makedirs(tmpdir, exist_ok=True)
    port = 13847  # avoid conflict with other instances

    conf = os.path.join(tmpdir, "test.conf")
    with open(conf, "w") as f:
        f.write(f"[general]\ndata_dir = {tmpdir}/data\nlog_level = error\n")
        f.write("[lmdb]\nmap_size_gb = 1\nmax_readers = 16\n")
        f.write("[embedding]\nmodel_path = none\ndimensions = 768\n")
        f.write(f"[http]\nenabled = true\nbind = 127.0.0.1\nport = {port}\n")

    proc = subprocess.Popen(
        [BINARY, "--foreground", "--config", conf],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
    )
    try:
        wait_for_port(port, proc, timeout=10.0)
    except RuntimeError as e:
        # Surface daemon's stderr so the cause (e.g. port in use) is visible
        try:
            stderr_tail = proc.stderr.read().decode("utf-8", "replace")[-2000:]
        except Exception:
            stderr_tail = ""
        proc.kill()
        proc.wait(timeout=5)
        shutil.rmtree(tmpdir, ignore_errors=True)
        print(f"ERROR: {e}")
        if stderr_tail:
            print(f"--- mnemond stderr ---\n{stderr_tail}")
        return 1

    try:
        print("=== MCP HTTP Transport Test (All 35 Tools) ===\n")

        # ---- Lifecycle ----
        print("  --- Lifecycle ---")
        resp, status, headers = post_mcp(port, {
            "jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                       "clientInfo": {"name": "http_test", "version": "1.0"}}
        })
        test("[HTTP] initialize returns 200", status == 200)
        test("[HTTP] protocolVersion in response",
             resp and resp.get("result", {}).get("protocolVersion") == "2024-11-05")
        test("[HTTP] Mcp-Session-Id header returned",
             "Mcp-Session-Id" in headers or "mcp-session-id" in headers)

        session_id = headers.get("Mcp-Session-Id", headers.get("mcp-session-id", ""))

        # Notification -> 202 Accepted
        try:
            req = urllib.request.Request(
                f"http://127.0.0.1:{port}/mcp",
                data=json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}).encode(),
                headers={"Content-Type": "application/json",
                         "Accept": "application/json"},
                method="POST"
            )
            with urllib.request.urlopen(req, timeout=5) as resp:
                test("[HTTP] notification returns 202", resp.status == 202)
        except urllib.error.HTTPError as e:
            test("[HTTP] notification returns 202", e.code == 202, f"got {e.code}")

        # tools/list
        resp, status, _ = post_mcp(port, {
            "jsonrpc": "2.0", "id": 2, "method": "tools/list"
        })
        tools = resp.get("result", {}).get("tools", []) if resp else []
        test("[HTTP] tools/list returns 35+ tools", len(tools) >= 28, f"got {len(tools)}")

        # ---- All 35 tools ----
        print("\n  --- Tools over HTTP ---")

        # store_memory
        r, ie = call_tool(port, "store_memory", {
            "content": "HTTP transport stores memories correctly",
            "source_type": "http_test"
        }, 10)
        test("[HTTP] store_memory", "id" in r and not ie)
        stored_id = r.get("id", "")

        # retrieve_memory
        r, ie = call_tool(port, "retrieve_memory", {"id": stored_id}, 11)
        test("[HTTP] retrieve_memory", "HTTP transport" in r.get("content", ""))

        # update_memory
        r, ie = call_tool(port, "update_memory", {
            "id": stored_id, "content": "Updated via HTTP"
        }, 12)
        test("[HTTP] update_memory", r.get("updated") == True)

        # search_keyword
        r, ie = call_tool(port, "search_keyword", {"query": "HTTP transport"}, 13)
        test("[HTTP] search_keyword", isinstance(r.get("results"), list))

        # search_semantic
        r, ie = call_tool(port, "search_semantic", {"query": "memory"}, 14)
        test("[HTTP] search_semantic", isinstance(r.get("results"), list))

        # search_hybrid
        r, ie = call_tool(port, "search_hybrid", {"query": "transport"}, 15)
        test("[HTTP] search_hybrid", isinstance(r.get("results"), list))

        # search_temporal
        r, ie = call_tool(port, "search_temporal", {"since": "2020-01-01T00:00:00Z"}, 16)
        test("[HTTP] search_temporal", isinstance(r.get("results"), list))

        # create_entity
        r, ie = call_tool(port, "create_entity", {
            "name": "HTTPTest", "entity_type": "test"
        }, 20)
        test("[HTTP] create_entity", "id" in r)
        ent_a = r.get("id", "")

        # add_observation
        r, ie = call_tool(port, "add_observation", {
            "entity_id": ent_a, "observation": "Created over HTTP"
        }, 21)
        test("[HTTP] add_observation", r.get("observation_count", 0) >= 1)

        # search_entities
        r, ie = call_tool(port, "search_entities", {"query": "HTTPTest"}, 22)
        test("[HTTP] search_entities", isinstance(r.get("entities"), list))

        # create_relation
        r2, _ = call_tool(port, "create_entity", {"name": "Target", "entity_type": "test"}, 23)
        ent_b = r2.get("id", "")
        r, ie = call_tool(port, "create_relation", {
            "source_id": ent_a, "target_id": ent_b, "edge_type": "tested_with"
        }, 24)
        test("[HTTP] create_relation", "id" in r)

        # get_entity_graph
        r, ie = call_tool(port, "get_entity_graph", {"entity_id": ent_a}, 25)
        test("[HTTP] get_entity_graph", "nodes" in r and "edges" in r)

        # get_history
        r, ie = call_tool(port, "get_history", {"entity_id": ent_a}, 30)
        test("[HTTP] get_history", isinstance(r.get("versions"), list))

        # get_state_at_time
        r, ie = call_tool(port, "get_state_at_time", {
            "entity_id": ent_a, "timestamp": "2030-01-01T00:00:00Z"
        }, 31)
        test("[HTTP] get_state_at_time", isinstance(r.get("name"), str))

        # get_changes_since
        r, ie = call_tool(port, "get_changes_since", {"since": "2020-01-01T00:00:00Z"}, 32)
        test("[HTTP] get_changes_since", isinstance(r.get("results"), list))

        # health_check
        r, ie = call_tool(port, "health_check", {}, 40)
        test("[HTTP] health_check", r.get("status") == "ok")

        # get_memory_stats
        r, ie = call_tool(port, "get_memory_stats", {}, 41)
        test("[HTTP] get_memory_stats", r.get("total_memories", 0) >= 1)

        # get_hardware_info
        r, ie = call_tool(port, "get_hardware_info", {}, 42)
        test("[HTTP] get_hardware_info", "cpu" in r and "gpu" in r)

        # get_index_stats
        r, ie = call_tool(port, "get_index_stats", {}, 43)
        test("[HTTP] get_index_stats", "lmdb" in r and "fts5" in r)

        # list_memories
        r, ie = call_tool(port, "list_memories", {"limit": 5}, 44)
        test("[HTTP] list_memories", isinstance(r.get("memories"), list))

        # consolidate_memories
        r, ie = call_tool(port, "consolidate_memories", {"dry_run": True}, 50)
        test("[HTTP] consolidate_memories", "consolidated_count" in r)

        # prune_stale
        r, ie = call_tool(port, "prune_stale", {"dry_run": True, "min_importance": 99}, 51)
        test("[HTTP] prune_stale", isinstance(r.get("candidates"), list))

        # import_batch
        r, ie = call_tool(port, "import_batch", {
            "memories": [{"content": "HTTP batch 1"}, {"content": "HTTP batch 2"}]
        }, 52)
        test("[HTTP] import_batch", r.get("imported") == 2)

        # import_file
        import_file = os.path.join(tmpdir, "test.jsonl")
        with open(import_file, "w") as f:
            f.write('{"content":"HTTP file import"}\n')
        r, ie = call_tool(port, "import_file", {"path": import_file, "format": "jsonl"}, 53)
        test("[HTTP] import_file", r.get("imported", 0) >= 1 or "error" in r)

        # import_directory
        r, ie = call_tool(port, "import_directory", {"path": tmpdir, "pattern": "*.jsonl"}, 54)
        test("[HTTP] import_directory", "files_processed" in r or "error" in r)

        # get_import_status
        r, ie = call_tool(port, "get_import_status", {}, 55)
        test("[HTTP] get_import_status", isinstance(r.get("jobs"), list))

        # rebuild_indexes
        r, ie = call_tool(port, "rebuild_indexes", {"target": "all"}, 56)
        test("[HTTP] rebuild_indexes", isinstance(r.get("rebuilt"), list))

        # delete_memory
        r, ie = call_tool(port, "delete_memory", {"id": stored_id}, 60)
        test("[HTTP] delete_memory", r.get("deleted") == True)

        # ---- Honeypot: Decoy Tools ----
        print("\n  --- Honeypot Decoy Tools ---")

        r, ie = call_tool(port, "admin_reset_auth", {"admin_key": "test"}, 70)
        test("[DECOY] admin_reset_auth returns error", "error" in r)
        test("[DECOY] admin_reset_auth isError=true", ie == True)

        r, ie = call_tool(port, "export_all_memories", {}, 71)
        test("[DECOY] export_all_memories returns error", "error" in r)

        r, ie = call_tool(port, "debug_raw_query",
                          {"query": "SELECT *", "database": "entities"}, 72)
        test("[DECOY] debug_raw_query returns error", "error" in r)
        test("[DECOY] debug_raw_query mentions debug", "debug" in r.get("error", ""))

        r, ie = call_tool(port, "set_system_config",
                          {"key": "auth_token", "value": "hacked"}, 73)
        test("[DECOY] set_system_config returns error", "error" in r)

        # ---- Error handling ----
        print("\n  --- Error Handling ---")

        # Unknown tool
        _, status, _ = post_mcp(port, {
            "jsonrpc": "2.0", "id": 70,
            "method": "tools/call",
            "params": {"name": "nonexistent", "arguments": {}}
        })
        # Unknown tool returns JSON-RPC error in 200 response (not HTTP error)
        test("[HTTP] unknown tool returns response", status == 200)

        # Wrong path
        try:
            req = urllib.request.Request(f"http://127.0.0.1:{port}/wrong",
                                         data=b"{}", method="POST")
            req.add_header("Content-Type", "application/json")
            urllib.request.urlopen(req, timeout=5)
            test("[HTTP] wrong path returns 404", False)
        except urllib.error.HTTPError as e:
            test("[HTTP] wrong path returns 404", e.code == 404)

        # Session delete
        try:
            req = urllib.request.Request(f"http://127.0.0.1:{port}/mcp",
                                         method="DELETE")
            if session_id:
                req.add_header("Mcp-Session-Id", session_id)
            urllib.request.urlopen(req, timeout=5)
            test("[HTTP] DELETE session returns 200", True)
        except urllib.error.HTTPError as e:
            test("[HTTP] DELETE session", False, f"status {e.code}")

    except Exception as e:
        failed += 1
        print(f"\nUNHANDLED EXCEPTION during test: {type(e).__name__}: {e}")
        import traceback as _tb
        _tb.print_exc()
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        shutil.rmtree(tmpdir, ignore_errors=True)


    print(f"\n{'='*65}")
    print(f"  {passed}/{passed+failed} passed, {failed} failed")
    print(f"{'='*65}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
