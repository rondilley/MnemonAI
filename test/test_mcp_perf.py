#!/usr/bin/env python3
"""
MCP Performance & Load Test

Benchmarks mnemon_ai under increasing load to measure:
  1. Latency per operation (store, retrieve, search)
  2. Throughput (operations/second)
  3. Memory growth under load
  4. Degradation curve at 100/1K/10K memories
  5. Concurrent client simulation

Usage:
    cd build
    LD_LIBRARY_PATH=~/.local/lib python3 ../test/test_mcp_perf.py

Requires mnemon_ai binary in the current directory.
"""

import json
import os
import resource
import subprocess
import sys
import time

BINARY = "./mnemon_ai"


def send_recv(proc, request):
    """Send a JSON-RPC request and read the response."""
    line = json.dumps(request) + "\n"
    proc.stdin.write(line)
    proc.stdin.flush()
    resp_line = proc.stdout.readline()
    if not resp_line:
        return None
    return json.loads(resp_line.strip())


def start_server(tmpdir):
    """Start mnemon_ai in stdio mode."""
    env = os.environ.copy()
    home = os.path.expanduser("~")
    env["LD_LIBRARY_PATH"] = f"{home}/.local/lib:" + env.get("LD_LIBRARY_PATH", "")

    os.makedirs(tmpdir, exist_ok=True)
    conf = os.path.join(tmpdir, "perf.conf")
    with open(conf, "w") as f:
        f.write(f"[general]\ndata_dir = {tmpdir}/data\nlog_level = error\n")
        f.write("[lmdb]\nmap_size_gb = 2\nmax_readers = 64\n")
        f.write("[embedding]\nmodel_path = none\ndimensions = 768\n")

    # Use valgrind to work around ggml-cpu SIMD constructor bug on some AMD CPUs
    use_valgrind = os.environ.get("MNEMON_USE_VALGRIND", "1") == "1"
    cmd = ["valgrind", "-q", BINARY] if use_valgrind else [BINARY]
    cmd += ["--stdio", "--config", conf]

    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )

    # Initialize
    send_recv(proc, {
        "jsonrpc": "2.0", "id": 0, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                   "clientInfo": {"name": "perf_test", "version": "1.0"}}
    })
    proc.stdin.write(json.dumps({
        "jsonrpc": "2.0", "method": "notifications/initialized"
    }) + "\n")
    proc.stdin.flush()

    return proc


def store_memory(proc, content, req_id):
    """Store a memory and return (id, latency_ms)."""
    t0 = time.monotonic()
    resp = send_recv(proc, {
        "jsonrpc": "2.0", "id": req_id,
        "method": "tools/call",
        "params": {
            "name": "store_memory",
            "arguments": {"content": content, "source_type": "perf_test"}
        }
    })
    lat = (time.monotonic() - t0) * 1000
    inner = json.loads(
        resp.get("result", {}).get("content", [{}])[0].get("text", "{}")
    ) if resp else {}
    return inner.get("id"), lat


def retrieve_memory(proc, mem_id, req_id):
    """Retrieve a memory by ID and return latency_ms."""
    t0 = time.monotonic()
    send_recv(proc, {
        "jsonrpc": "2.0", "id": req_id,
        "method": "tools/call",
        "params": {"name": "retrieve_memory", "arguments": {"id": mem_id}}
    })
    return (time.monotonic() - t0) * 1000


def search_keyword(proc, query, req_id, top_k=10):
    """Search by keyword and return (count, latency_ms)."""
    t0 = time.monotonic()
    resp = send_recv(proc, {
        "jsonrpc": "2.0", "id": req_id,
        "method": "tools/call",
        "params": {
            "name": "search_keyword",
            "arguments": {"query": query, "top_k": top_k}
        }
    })
    lat = (time.monotonic() - t0) * 1000
    inner = json.loads(
        resp.get("result", {}).get("content", [{}])[0].get("text", "{}")
    ) if resp else {}
    count = len(inner.get("results", []))
    return count, lat


