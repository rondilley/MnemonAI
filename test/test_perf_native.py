#!/usr/bin/env python3
"""Native mnemond realistic-workload perf harness: scale-sweep + client-sweep at human cadence."""

import argparse
import json
import os
import random
import subprocess
import sys
import threading
import time
import urllib.request
from collections import defaultdict

BINARY = "./mnemond"
DEFAULT_MODEL = os.path.expanduser(
    "~/.local/share/mnemond/models/nomic-embed-text-v1.5.Q8_0.gguf")
DEFAULT_PORT = 13911

CONTENT_TEMPLATES = [
    "The user mentioned their favorite {topic} is {item}, discovered during {event}.",
    "In a meeting with {person}, we discussed {system} and picked {technology} for {reason}.",
    "Debug session: {component} failed because {cause}. Fix: {fix}. Lesson: {lesson}.",
    "Project note: {project} deadline is {date}. Blockers: {blocker1}, {blocker2}.",
    "Learned: {concept} works by {mechanism}. Similar to {analogy} but differs in {difference}.",
    "Config change for {service}: {setting}={value}. Reason: {reason}. Rollback: {procedure}.",
]
TOPIC_WORDS = [
    "database", "vector search", "embedding", "knowledge graph", "LMDB",
    "HNSW", "BM25", "RRF fusion", "temporal reasoning", "FTS5",
    "MessagePack", "UUIDv7", "MCP protocol", "JSON-RPC", "SSE streaming",
    "reader pool", "writer queue", "GPU embedding", "ROCm HIP", "AVX-512",
    "prompt injection", "secret detection", "consolidation", "entity merging",
]
FILL_WORDS = [
    "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta",
    "iota", "kappa", "lambda", "mu", "nu", "xi", "pi", "rho",
    "sigma", "tau", "phi", "chi", "psi", "omega",
]


def gen_content(seed):
    r = random.Random(seed)
    tpl = r.choice(CONTENT_TEMPLATES)
    topic = r.choice(TOPIC_WORDS)
    w = r.sample(FILL_WORDS, k=8)
    fields = {
        "topic": topic, "item": w[0], "event": w[1], "person": w[2],
        "system": topic, "technology": w[3], "reason": f"better {w[4]}",
        "component": topic, "cause": f"{w[5]} misconfig",
        "fix": f"set {w[6]} correctly", "lesson": f"check {w[7]}",
        "project": f"project-{seed % 50}",
        "date": f"2026-0{(seed % 9) + 1}-15",
        "blocker1": w[4], "blocker2": w[5], "concept": topic,
        "mechanism": f"{w[0]}-{w[1]} dispatch", "analogy": w[2],
        "difference": w[3], "service": topic, "setting": w[0],
        "value": str(r.randint(1, 10000)), "procedure": f"revert {w[1]}",
    }
    return tpl.format(**fields)


def gen_query(seed):
    r = random.Random(seed)
    return f"{r.choice(TOPIC_WORDS)} {r.choice(FILL_WORDS)}"


class RSSampler:
    def __init__(self, pid, interval_s=1.0):
        self.pid = pid
        self.interval = interval_s
        self.samples = []
        self.labels = []
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t0 = None

    def start(self):
        self._t0 = time.monotonic()
        self._t.start()

    def stop(self):
        self._stop.set()
        self._t.join(timeout=2)

    def mark(self, label):
        if self._t0 is not None:
            self.labels.append((time.monotonic() - self._t0, label))

    def _read_rss_kb(self):
        try:
            with open(f"/proc/{self.pid}/status") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        return int(line.split()[1])
        except FileNotFoundError:
            return None
        return None

    def _run(self):
        while not self._stop.wait(self.interval):
            rss = self._read_rss_kb()
            if rss is not None:
                self.samples.append((time.monotonic() - self._t0, rss))

    def summary(self):
        if not self.samples:
            return {}
        kbs = [s[1] for s in self.samples]
        return {
            "rss_kb_start": kbs[0],
            "rss_kb_end": kbs[-1],
            "rss_kb_peak": max(kbs),
            "rss_kb_growth": kbs[-1] - kbs[0],
            "samples": len(kbs),
            "labels": list(self.labels),
        }


