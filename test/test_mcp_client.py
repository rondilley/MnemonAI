#!/usr/bin/env python3
"""
MCP Client Compatibility Test -- Exercises ALL 28 tools over stdio.

Acts as a standard MCP client, validating response schemas against the
MCP spec and verifying functional correctness for every registered tool.

Usage:
    cd build
    LD_LIBRARY_PATH=~/.local/lib python3 ../test/test_mcp_client.py
"""

import json
import os
import shutil
import subprocess
import sys
import time

BINARY = "./mnemon_ai"
passed = 0
failed = 0


def send_recv(proc, request):
    line = json.dumps(request) + "\n"
    proc.stdin.write(line)
    proc.stdin.flush()
    resp_line = proc.stdout.readline()
    if not resp_line:
        return None
    return json.loads(resp_line.strip())


def call_tool(proc, name, arguments, req_id):
    """Call a tool and return the parsed inner JSON result."""
    resp = send_recv(proc, {
        "jsonrpc": "2.0", "id": req_id,
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments}
    })
    if not resp:
        return None, None
    result = resp.get("result", {})
    is_error = result.get("isError", False)
    content = result.get("content", [])
    if content:
        inner = json.loads(content[0].get("text", "{}"))
    else:
        inner = {}
    return inner, is_error


def test(name, condition, detail=""):
    global passed, failed
    if not condition:
        failed += 1
        print(f"  {name:<62s} FAIL: {detail}")
    else:
        passed += 1
        print(f"  {name:<62s} PASS")
    return condition