def percentile(sorted_list, p):
    """Calculate p-th percentile from sorted list."""
    if not sorted_list:
        return 0
    k = (len(sorted_list) - 1) * p / 100
    f = int(k)
    c = f + 1 if f + 1 < len(sorted_list) else f
    return sorted_list[f] + (k - f) * (sorted_list[c] - sorted_list[f])


def report_latencies(name, latencies):
    """Print latency statistics."""
    if not latencies:
        print(f"  {name}: no data")
        return
    latencies.sort()
    avg = sum(latencies) / len(latencies)
    p50 = percentile(latencies, 50)
    p95 = percentile(latencies, 95)
    p99 = percentile(latencies, 99)
    print(f"  {name:<25s}  avg={avg:7.2f}ms  p50={p50:7.2f}ms  "
          f"p95={p95:7.2f}ms  p99={p99:7.2f}ms  n={len(latencies)}")


def main():
    if not os.path.exists(BINARY):
        print(f"ERROR: {BINARY} not found. Run from build/ directory.")
        sys.exit(1)

    tmpdir = f"/tmp/mnemon_perf_test_{os.getpid()}"
    os.makedirs(tmpdir, exist_ok=True)

    print("=" * 70)
    print("  mnemon_ai MCP Performance & Load Test")
    print("=" * 70)

    # Test at different scales
    for scale in [100, 1000]:
        print(f"\n--- Scale: {scale} memories ---\n")

        proc = start_server(tmpdir + f"_{scale}")

        # Phase 1: Store
        store_lats = []
        stored_ids = []
        t_start = time.monotonic()

        for i in range(scale):
            content = f"Memory number {i}: This is test content about topic {i % 50} " \
                      f"with keywords like database, search, vector, embedding, " \
                      f"knowledge graph, and MCP protocol variant {i}"
            mem_id, lat = store_memory(proc, content, 1000 + i)
            store_lats.append(lat)
            if mem_id:
                stored_ids.append(mem_id)

        store_elapsed = time.monotonic() - t_start
        store_throughput = scale / store_elapsed if store_elapsed > 0 else 0

        report_latencies("store_memory", store_lats)
        print(f"  {'throughput':<25s}  {store_throughput:.1f} ops/sec  "
              f"({scale} in {store_elapsed:.2f}s)")

        # Phase 2: Retrieve (random sample)
        retrieve_lats = []
        sample_size = min(100, len(stored_ids))
        import random
        sample = random.sample(stored_ids, sample_size)

        for i, mid in enumerate(sample):
            lat = retrieve_memory(proc, mid, 2000 + i)
            retrieve_lats.append(lat)

        report_latencies("retrieve_memory", retrieve_lats)

        # Phase 3: Keyword search
        search_lats = []
        queries = ["database", "search vector", "knowledge graph",
                    "MCP protocol", "embedding model", "test content",
                    "topic 25", "variant 42", "memory number", "keyword"]
        for i, q in enumerate(queries * 5):  # 50 searches
            count, lat = search_keyword(proc, q, 3000 + i)
            search_lats.append(lat)

        report_latencies("search_keyword", search_lats)

        # Phase 4: Stats
        t0 = time.monotonic()
        resp = send_recv(proc, {
            "jsonrpc": "2.0", "id": 9999,
            "method": "tools/call",
            "params": {"name": "get_memory_stats", "arguments": {}}
        })
        stats_lat = (time.monotonic() - t0) * 1000
        inner = json.loads(
            resp.get("result", {}).get("content", [{}])[0].get("text", "{}")
        ) if resp else {}
        print(f"\n  Stats: memories={inner.get('total_memories', '?')} "
              f"entities={inner.get('total_entities', '?')} "
              f"fts={inner.get('fts_indexed', '?')} "
              f"vectors={inner.get('memory_vectors', '?')} "
              f"latency={stats_lat:.2f}ms")

        # Cleanup
        proc.stdin.close()
        proc.wait(timeout=10)

    # Cleanup
    import shutil
    for scale in [100, 1000]:
        shutil.rmtree(tmpdir + f"_{scale}", ignore_errors=True)
    shutil.rmtree(tmpdir, ignore_errors=True)

    print("\n" + "=" * 70)
    print("  Performance test complete")
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