def pct(sorted_lats, p):
    if not sorted_lats:
        return 0.0
    k = (len(sorted_lats) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(sorted_lats) - 1)
    return sorted_lats[f] + (k - f) * (sorted_lats[c] - sorted_lats[f])


def histogram(latencies):
    if not latencies:
        return {"n": 0}
    s = sorted(latencies)
    return {
        "n": len(s),
        "avg_ms": sum(s) / len(s),
        "p50_ms": pct(s, 50),
        "p95_ms": pct(s, 95),
        "p99_ms": pct(s, 99),
        "max_ms": s[-1],
    }


def fmt_row(label, h, extras=""):
    if not h or h.get("n", 0) == 0:
        return f"  {label:<20s} (no data) {extras}"
    return (f"  {label:<20s} n={h['n']:<6d}  "
            f"avg={h['avg_ms']:7.2f}  p50={h['p50_ms']:7.2f}  "
            f"p95={h['p95_ms']:7.2f}  p99={h['p99_ms']:7.2f}  "
            f"max={h['max_ms']:7.2f}  ms"
            + (f"  {extras}" if extras else ""))


class Client:
    def __init__(self, port):
        self.port = port
        self.session_id = None
        self._rid = 0
        self._init()

    def _next_id(self):
        self._rid += 1
        return self._rid

    def _post(self, body, timeout=60):
        url = f"http://127.0.0.1:{self.port}/mcp"
        data = json.dumps(body).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        }
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        req = urllib.request.Request(url, data=data, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                body_bytes = resp.read()
                parsed = json.loads(body_bytes.decode("utf-8")) if body_bytes else {}
                return parsed, dict(resp.headers)
        except Exception:
            raise

    def _init(self):
        resp, headers = self._post({
            "jsonrpc": "2.0", "id": self._next_id(), "method": "initialize",
            "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                       "clientInfo": {"name": "perf_native", "version": "1.0"}},
        })
        self.session_id = headers.get("Mcp-Session-Id") or headers.get("mcp-session-id")
        try:
            self._post({"jsonrpc": "2.0", "method": "notifications/initialized"})
        except Exception:
            pass

    def call(self, tool, args, timeout=60):
        t0 = time.monotonic()
        resp, _ = self._post({
            "jsonrpc": "2.0", "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": tool, "arguments": args},
        }, timeout=timeout)
        lat_ms = (time.monotonic() - t0) * 1000.0
        result = resp.get("result", {}) if resp else {}
        is_err = result.get("isError", False)
        content = result.get("content", [])
        inner = {}
        if content:
            try:
                inner = json.loads(content[0]["text"])
            except (json.JSONDecodeError, KeyError, TypeError):
                inner = {}
        return inner, is_err, lat_ms


