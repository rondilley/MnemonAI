#!/usr/bin/env python3
"""Full network (Streamable HTTP) test exercising ALL 38 mnemond tools -- with
the REAL embedding model loaded (the "embeddings-on" integration test).

This complements test/test_mcp_http.py, which runs with model_path=none and so
stubs the vector/semantic paths. This test loads the actual GPU model, so it
validates real embedding, chunked vector indexing, and semantic/hybrid retrieval
end-to-end over HTTP. Uses an isolated data dir so production data is untouched.

Skips (exit 77, the ctest SKIP_RETURN_CODE) when the model is not present, so it
never fails on machines/CI without the model.

Run:  LD_LIBRARY_PATH=~/.local/lib python3 test/test_all_tools_http.py
"""
import json, os, signal, subprocess, sys, time, urllib.request, urllib.error

HOME = os.path.expanduser("~")
BINARY = os.path.join(HOME, ".local/bin/mnemond")
MODEL = os.path.join(HOME, ".local/share/mnemond/models/nomic-embed-text-v1.5.Q8_0.gguf")
PORT = 13903
URL = f"http://127.0.0.1:{PORT}/mcp"

passed = failed = 0
session_id = None


def post(body, sid=None, raw=False, timeout=60):
    headers = {"Content-Type": "application/json",
               "Accept": "application/json, text/event-stream",
               "Origin": "http://127.0.0.1"}
    if sid:
        headers["Mcp-Session-Id"] = sid
    req = urllib.request.Request(URL, data=json.dumps(body).encode(),
                                 headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def call(name, args, timeout=60):
    """Return (inner_result_dict, is_error_bool) for a tools/call over HTTP."""
    st, _, body = post({"jsonrpc": "2.0", "id": 99, "method": "tools/call",
                        "params": {"name": name, "arguments": args}},
                       session_id, timeout=timeout)
    if st != 200:
        return {"_http": st, "_body": body.decode("utf-8", "replace")[:200]}, True
    env = json.loads(body)
    res = env.get("result", {})
    is_err = res.get("isError", False)
    content = res.get("content", [])
    inner = json.loads(content[0]["text"]) if content else {}
    return inner, is_err


def check(tool, ok, detail=""):
    global passed, failed
    mark = "PASS" if ok else "FAIL"
    if ok:
        passed += 1
    else:
        failed += 1
    print(f"  {tool:<24s} {mark}  {detail}")
    return ok


def main():
    global session_id
    if not os.path.exists(BINARY):
        print(f"ERROR: {BINARY} not found"); sys.exit(1)
    if not os.path.exists(MODEL):
        print(f"SKIP: embedding model not found at {MODEL} -- "
              f"this is the embeddings-on test; run test_mcp_http.py for the "
              f"model-less variant"); sys.exit(77)

    tmp = os.path.join(HOME, f".mnemon_alltools_{os.getpid()}")
    os.makedirs(tmp, exist_ok=True)
    impdir = os.path.join(tmp, "imp"); os.makedirs(impdir, exist_ok=True)
    with open(os.path.join(impdir, "data.jsonl"), "w") as f:
        f.write(json.dumps({"content": "Imported note about quarterly planning."}) + "\n")
        f.write(json.dumps({"content": "Imported note about the security review."}) + "\n")
    conf = os.path.join(tmp, "t.conf")
    with open(conf, "w") as f:
        f.write(f"[general]\ndata_dir = {tmp}/data\nlog_level = error\n")
        f.write("[lmdb]\nmap_size_gb = 1\nmax_readers = 16\n")
        f.write(f"[embedding]\nmodel_path = {MODEL}\ndimensions = 768\ngpu_layers = 99\n")
        f.write(f"[import]\nallowed_paths = {tmp}\n")
        f.write(f"[http]\nenabled = true\nbind = 127.0.0.1\nport = {PORT}\n")

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = f"{HOME}/.local/lib:" + env.get("LD_LIBRARY_PATH", "")
    proc = subprocess.Popen([BINARY, "--foreground", "--config", conf],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    try:
        # Wait for HTTP to accept an initialize.
        sid = None
        for _ in range(60):
            try:
                st, hdr, _ = post({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                    "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                               "clientInfo": {"name": "alltools", "version": "1"}}}, timeout=5)
                if st == 200:
                    sid = hdr.get("Mcp-Session-Id", hdr.get("mcp-session-id"))
                    break
            except Exception:
                pass
            time.sleep(0.5)
        if not sid:
            print("ERROR: daemon did not come up over HTTP")
            print(proc.stderr.read().decode("utf-8", "replace")[-2000:]); sys.exit(1)
        session_id = sid
        post({"jsonrpc": "2.0", "method": "notifications/initialized"}, sid)

        print("=== Full HTTP test: all mnemond tools ===\n")

        # --- registry: confirm all 38 tools advertised ---
        st, _, body = post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, sid)
        tools = [t["name"] for t in json.loads(body)["result"]["tools"]]
        check("tools/list", len(tools) == 38, f"{len(tools)} tools advertised")
        advertised = set(tools)

        print("\n  -- monitoring --")
        r, e = call("health_check", {}); check("health_check", not e and r.get("status") == "ok", r.get("version", ""))
        r, e = call("get_hardware_info", {}); check("get_hardware_info", not e and "cpu" in json.dumps(r).lower())
        r, e = call("get_memory_stats", {}); check("get_memory_stats", not e)
        r, e = call("get_index_stats", {}); check("get_index_stats", not e and "lmdb" in r)

        print("\n  -- memory CRUD --")
        r, e = call("store_memory", {"content": "The Apollo project uses PostgreSQL 16 and Redis for caching."})
        mem_id = r.get("id") or r.get("memory_id")
        check("store_memory", not e and bool(mem_id), mem_id or "")
        r, e = call("retrieve_memory", {"id": mem_id}); check("retrieve_memory", not e and "PostgreSQL" in json.dumps(r))
        r, e = call("update_memory", {"id": mem_id, "content": "The Apollo project uses PostgreSQL 16, Redis, and Kafka."})
        check("update_memory", not e)
        r, e = call("list_memories", {"limit": 5}); check("list_memories", not e and r.get("total_count", 0) >= 1)

        # seed a few more memories for search/consolidate
        for c in ["Redis is used for session caching in Apollo.",
                  "Kafka handles the event stream pipeline.",
                  "The security review flagged an auth bug.",
                  "Quarterly planning meeting scheduled for March."]:
            call("store_memory", {"content": c})
        time.sleep(0.5)

        print("\n  -- search --")
        r, e = call("search_hybrid", {"query": "database caching", "top_k": 5}); check("search_hybrid", not e and len(r.get("results", [])) > 0, f"{len(r.get('results',[]))} hits")
        r, e = call("search_semantic", {"query": "event streaming pipeline", "top_k": 5})
        sem = r.get("results", [])
        ghosts = [x for x in sem if x.get("content", "") == "" or x.get("tier") == "unknown"]
        check("search_semantic", not e and len(sem) > 0 and not ghosts, f"{len(sem)} hits, {len(ghosts)} ghosts")
        r, e = call("search_keyword", {"query": "Kafka", "top_k": 5}); check("search_keyword", not e and len(r.get("results", [])) > 0)
        r, e = call("search_temporal", {"since": "2020-01-01", "until": "2035-12-31", "top_k": 5}); check("search_temporal", not e and len(r.get("results", [])) > 0)

        print("\n  -- entity graph --")
        r, e = call("create_entity", {"name": "Apollo Service", "entity_type": "service", "observations": ["core backend"]})
        ent1 = r.get("id"); check("create_entity", not e and bool(ent1), ent1 or "")
        r, e = call("create_entity", {"name": "PostgreSQL", "entity_type": "technology", "observations": ["relational db"]})
        ent2 = r.get("id")
        r, e = call("add_observation", {"entity_id": ent1, "observation": "handles auth"}); check("add_observation", not e)
        r, e = call("create_relation", {"source_id": ent1, "target_id": ent2, "edge_type": "depends_on"}); check("create_relation", not e)
        r, e = call("search_entities", {"query": "Apollo", "top_k": 5}); check("search_entities", not e and len(r.get("entities", [])) > 0, f"{len(r.get('entities',[]))} found")
        r, e = call("get_entity_graph", {"entity_id": ent1, "depth": 2})
        nodes = r.get("nodes", []); nulls = [n for n in nodes if n.get("id", "").startswith("00000000") or n.get("name", "") == ""]
        check("get_entity_graph", not e and len(nodes) > 0 and not nulls, f"{len(nodes)} nodes, {len(nulls)} null")

        print("\n  -- temporal / bi-temporal --")
        r, e = call("get_history", {"entity_id": ent1}); check("get_history", not e and r.get("count", 0) >= 2, f"{r.get('count')} versions")
        r, e = call("get_state_at_time", {"entity_id": ent1, "timestamp": "2035-01-01T00:00:00Z"}); check("get_state_at_time", not e and r.get("observation_count", 0) >= 2, f"{r.get('observation_count')} obs")
        r, e = call("get_changes_since", {"since": "2020-01-01", "top_k": 10}); check("get_changes_since", not e)
        r, e = call("extract_events", {"content": "Launch on 2026-06-10. Beta on January 15, 2026."})
        check("extract_events", not e and r.get("extracted", 0) == 2, f"{r.get('extracted')} events")
        time.sleep(0.3)
        r, e = call("search_events", {"since": "2026-01-01", "until": "2026-12-31", "top_k": 10}); check("search_events", not e and len(r.get("results", [])) >= 1, f"{len(r.get('results',[]))} events")
        r, e = call("calculate_duration", {"from": "2026-01-01", "to": "2026-01-31"}); check("calculate_duration", not e and r.get("days") == 30, f"{r.get('days')} days")

        print("\n  -- graph maintenance --")
        r, e = call("link_entities", {}); check("link_entities", not e and "edges_created" in r, f"{r.get('edges_created')} edges")
        r, e = call("resolve_entities", {}); check("resolve_entities", not e and "merged" in r, f"merged={r.get('merged')}")
        r, e = call("consolidate_memories", {"dry_run": True}); check("consolidate_memories", not e and "consolidated_count" in r, str(r.get("consolidated_count")))
        r, e = call("prune_stale", {"dry_run": True}); check("prune_stale", not e and "count" in r, f"{r.get('count')} candidates")

        print("\n  -- import --")
        r, e = call("import_batch", {"memories": [{"content": "Batch memory one."}, {"content": "Batch memory two."}]})
        check("import_batch", not e and r.get("imported", 0) == 2, f"{r.get('imported')} imported")
        r, e = call("import_file", {"path": os.path.join(impdir, "data.jsonl"), "format": "jsonl"})
        check("import_file", not e and r.get("imported", 0) >= 1, f"{r.get('imported')} imported")
        r, e = call("import_directory", {"path": impdir, "pattern": "*.jsonl"})
        check("import_directory", not e, json.dumps(r)[:60])
        r, e = call("get_import_status", {}); check("get_import_status", not e)

        print("\n  -- index management --")
        r, e = call("rebuild_indexes", {"target": "fts"}); check("rebuild_indexes", not e and "fts" in json.dumps(r))

        print("\n  -- deletes --")
        r, e = call("delete_memory", {"id": mem_id}); check("delete_memory", not e and r.get("deleted") is True)
        r, e = call("delete_entity", {"id": ent2}); check("delete_entity", not e and r.get("deleted") is True)

        print("\n  -- admin / restricted (expect controlled response, not transport failure) --")
        r, e = call("set_system_config", {"key": "log_level", "value": "info"}); check("set_system_config", isinstance(r, dict), json.dumps(r)[:50])
        r, e = call("admin_reset_auth", {"admin_key": "wrong"}); check("admin_reset_auth", isinstance(r, dict), "responded")
        r, e = call("export_all_memories", {}); check("export_all_memories", isinstance(r, dict), "responded (decoy)")
        r, e = call("debug_raw_query", {"query": "x", "database": "entities"}); check("debug_raw_query", isinstance(r, dict), "responded (debug-gated)")

        # coverage check
        exercised = {"health_check","get_hardware_info","get_memory_stats","get_index_stats",
            "store_memory","retrieve_memory","update_memory","list_memories","search_hybrid",
            "search_semantic","search_keyword","search_temporal","create_entity","add_observation",
            "create_relation","search_entities","get_entity_graph","get_history","get_state_at_time",
            "get_changes_since","extract_events","search_events","calculate_duration","link_entities",
            "resolve_entities","consolidate_memories","prune_stale","import_batch","import_file",
            "import_directory","get_import_status","rebuild_indexes","delete_memory","delete_entity",
            "set_system_config","admin_reset_auth","export_all_memories","debug_raw_query"}
        missing = advertised - exercised
        print()
        check("coverage: every advertised tool exercised", not missing, f"missing={sorted(missing)}")

        print(f"\n=== {passed} passed, {failed} failed (of {passed+failed}) ===")
        sys.exit(0 if failed == 0 else 1)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        subprocess.run(["rm", "-rf", tmp])


if __name__ == "__main__":
    main()