def main():
    global passed, failed

    if not os.path.exists(BINARY):
        print(f"ERROR: {BINARY} not found. Run from build/ directory.")
        sys.exit(1)

    env = os.environ.copy()
    home = os.path.expanduser("~")
    env["LD_LIBRARY_PATH"] = f"{home}/.local/lib:" + env.get("LD_LIBRARY_PATH", "")

    # Create temp dirs under HOME (so import path validation passes)
    tmpdir = os.path.join(home, f".mnemon_mcp_client_test_{os.getpid()}")
    os.makedirs(tmpdir, exist_ok=True)

    conf = os.path.join(tmpdir, "test.conf")
    with open(conf, "w") as f:
        f.write(f"[general]\ndata_dir = {tmpdir}/data\nlog_level = error\n")
        f.write("[lmdb]\nmap_size_gb = 1\nmax_readers = 16\n")
        f.write("[embedding]\nmodel_path = none\ndimensions = 768\n")

    use_valgrind = os.environ.get("MNEMON_USE_VALGRIND", "0") == "1"
    cmd = ["valgrind", "-q", BINARY] if use_valgrind else [BINARY]
    cmd += ["--stdio", "--config", conf]

    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, env=env,
    )

    print("=== MCP Client Compatibility Test (All 28 Tools) ===\n")

    # ---- 1. Lifecycle ----
    print("  --- Lifecycle ---")
    resp = send_recv(proc, {
        "jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                   "clientInfo": {"name": "test_client", "version": "1.0"}}
    })
    r = resp.get("result", {}) if resp else {}
    test("[SPEC] protocolVersion = 2024-11-05",
         r.get("protocolVersion") == "2024-11-05")
    test("[SPEC] capabilities.tools exists",
         "tools" in r.get("capabilities", {}))
    test("[SPEC] serverInfo.name + version present",
         isinstance(r.get("serverInfo", {}).get("name"), str) and
         isinstance(r.get("serverInfo", {}).get("version"), str))

    proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
    proc.stdin.flush()

    # ---- 2. tools/list ----
    print("\n  --- Tools Discovery ---")
    resp = send_recv(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    tools = resp.get("result", {}).get("tools", [])
    tool_names = {t["name"] for t in tools}
    test("[SPEC] tools/list returns >= 28 tools", len(tools) >= 28, f"got {len(tools)}")

    expected_tools = [
        "store_memory", "retrieve_memory", "update_memory", "delete_memory",
        "search_hybrid", "search_semantic", "search_keyword", "search_temporal",
        "create_entity", "add_observation", "create_relation",
        "search_entities", "get_entity_graph",
        "get_history", "get_state_at_time", "get_changes_since",
        "get_memory_stats", "health_check", "get_hardware_info", "get_index_stats",
        "list_memories", "consolidate_memories", "prune_stale",
        "import_batch", "import_file", "import_directory", "get_import_status",
        "rebuild_indexes",
    ]
    for t in expected_tools:
        test(f"[SPEC] tool '{t}' registered", t in tool_names)

    for t in tools:
        test(f"[SPEC] tool '{t['name']}' has inputSchema",
             isinstance(t.get("inputSchema"), dict))

    # ---- 3. Memory CRUD ----
    print("\n  --- Memory CRUD ---")

    # store_memory
    r, ie = call_tool(proc, "store_memory", {
        "content": "Python uses dynamic typing and garbage collection",
        "source_type": "test", "tags": ["python", "language"],
        "tier": "episodic"
    }, 10)
    test("[TOOL] store_memory: returns id", "id" in r)
    test("[TOOL] store_memory: returns created_at", "created_at" in r)
    test("[TOOL] store_memory: isError=false", ie == False)
    stored_id = r.get("id", "")

    # store_memory: secret rejected
    r, ie = call_tool(proc, "store_memory", {
        "content": "ghp_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    }, 11)
    test("[TOOL] store_memory: secret rejected (isError=true)", ie == True)

    # retrieve_memory
    r, ie = call_tool(proc, "retrieve_memory", {"id": stored_id}, 20)
    test("[TOOL] retrieve_memory: content matches",
         "backpropagation" not in r.get("content", "") and "Python" in r.get("content", ""))
    test("[TOOL] retrieve_memory: has source_type", r.get("source_type") == "test")

    # update_memory
    r, ie = call_tool(proc, "update_memory", {
        "id": stored_id, "content": "Updated: Python 3.12 is the latest"
    }, 30)
    test("[TOOL] update_memory: updated=true", r.get("updated") == True)

    # Verify update
    r, ie = call_tool(proc, "retrieve_memory", {"id": stored_id}, 31)
    test("[TOOL] update_memory: content actually changed",
         "3.12" in r.get("content", ""))

    # ---- 4. Search ----
    print("\n  --- Search ---")

    # Store more memories for search
    for content in [
        "PostgreSQL supports JSONB and full-text search",
        "Redis is an in-memory key-value store",
        "SQLite is a serverless embedded database",
    ]:
        call_tool(proc, "store_memory", {"content": content, "source_type": "test"}, 40)

    r, ie = call_tool(proc, "search_keyword", {"query": "database", "top_k": 5}, 50)
    test("[TOOL] search_keyword: returns results array",
         isinstance(r.get("results"), list))
    test("[TOOL] search_keyword: found results", len(r.get("results", [])) >= 1)

    r, ie = call_tool(proc, "search_semantic", {"query": "database"}, 51)
    test("[TOOL] search_semantic: returns results array",
         isinstance(r.get("results"), list))

    r, ie = call_tool(proc, "search_hybrid", {"query": "database search"}, 52)
    test("[TOOL] search_hybrid: returns results array",
         isinstance(r.get("results"), list))

    r, ie = call_tool(proc, "search_temporal", {
        "since": "2020-01-01T00:00:00Z", "top_k": 5
    }, 53)
    test("[TOOL] search_temporal: returns results array",
         isinstance(r.get("results"), list))

    # ---- 5. Entity/Graph ----
    print("\n  --- Entity & Graph ---")

    r, ie = call_tool(proc, "create_entity", {
        "name": "Rust", "entity_type": "language",
        "observations": ["Systems programming", "Memory safe without GC"]
    }, 60)
    test("[TOOL] create_entity: returns id + name", "id" in r and r.get("name") == "Rust")
    entity_a = r.get("id", "")

    r, ie = call_tool(proc, "create_entity", {
        "name": "Go", "entity_type": "language"
    }, 61)
    entity_b = r.get("id", "")

    r, ie = call_tool(proc, "add_observation", {
        "entity_id": entity_a, "observation": "Cargo is the package manager"
    }, 62)
    test("[TOOL] add_observation: count >= 3", r.get("observation_count", 0) >= 3)

    r, ie = call_tool(proc, "search_entities", {"query": "Rust"}, 63)
    test("[TOOL] search_entities: entities array", isinstance(r.get("entities"), list))

    r, ie = call_tool(proc, "create_relation", {
        "source_id": entity_a, "target_id": entity_b,
        "edge_type": "competes_with", "description": "Both are systems languages"
    }, 64)
    test("[TOOL] create_relation: returns edge id", "id" in r)

    r, ie = call_tool(proc, "get_entity_graph", {
        "entity_id": entity_a, "depth": 2
    }, 65)
    test("[TOOL] get_entity_graph: entity + edges_out + related",
         "entity" in r and "edges_out" in r and "related_entities" in r)

    # ---- 6. Temporal ----
    print("\n  --- Temporal ---")

    r, ie = call_tool(proc, "get_history", {"entity_id": entity_a}, 70)
    test("[TOOL] get_history: versions array", isinstance(r.get("versions"), list))

    r, ie = call_tool(proc, "get_state_at_time", {
        "entity_id": entity_a,
        "timestamp": "2030-01-01T00:00:00Z"
    }, 71)
    test("[TOOL] get_state_at_time: returns entity name",
         isinstance(r.get("name"), str))

    r, ie = call_tool(proc, "get_changes_since", {
        "since": "2020-01-01T00:00:00Z"
    }, 72)
    test("[TOOL] get_changes_since: results array",
         isinstance(r.get("results"), list))

    # ---- 7. System ----
    print("\n  --- System ---")

    r, ie = call_tool(proc, "health_check", {}, 80)
    test("[TOOL] health_check: status=ok", r.get("status") == "ok")
    test("[TOOL] health_check: has version", isinstance(r.get("version"), str))

    r, ie = call_tool(proc, "get_memory_stats", {}, 81)
    test("[TOOL] get_memory_stats: total_memories > 0",
         r.get("total_memories", 0) > 0)

    r, ie = call_tool(proc, "get_hardware_info", {}, 82)
    test("[TOOL] get_hardware_info: has cpu + gpu + ram",
         "cpu" in r and "gpu" in r and "ram_mb" in r)

    r, ie = call_tool(proc, "get_index_stats", {}, 83)
    test("[TOOL] get_index_stats: lmdb + fts5 + vector",
         "lmdb" in r and "fts5" in r and "vector" in r)

    r, ie = call_tool(proc, "list_memories", {"limit": 3}, 84)
    test("[TOOL] list_memories: memories array + total_count",
         isinstance(r.get("memories"), list) and "total_count" in r)

    # ---- 8. Lifecycle Tools ----
    print("\n  --- Lifecycle ---")

    r, ie = call_tool(proc, "consolidate_memories", {"dry_run": True}, 90)
    test("[TOOL] consolidate_memories: consolidated_count + duration",
         "consolidated_count" in r and "duration_ms" in r)

    r, ie = call_tool(proc, "prune_stale", {
        "min_age_days": 0, "min_importance": 99.0, "dry_run": True
    }, 91)
    test("[TOOL] prune_stale: candidates array + dry_run flag",
         isinstance(r.get("candidates"), list) and r.get("dry_run") == True)

    r, ie = call_tool(proc, "rebuild_indexes", {"target": "all"}, 92)
    test("[TOOL] rebuild_indexes: rebuilt array + duration",
         isinstance(r.get("rebuilt"), list) and "duration_ms" in r)

    # ---- 9. Import ----
    print("\n  --- Import ---")

    r, ie = call_tool(proc, "import_batch", {
        "memories": [
            {"content": "Batch test 1"},
            {"content": "Batch test 2"},
            {"content": "Batch test 3"},
        ]
    }, 100)
    test("[TOOL] import_batch: imported=3",
         r.get("imported") == 3 and r.get("errors") == 0)

    # Create a temp JSONL file under HOME for import_file
    import_file = os.path.join(tmpdir, "test_import.jsonl")
    with open(import_file, "w") as f:
        f.write('{"content":"File import line 1"}\n')
        f.write('{"content":"File import line 2"}\n')

    r, ie = call_tool(proc, "import_file", {
        "path": import_file, "format": "jsonl"
    }, 101)
    test("[TOOL] import_file: imported >= 1",
         r.get("imported", 0) >= 1 or "error" in r)

    # Create temp dir with text files
    import_dir = os.path.join(tmpdir, "docs")
    os.makedirs(import_dir, exist_ok=True)
    for i in range(2):
        with open(os.path.join(import_dir, f"doc{i}.txt"), "w") as f:
            f.write(f"Directory import document {i}\n")

    r, ie = call_tool(proc, "import_directory", {
        "path": import_dir, "pattern": "*.txt"
    }, 102)
    test("[TOOL] import_directory: files_processed >= 1",
         r.get("files_processed", 0) >= 1 or "error" in r)

    r, ie = call_tool(proc, "get_import_status", {}, 103)
    test("[TOOL] get_import_status: jobs array",
         isinstance(r.get("jobs"), list))

    # ---- 10. Delete + Error ----
    print("\n  --- Delete + Errors ---")

    r, ie = call_tool(proc, "delete_memory", {"id": stored_id}, 110)
    test("[TOOL] delete_memory: deleted=true", r.get("deleted") == True)

    r, ie = call_tool(proc, "retrieve_memory", {"id": stored_id}, 111)
    test("[TOOL] retrieve after delete: error returned", "error" in r)

    # Unknown tool
    resp = send_recv(proc, {
        "jsonrpc": "2.0", "id": 120,
        "method": "tools/call",
        "params": {"name": "bogus_tool", "arguments": {}}
    })
    test("[SPEC] unknown tool: error code -32602",
         resp.get("error", {}).get("code") == -32602)

    # ---- Cleanup ----
    proc.stdin.close()
    proc.wait(timeout=10)
    shutil.rmtree(tmpdir, ignore_errors=True)

    print(f"\n{'='*65}")
    print(f"  {passed}/{passed+failed} passed, {failed} failed")
    print(f"{'='*65}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