def _ready_check(port):
    try:
        req = urllib.request.Request(
            f"http://127.0.0.1:{port}/mcp",
            data=json.dumps({
                "jsonrpc": "2.0", "id": 0, "method": "initialize",
                "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                           "clientInfo": {"name": "ready", "version": "1"}},
            }).encode("utf-8"),
            headers={"Content-Type": "application/json",
                     "Accept": "application/json, text/event-stream"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=2) as resp:
            return resp.status == 200
    except Exception:
        return False


def start_daemon(tmpdir, port, model_path, use_embedding, reader_pool=4):
    os.makedirs(tmpdir, exist_ok=True)
    conf = os.path.join(tmpdir, "perf.conf")
    with open(conf, "w") as f:
        f.write(f"[general]\ndata_dir = {tmpdir}/data\nlog_level = error\n")
        f.write("[lmdb]\nmap_size_gb = 32\nmax_readers = 256\n")
        if use_embedding and model_path:
            f.write(f"[embedding]\nmodel_path = {model_path}\ndimensions = 768\n")
        else:
            f.write("[embedding]\nmodel_path = none\ndimensions = 768\n")
        f.write(f"[http]\nenabled = true\nbind = 127.0.0.1\nport = {port}\n")
        f.write(f"[threads]\nreader_pool_size = {reader_pool}\n")

    env = os.environ.copy()
    home = os.path.expanduser("~")
    env["LD_LIBRARY_PATH"] = f"{home}/.local/lib:" + env.get("LD_LIBRARY_PATH", "")
    proc = subprocess.Popen(
        [BINARY, "--foreground", "--config", conf],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env,
    )
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        if _ready_check(port):
            return proc
        if proc.poll() is not None:
            err = proc.stderr.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"mnemond exited during startup:\n{err[:2000]}")
        time.sleep(0.3)
    proc.kill()
    raise RuntimeError(
        f"mnemond HTTP :{port} did not come up in 90s "
        "(cold GPU JIT? run `mnemond --warmup` once)")


def ingest_block(client, seed_start, n, source_type="perf"):
    lats = []
    errs = 0
    t0 = time.monotonic()
    for i in range(n):
        try:
            _, err, lat = client.call(
                "store_memory",
                {"content": gen_content(seed_start + i),
                 "source_type": source_type})
            if err:
                errs += 1
            else:
                lats.append(lat)
        except Exception:
            errs += 1
    return lats, errs, time.monotonic() - t0


def rate_limited_query_run(client, n_queries, rate_per_s, seed_offset, use_embedding):
    tool = "search_hybrid" if use_embedding else "search_keyword"
    interval = 1.0 / rate_per_s if rate_per_s > 0 else 0.0
    lats = []
    errs = 0
    next_tick = time.monotonic()
    for i in range(n_queries):
        if interval > 0:
            now = time.monotonic()
            if now < next_tick:
                time.sleep(next_tick - now)
            next_tick += interval
        try:
            _, err, lat = client.call(
                tool, {"query": gen_query(seed_offset + i), "top_k": 10})
            if err:
                errs += 1
            else:
                lats.append(lat)
        except Exception:
            errs += 1
    return tool, lats, errs


def phase_scale_sweep(port, scale_steps, queries_per_step, query_rate,
                      use_embedding, sampler):
    print(f"\n=== scale-sweep: corpus {scale_steps}, {queries_per_step} queries/step "
          f"at {query_rate:.1f} req/s ===")
    ingest_c = Client(port)
    query_c = Client(port)

    results = []
    current = 0
    all_ingest_lats = []
    all_ingest_errs = 0
    all_ingest_t = 0.0

    for target in scale_steps:
        if target > current:
            delta = target - current
            print(f"\n  [ingest] growing {current} -> {target} ({delta} new)...")
            if sampler:
                sampler.mark(f"ingest_to_{target}")
            lats, errs, dt = ingest_block(ingest_c, current, delta)
            current = target
            all_ingest_lats.extend(lats)
            all_ingest_errs += errs
            all_ingest_t += dt
            rate = delta / dt if dt > 0 else 0.0
            print(fmt_row("store_memory", histogram(lats),
                          f"{rate:7.1f} ops/s  errs={errs}"))

        if sampler:
            sampler.mark(f"search_at_{target}")
        print(f"\n  [search @ corpus={target}] {queries_per_step} queries...")
        tool, qlats, qerrs = rate_limited_query_run(
            query_c, queries_per_step, query_rate,
            seed_offset=target * 100, use_embedding=use_embedding)
        h = histogram(qlats)
        print(fmt_row(tool, h, f"errs={qerrs}"))
        results.append({
            "corpus_size": target,
            "search_tool": tool,
            "search_histogram": h,
            "search_errors": qerrs,
        })

    try:
        stats, _, _ = query_c.call("get_memory_stats", {})
    except Exception:
        stats = {}

    return {
        "scale_steps": scale_steps,
        "queries_per_step": queries_per_step,
        "query_rate_req_s": query_rate,
        "per_step": results,
        "ingest_histogram": histogram(all_ingest_lats),
        "ingest_errors": all_ingest_errs,
        "ingest_elapsed_s": all_ingest_t,
        "ingest_throughput_ops_s": (
            len(all_ingest_lats) / all_ingest_t if all_ingest_t > 0 else 0.0),
        "final_stats": stats,
    }


def phase_client_sweep(port, client_counts, ops_per_client, rate_per_client,
                       use_embedding, sampler,
                       store_ratio=0.05, retrieve_ratio=0.10):
    print(f"\n=== client-sweep: counts {client_counts}, {ops_per_client} ops/client "
          f"at {rate_per_client:.1f} req/s/client ===")
    print(f"  mix: store={store_ratio:.0%} retrieve={retrieve_ratio:.0%} "
          f"search={1 - store_ratio - retrieve_ratio:.0%}")

    seed = Client(port)
    known_ids = []
    for i in range(300):
        try:
            inner, err, _ = seed.call(
                "store_memory",
                {"content": gen_content(50_000_000 + i),
                 "source_type": "perf_sweep_seed"})
            if not err and inner.get("id"):
                known_ids.append(inner["id"])
        except Exception:
            pass
    print(f"  seeded {len(known_ids)} retrieve targets")

    results_by_count = []
    for c in client_counts:
        if sampler:
            sampler.mark(f"clients_{c}")
        print(f"\n  [clients={c}]")
        lats = defaultdict(list)
        errs = defaultdict(int)
        lock = threading.Lock()

        def worker(wid):
            try:
                cl = Client(port)
            except Exception:
                with lock:
                    errs["client_init"] += ops_per_client
                return
            r = random.Random(wid * 99991 + 7)
            interval = 1.0 / rate_per_client if rate_per_client > 0 else 0.0
            next_tick = time.monotonic()
            local = defaultdict(list)
            local_err = defaultdict(int)
            tool_search = "search_hybrid" if use_embedding else "search_keyword"
            for i in range(ops_per_client):
                if interval > 0:
                    now = time.monotonic()
                    if now < next_tick:
                        time.sleep(next_tick - now)
                    next_tick += interval
                roll = r.random()
                op = None
                try:
                    if roll < store_ratio:
                        op = "store_memory"
                        _, err, lat = cl.call(op, {
                            "content": gen_content(wid * 10_000_000 + i + 999),
                            "source_type": "perf_sweep"})
                    elif roll < store_ratio + retrieve_ratio and known_ids:
                        op = "retrieve_memory"
                        _, err, lat = cl.call(op, {"id": r.choice(known_ids)})
                    else:
                        op = tool_search
                        _, err, lat = cl.call(op, {
                            "query": gen_query(wid * 7919 + i), "top_k": 10})
                    if err:
                        local_err[op] += 1
                    else:
                        local[op].append(lat)
                except Exception:
                    if op:
                        local_err[op] += 1
                    else:
                        local_err["unknown"] += 1
            with lock:
                for k, v in local.items():
                    lats[k].extend(v)
                for k, v in local_err.items():
                    errs[k] += v

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(c)]
        t0 = time.monotonic()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        dt = time.monotonic() - t0
        total_ok = sum(len(v) for v in lats.values())
        total_err = sum(errs.values())
        print(f"    elapsed={dt:.1f}s  ok={total_ok}  err={total_err}  "
              f"overall={total_ok / dt if dt > 0 else 0.0:.1f} ops/s")
        for op in sorted(lats.keys()):
            print(fmt_row(op, histogram(lats[op])))

        results_by_count.append({
            "client_count": c,
            "elapsed_s": dt,
            "total_ops_ok": total_ok,
            "total_ops_err": total_err,
            "overall_throughput_ops_s": total_ok / dt if dt > 0 else 0.0,
            "per_op": {op: histogram(lats[op]) for op in lats},
            "errors": dict(errs),
        })

    return {
        "client_counts": client_counts,
        "ops_per_client": ops_per_client,
        "rate_per_client_req_s": rate_per_client,
        "mix_store_ratio": store_ratio,
        "mix_retrieve_ratio": retrieve_ratio,
        "per_count": results_by_count,
    }


def print_scale_summary(scale_result):
    print("\n  --- scale sweep: search latency vs corpus size ---")
    print(f"  {'corpus':>10s}  {'n':>5s}  {'avg':>7s}  {'p50':>7s}  "
          f"{'p95':>7s}  {'p99':>7s}  {'max':>7s}  (ms)")
    for step in scale_result["per_step"]:
        h = step["search_histogram"]
        print(f"  {step['corpus_size']:>10d}  {h.get('n', 0):>5d}  "
              f"{h.get('avg_ms', 0):>7.2f}  {h.get('p50_ms', 0):>7.2f}  "
              f"{h.get('p95_ms', 0):>7.2f}  {h.get('p99_ms', 0):>7.2f}  "
              f"{h.get('max_ms', 0):>7.2f}")


def print_client_summary(client_result):
    print("\n  --- client sweep: search latency vs concurrent clients ---")
    print(f"  {'clients':>8s}  {'n':>5s}  {'avg':>7s}  {'p50':>7s}  "
          f"{'p95':>7s}  {'p99':>7s}  {'max':>7s}  (ms, search only)")
    for row in client_result["per_count"]:
        search_key = next((k for k in row["per_op"]
                           if k.startswith("search_")), None)
        if not search_key:
            continue
        h = row["per_op"][search_key]
        print(f"  {row['client_count']:>8d}  {h.get('n', 0):>5d}  "
              f"{h.get('avg_ms', 0):>7.2f}  {h.get('p50_ms', 0):>7.2f}  "
              f"{h.get('p95_ms', 0):>7.2f}  {h.get('p99_ms', 0):>7.2f}  "
              f"{h.get('max_ms', 0):>7.2f}")


def parse_int_list(s):
    return [int(x) for x in s.split(",") if x.strip()]


def main():
    ap = argparse.ArgumentParser(
        description="mnemond realistic-workload perf harness")
    ap.add_argument("--scale-steps", type=parse_int_list,
                    default=[1000, 10000],
                    help="corpus sizes to sweep through (default 1000,10000)")
    ap.add_argument("--queries-per-step", type=int, default=100,
                    help="search queries at each scale step (default 100)")
    ap.add_argument("--query-rate", type=float, default=2.0,
                    help="queries/sec during scale sweep (default 2.0)")
    ap.add_argument("--client-counts", type=parse_int_list,
                    default=[1, 2, 5, 10],
                    help="concurrent client counts to sweep (default 1,2,5,10)")
    ap.add_argument("--ops-per-client", type=int, default=50,
                    help="ops per client in client sweep (default 50)")
    ap.add_argument("--rate-per-client", type=float, default=2.0,
                    help="req/s rate per client (default 2.0)")
    ap.add_argument("--mode",
                    choices=["scale", "clients", "both"], default="both")
    ap.add_argument("--no-embedding", action="store_true",
                    help="disable embedding model (keyword-only path)")
    ap.add_argument("--model-path", default=DEFAULT_MODEL)
    ap.add_argument("--reader-pool", type=int, default=4)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--output", help="write JSON results to this file")
    ap.add_argument("--keep-data", action="store_true")
    ap.add_argument("--data-dir",
                    help="persistent data dir (skips tmpdir, not auto-cleaned)")
    ap.add_argument("--rss-interval", type=float, default=1.0)
    args = ap.parse_args()

    if not os.path.exists(BINARY):
        print(f"ERROR: {BINARY} not found. Run from build/ directory.")
        sys.exit(1)

    use_embedding = not args.no_embedding
    if use_embedding and not os.path.exists(args.model_path):
        print(f"WARN: embedding model not found at {args.model_path}")
        print("WARN: falling back to keyword-only. Override with --model-path.")
        use_embedding = False

    user_owns_data = bool(args.data_dir)
    tmpdir = args.data_dir or f"/tmp/mnemon_perf_native_{os.getpid()}"
    print("=" * 74)
    print("  mnemond realistic-workload perf harness")
    print(f"  binary        : {BINARY}")
    print(f"  transport     : HTTP 127.0.0.1:{args.port}")
    print(f"  embedding     : "
          f"{'ON (' + args.model_path + ')' if use_embedding else 'OFF (keyword-only)'}")
    print(f"  reader pool   : {args.reader_pool}")
    print(f"  mode          : {args.mode}")
    print(f"  scale steps   : {args.scale_steps}")
    print(f"  client counts : {args.client_counts}")
    print(f"  tmpdir        : {tmpdir}")
    print("=" * 74)

    proc = start_daemon(tmpdir, args.port, args.model_path,
                        use_embedding, reader_pool=args.reader_pool)
    sampler = RSSampler(proc.pid, interval_s=args.rss_interval)
    sampler.start()

    results = {
        "config": {
            "scale_steps": args.scale_steps,
            "queries_per_step": args.queries_per_step,
            "query_rate": args.query_rate,
            "client_counts": args.client_counts,
            "ops_per_client": args.ops_per_client,
            "rate_per_client": args.rate_per_client,
            "use_embedding": use_embedding,
            "mode": args.mode,
            "reader_pool": args.reader_pool,
        },
        "phases": {},
    }

    try:
        if args.mode in ("scale", "both"):
            results["phases"]["scale_sweep"] = phase_scale_sweep(
                args.port, args.scale_steps, args.queries_per_step,
                args.query_rate, use_embedding, sampler)
            print_scale_summary(results["phases"]["scale_sweep"])
        if args.mode in ("clients", "both"):
            if args.mode == "clients":
                seed_target = args.scale_steps[-1] if args.scale_steps else 10000
                seed_c = Client(args.port)
                existing = 0
                try:
                    stats, _, _ = seed_c.call("get_memory_stats", {})
                    existing = int(stats.get("total_memories", 0))
                except Exception:
                    existing = 0
                if existing >= seed_target:
                    print(f"\n  [prep] corpus already at {existing} "
                          f"(>= target {seed_target}), skipping seed ingest")
                else:
                    delta = seed_target - existing
                    print(f"\n  [prep] seeding corpus from {existing} "
                          f"to {seed_target} (+{delta})...")
                    ingest_block(seed_c, existing, delta)
            results["phases"]["client_sweep"] = phase_client_sweep(
                args.port, args.client_counts, args.ops_per_client,
                args.rate_per_client, use_embedding, sampler)
            print_client_summary(results["phases"]["client_sweep"])
    finally:
        sampler.stop()
        rss = sampler.summary()
        if rss:
            print("\n--- RSS sampling ---")
            print(f"  start={rss['rss_kb_start']:>10,} KB   "
                  f"peak={rss['rss_kb_peak']:>10,} KB   "
                  f"end={rss['rss_kb_end']:>10,} KB   "
                  f"growth={rss['rss_kb_growth']:+,} KB   "
                  f"samples={rss['samples']}")
            results["rss"] = rss
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)

        if not args.keep_data and not user_owns_data:
            import shutil
            shutil.rmtree(tmpdir, ignore_errors=True)
        else:
            print(f"\n  (data preserved at {tmpdir})")

    if args.output:
        with open(args.output, "w") as f:
            json.dump(results, f, indent=2)
        print(f"\nresults written to {args.output}")

    print("\n" + "=" * 74)
    print("  complete")
    print("=" * 74)


if __name__ == "__main__":
    main()
