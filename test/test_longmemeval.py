#!/usr/bin/env python3
"""
LongMemEval Benchmark for mnemond

Two-phase architecture:
  Phase 1 (generate): Ingest sessions into mnemond, run agent to answer
    questions, save results to JSONL. No network calls. Pure performance.
  Phase 2 (judge): Read JSONL, send to judge panel with rate-limit-safe
    pacing. Can be re-run independently with different judges.

Usage:
    cd build

    # Phase 1: Generate answers (local only, no API needed)
    LD_LIBRARY_PATH=~/.local/lib python3 ../test/test_longmemeval.py generate

    # Phase 2: Judge answers (uses API keys)
    LD_LIBRARY_PATH=~/.local/lib python3 ../test/test_longmemeval.py judge

    # Both phases (legacy single-run mode)
    LD_LIBRARY_PATH=~/.local/lib python3 ../test/test_longmemeval.py

Options (env vars):
    MNEMON_LONGMEMEVAL_VARIANT=oracle  Dataset: oracle or s (default: oracle)
    MNEMON_LONGMEMEVAL_AGENT=auto      Agent: auto, api:NAME, none (default: auto)
    MNEMON_LONGMEMEVAL_JUDGES=auto     Judges: auto, list, none (default: auto)
    MNEMON_LONGMEMEVAL_VERBOSE=1       Verbose output
    MNEMON_LONGMEMEVAL_TOPK=10         Search top-K (default 10)
    MNEMON_LONGMEMEVAL_LIMIT=0         Limit questions (0 = all 500)
    MNEMON_LONGMEMEVAL_RESULTS=path    Results JSONL path (default: auto)
    MNEMON_LONGMEMEVAL_PACE=2.0        Judge pacing: seconds between API calls
    MNEMON_LONGMEMEVAL_MODEL=path      Override chat model GGUF path
    MNEMON_USE_VALGRIND=1              Run under valgrind

Reference: https://arxiv.org/abs/2410.10813
"""

import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from longmemeval_agent import (
    detect_hardware, select_chat_model, ensure_chat_model,
    ReActAgent, ApiReActAgent, IngestionPipeline, JudgePanel,
    load_api_key, JUDGE_APIS, _api_call,
)

BINARY = "./mnemond"
LLAMA_BINARY = os.path.expanduser("~/.local/bin/llama-completion")

ABILITY_MAP = {
    "single-session-user": "IE", "single-session-assistant": "IE",
    "single-session-preference": "IE", "multi-session": "MR",
    "temporal-reasoning": "TR", "knowledge-update": "KU",
}
ABILITY_NAMES = {
    "IE": "Information Extraction", "MR": "Multi-Session Reasoning",
    "TR": "Temporal Reasoning", "KU": "Knowledge Updates",
}


# ---------------------------------------------------------------------------
# MCP helpers
# ---------------------------------------------------------------------------

def send_recv(proc, request):
    line = json.dumps(request) + "\n"
    try:
        proc.stdin.write(line)
        proc.stdin.flush()
    except (BrokenPipeError, OSError):
        return None
    resp_line = proc.stdout.readline()
    if not resp_line or not resp_line.strip():
        return None
    try:
        return json.loads(resp_line.strip())
    except json.JSONDecodeError:
        return None


def call_tool(proc, name, arguments, req_id):
    resp = send_recv(proc, {
        "jsonrpc": "2.0", "id": req_id,
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments}
    })
    if not resp:
        return {}, True
    result = resp.get("result", {})
    is_error = result.get("isError", False)
    content = result.get("content", [])
    if content:
        inner = json.loads(content[0].get("text", "{}"))
    else:
        inner = {}
    return inner, is_error


def start_server(tmpdir):
    env = os.environ.copy()
    home = os.path.expanduser("~")
    env["LD_LIBRARY_PATH"] = f"{home}/.local/lib:" + env.get("LD_LIBRARY_PATH", "")
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
        stderr=subprocess.PIPE, text=True, env=env)
    send_recv(proc, {
        "jsonrpc": "2.0", "id": 0, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                   "clientInfo": {"name": "longmemeval", "version": "4.0"}}
    })
    proc.stdin.write(json.dumps({
        "jsonrpc": "2.0", "method": "notifications/initialized"
    }) + "\n")
    proc.stdin.flush()
    return proc


def stop_server(proc):
    try:
        proc.stdin.close()
        proc.wait(timeout=10)
    except Exception:
        proc.kill()


# ---------------------------------------------------------------------------
# Dataset
# ---------------------------------------------------------------------------

def load_dataset(variant):
    test_dir = os.path.dirname(os.path.abspath(__file__))
    fname = ("longmemeval_oracle.json" if variant == "oracle"
             else "longmemeval_s.json")
    path = os.path.join(test_dir, fname)
    if not os.path.exists(path):
        print(f"ERROR: {path} not found")
        sys.exit(1)
    with open(path) as f:
        return json.load(f)


def parse_dataset_date(date_str):
    if not date_str:
        return None
    cleaned = re.sub(r'\s*\([^)]*\)\s*', ' ', date_str).strip()
    parts = cleaned.split()
    if len(parts) < 2:
        return None
    date_part = parts[0].replace('/', '-')
    time_part = parts[1] + (":00" if len(parts[1]) == 5 else "")
    return f"{date_part}T{time_part}Z"


def session_to_content(session, session_id=None, session_date=None):
    parts = []
    if session_id:
        parts.append(f"[Session ID: {session_id}]")
    if session_date:
        parts.append(f"[Session date: {session_date}]")
    for turn in session:
        parts.append(f"{turn['role'].capitalize()}: {turn['content']}")
    return "\n".join(parts)


# ---------------------------------------------------------------------------
# Retrieval metrics
# ---------------------------------------------------------------------------
# Standard information retrieval metrics for direct comparison with
# MemPalace (R@5=96.6%), Supermemory, and academic baselines.
#
#   R@K  (Recall@K):  Did ANY ground-truth session appear in top-K?
#   NDCG@K:           Normalized Discounted Cumulative Gain -- measures
#                     ranking quality (right session at rank 1 > rank 5).
#   Session recall:   Fraction of ground-truth sessions retrieved (any K).

import math


def normalize(text):
    text = text.lower()
    text = re.sub(r'[^\w\s]', ' ', text)
    return " ".join(text.split())


def _dcg(relevances, k):
    """Discounted Cumulative Gain."""
    score = 0.0
    for i, rel in enumerate(relevances[:k]):
        score += rel / math.log2(i + 2)
    return score


def _ndcg(relevances, k):
    """Normalized DCG."""
    ideal = sorted(relevances, reverse=True)
    idcg = _dcg(ideal, k)
    if idcg == 0:
        return 0.0
    return _dcg(relevances, k) / idcg


def evaluate_retrieval(search_results, question):
    """Compute retrieval quality metrics.

    Returns dict with: recall_at_5, recall_at_10, ndcg_at_5, ndcg_at_10,
    session_recall, answer_found, num_results, retrieval_ms (if timed externally).
    """
    answer = str(question["answer"])
    correct_ids = set(question["answer_session_ids"])

    # Extract ranked session IDs from search results (preserving order)
    ranked_ids = []
    all_content = ""
    for r in search_results:
        content = r.get("content", "")
        all_content += " " + content
        for line in content.split("\n"):
            if line.startswith("[Session ID: "):
                sid = line[13:-1]
                if sid not in ranked_ids:
                    ranked_ids.append(sid)
                break  # One session ID per result

    # R@K: did any correct session appear in top K?
    def recall_at_k(k):
        top_k = set(ranked_ids[:k])
        return 1.0 if (correct_ids & top_k) else 0.0

    # NDCG@K: ranking quality
    relevances = [1.0 if sid in correct_ids else 0.0 for sid in ranked_ids]

    # Session recall: fraction of correct sessions anywhere in results
    all_retrieved = set(ranked_ids)
    session_recall = (len(correct_ids & all_retrieved) / len(correct_ids)
                      if correct_ids else 0.0)

    # Substring match (legacy metric)
    all_norm = normalize(all_content)
    ans_norm = normalize(answer)
    answer_found = ans_norm in all_norm if ans_norm else False

    return {
        "recall_at_5": recall_at_k(5),
        "recall_at_10": recall_at_k(10),
        "ndcg_at_5": _ndcg(relevances, 5),
        "ndcg_at_10": _ndcg(relevances, 10),
        "session_recall": session_recall,
        "answer_found": answer_found,
        "num_results": len(search_results),
    }


# =========================================================================
# Phase 1: Generate answers
# =========================================================================

def start_server_with_embeddings(tmpdir):
    """Start mnemond with embedding model enabled for realistic benchmarking."""
    env = os.environ.copy()
    home = os.path.expanduser("~")
    env["LD_LIBRARY_PATH"] = f"{home}/.local/lib:" + env.get("LD_LIBRARY_PATH", "")
    model = os.path.join(home, ".local/share/mnemond/models",
                         "nomic-embed-text-v1.5.Q8_0.gguf")
    os.makedirs(tmpdir, exist_ok=True)
    conf = os.path.join(tmpdir, "bench.conf")
    with open(conf, "w") as f:
        f.write(f"[general]\ndata_dir = {tmpdir}/data\nlog_level = warning\n")
        f.write("[lmdb]\nmap_size_gb = 4\nmax_readers = 32\n")
        if os.path.exists(model):
            f.write(f"[embedding]\nmodel_path = {model}\n"
                    f"dimensions = 768\ngpu_layers = 99\n")
        else:
            f.write("[embedding]\nmodel_path = none\ndimensions = 768\n")
    use_valgrind = os.environ.get("MNEMON_USE_VALGRIND", "0") == "1"
    cmd = ["valgrind", "-q", BINARY] if use_valgrind else [BINARY]
    cmd += ["--stdio", "--config", conf]
    # Send stderr to a log file instead of a pipe to prevent deadlock.
    # With 19K+ sessions, mnemond generates enough warnings to fill
    # a 64KB pipe buffer, blocking both processes.
    stderr_log = os.path.join(tmpdir, "mnemond.log")
    stderr_fh = open(stderr_log, "w")
    proc = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=stderr_fh, text=True, env=env)
    send_recv(proc, {
        "jsonrpc": "2.0", "id": 0, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                   "clientInfo": {"name": "longmemeval", "version": "5.0"}}
    })
    proc.stdin.write(json.dumps({
        "jsonrpc": "2.0", "method": "notifications/initialized"
    }) + "\n")
    proc.stdin.flush()

    # Wait for embedding model to load
    if os.path.exists(model):
        import time as _t
        _t.sleep(5)

    return proc, os.path.exists(model)


def phase_generate(dataset, variant, agent_mode, model_path, model_info,
                   top_k, verbose, results_path):
    """Run all questions through mnemond + agent. Save results to JSONL.

    Architecture: single persistent mnemond instance with all sessions
    pre-loaded (realistic). Embeddings enabled if model is available.
    """
    home = os.path.expanduser("~")
    tmpdir = os.path.join(home, f".mnemon_longmemeval_{os.getpid()}")
    req_id = 100
    t_start = time.monotonic()

    # ---- Step 1: Start persistent mnemond with embeddings ----
    proc, has_embeddings = start_server_with_embeddings(tmpdir)
    print(f"    Embeddings: {'enabled (GPU)' if has_embeddings else 'disabled'}")

    # Verify server is responding
    r, ie = call_tool(proc, "get_memory_stats", {}, 1)
    if ie or proc.poll() is not None:
        print("    ERROR: mnemond failed to start. Check stderr.")
        stderr = proc.stderr.read() if proc.poll() is not None else ""
        for line in stderr.split("\n")[-10:]:
            if line.strip():
                print(f"      {line}")
        return results_path

    # ---- Step 2: Ingest all unique sessions ----
    # Dedup by session_id -- many sessions are shared across questions
    all_sessions = {}  # sid -> (session_turns, date_str)
    for q in dataset:
        for si, sess in enumerate(q["haystack_sessions"]):
            sid = q["haystack_session_ids"][si]
            if sid not in all_sessions:
                sdate = (q["haystack_dates"][si]
                         if si < len(q.get("haystack_dates", [])) else None)
                all_sessions[sid] = (sess, sdate)

    total_sessions = len(all_sessions)
    print(f"    Ingesting {total_sessions} unique sessions "
          f"({len(dataset)} questions)...")

    ingest_start = time.monotonic()
    ingested = 0
    stored_ok = 0
    store_errors = 0
    for sid, (sess, sdate) in all_sessions.items():
        req_id += 1
        content = session_to_content(sess, session_id=sid, session_date=sdate)
        iso_date = parse_dataset_date(sdate) if sdate else None
        args = {
            "content": content,
            "source_type": "conversation",
            "tags": ["longmemeval", f"session:{sid}"],
        }
        if iso_date:
            args["created_at"] = iso_date
        r, ie = call_tool(proc, "store_memory", args, req_id)
        if ie or not r.get("id"):
            store_errors += 1
            if store_errors <= 5 or verbose:
                err_msg = r.get("error", "empty response") if r else "None"
                print(f"      WARN: store failed for {sid} "
                      f"({len(content)} chars): {err_msg}")
            if proc.poll() is not None:
                print("      ERROR: mnemond process died!")
                break
        else:
            stored_ok += 1
        ingested += 1
        if ingested % 500 == 0:
            elapsed = time.monotonic() - ingest_start
            rate = ingested / elapsed
            eta = (total_sessions - ingested) / rate if rate > 0 else 0
            print(f"      {ingested}/{total_sessions} "
                  f"({elapsed:.0f}s, {rate:.0f}/s, ETA {eta:.0f}s)")

    ingest_ms = (time.monotonic() - ingest_start) * 1000
    print(f"    Ingested {stored_ok}/{ingested} sessions in {ingest_ms/1000:.1f}s "
          f"({ingest_ms/max(ingested,1):.1f}ms/session)"
          f"{f' ({store_errors} errors)' if store_errors else ''}")

    # ---- Step 3: Extract events for temporal reasoning ----
    # Run extract_events on a sample of sessions to populate event entities
    event_start = time.monotonic()
    events_created = 0
    for sid, (sess, sdate) in list(all_sessions.items()):
        # Extract from user turns only (where events are mentioned)
        user_text = "\n".join(
            t["content"] for t in sess if t["role"] == "user")
        if len(user_text) < 50:
            continue
        context_year = 2023
        if sdate:
            iso = parse_dataset_date(sdate)
            if iso:
                try:
                    context_year = int(iso[:4])
                except (ValueError, IndexError):
                    pass
        req_id += 1
        r, ie = call_tool(proc, "extract_events", {
            "content": user_text[:4000],
            "context_year": context_year,
            "create_entities": True,
        }, req_id)
        if not ie:
            events_created += r.get("entities_created", 0)

    event_ms = (time.monotonic() - event_start) * 1000
    print(f"    Extracted {events_created} event entities ({event_ms/1000:.1f}s)")

    # ---- Step 4: Get stats ----
    req_id += 1
    stats, _ = call_tool(proc, "get_memory_stats", {}, req_id)
    print(f"    Database: {stats.get('total_memories', 0)} memories, "
          f"{stats.get('total_entities', 0)} entities, "
          f"{stats.get('memory_vectors', 0)} vectors")

    # ---- Step 5: Query each question against full corpus ----
    print(f"\n    Querying {len(dataset)} questions...")
    results_file = open(results_path, "w")
    total = len(dataset)
    query_times = []

    for qi, question in enumerate(dataset):
        qid = question["question_id"]
        qtype = question["question_type"]
        query_text = question["question"]
        ability = ABILITY_MAP.get(qtype, "??")

        # -- Agent or direct search --
        generated_answer = None
        agent_timing = {"inference_ms": 0, "tool_ms": 0, "turns": 0}
        agent_start = time.monotonic()

        if agent_mode == "auto" and model_path:
            agent = ReActAgent(proc, LLAMA_BINARY, model_path,
                               ctx_size=model_info["ctx_size"],
                               gpu_layers=model_info["gpu_layers"])
            generated_answer, agent_timing = agent.answer(query_text)
        elif agent_mode.startswith("api:"):
            api_name = agent_mode[4:]
            try:
                agent = ApiReActAgent(proc, api_name)
                generated_answer, agent_timing = agent.answer(query_text)
            except ValueError:
                pass

        agent_ms = (time.monotonic() - agent_start) * 1000

        # -- Retrieval metrics (always run search_hybrid) --
        req_id += 1
        t0 = time.monotonic()
        r, ie = call_tool(proc, "search_hybrid",
                          {"query": query_text, "top_k": top_k}, req_id)
        search_ms = (time.monotonic() - t0) * 1000
        query_times.append(search_ms)
        search_results = r.get("results", []) if not ie else []
        retrieval = evaluate_retrieval(search_results, question)

        result = {
            "question_id": qid,
            "question_type": qtype,
            "ability": ability,
            "question": query_text,
            "expected_answer": str(question["answer"]),
            "generated_answer": generated_answer,
            "recall_at_5": retrieval["recall_at_5"],
            "recall_at_10": retrieval["recall_at_10"],
            "ndcg_at_5": retrieval["ndcg_at_5"],
            "ndcg_at_10": retrieval["ndcg_at_10"],
            "session_recall": retrieval["session_recall"],
            "answer_found": retrieval["answer_found"],
            "num_results": retrieval["num_results"],
            "search_ms": search_ms,
            "ingest_ms": ingest_ms / total,  # Amortized
            "agent_ms": agent_ms,
            "agent_inference_ms": agent_timing["inference_ms"],
            "agent_tool_ms": agent_timing["tool_ms"],
            "agent_turns": agent_timing["turns"],
            "corpus_size": total_sessions,
            "events_total": events_created,
        }

        results_file.write(json.dumps(result) + "\n")
        results_file.flush()

        if verbose:
            r5 = retrieval["recall_at_5"]
            sr = retrieval["session_recall"] * 100
            print(f"  [{qi+1}/{total}] {qid} R@5={'HIT' if r5 else 'MISS'} "
                  f"sr={sr:.0f}% {search_ms:.1f}ms", end="")
            if generated_answer:
                print(f"  -> {generated_answer[:60]}...", end="")
            print()

        if not verbose:
            if (qi + 1) % 50 == 0:
                pct = (qi + 1) / total * 100
                elapsed = time.monotonic() - t_start
                print(f"  [{qi+1}/{total}] {pct:.0f}% ({elapsed:.0f}s)")
            elif (qi + 1) % 10 == 0:
                print(".", end="", flush=True)

    results_file.close()
    stop_server(proc)

    total_elapsed = time.monotonic() - t_start

    # -- Summary --
    avg_search = _avg(query_times)
    print(f"\n\n  Phase 1 complete: {total} questions")
    print(f"    Corpus:     {total_sessions} sessions"
          f" ({events_created} event entities)")
    print(f"    Ingest:     {ingest_ms/1000:.1f}s "
          f"({ingest_ms/total_sessions:.1f}ms/session)")
    print(f"    Avg search: {avg_search:.1f}ms")
    print(f"    Total time: {total_elapsed:.0f}s")
    print(f"  Results saved to: {results_path}")

    shutil.rmtree(tmpdir, ignore_errors=True)
    return results_path


# =========================================================================
# Phase 2: Judge answers
# =========================================================================

def phase_judge(results_path, judge_panel, pace, verbose):
    """Read JSONL results, judge each with the panel at controlled pace."""
    results = []
    with open(results_path) as f:
        for line in f:
            line = line.strip()
            if line:
                results.append(json.loads(line))

    total = len(results)
    print(f"\n  Judging {total} answers with [{', '.join(judge_panel.names)}]")
    print(f"  Pace: {pace:.1f}s between calls")

    t_start = time.monotonic()

    for i, result in enumerate(results):
        qid = result["question_id"]
        qtype = result["question_type"]
        question = result["question"]
        expected = result["expected_answer"]
        generated = result.get("generated_answer")

        if generated:
            judge_result = judge_panel.evaluate(
                qtype, qid, question, expected, generated)
        elif "_abs" in qid:
            judge_result = judge_panel.evaluate(
                qtype, qid, question, expected,
                "I don't have that information.")
        else:
            judge_result = {"correct": False, "votes": {}, "agree": 0,
                            "total": 0}

        result["judge_correct"] = judge_result["correct"]
        result["judge_votes"] = judge_result["votes"]

        if verbose:
            jc = judge_result["correct"]
            votes = judge_result["votes"]
            vote_str = "/".join("Y" if v else "N" for v in votes.values())
            print(f"  [{i+1}/{total}] {qid}: "
                  f"{'YES' if jc else 'NO'}[{vote_str}]")
        elif (i + 1) % 50 == 0:
            pct = (i + 1) / total * 100
            elapsed = time.monotonic() - t_start
            print(f"  [{i+1}/{total}] {pct:.0f}% ({elapsed:.0f}s)")
        elif (i + 1) % 10 == 0:
            print(".", end="", flush=True)

        # Rate-limit pacing
        if pace > 0 and i < total - 1:
            time.sleep(pace)

    judge_elapsed = time.monotonic() - t_start

    # Save judged results back
    judged_path = results_path.replace(".jsonl", ".judged.jsonl")
    with open(judged_path, "w") as f:
        for r in results:
            f.write(json.dumps(r) + "\n")

    print(f"\n  Judging complete: {judge_elapsed:.0f}s")
    print(f"  Judged results: {judged_path}")

    return results, judged_path


# =========================================================================
# Report
# =========================================================================

def _avg(vals):
    return sum(vals) / len(vals) if vals else 0.0


def print_report(results, variant, judge_names=None):
    """Print the full benchmark report from result dicts."""
    by_type = defaultdict(list)
    by_ability = defaultdict(list)
    for r in results:
        by_type[r["question_type"]].append(r)
        by_ability[r["ability"]].append(r)

    has_judge = any(r.get("judge_correct") is not None for r in results)
    has_r5 = any("recall_at_5" in r for r in results)

    # ---- Per-type breakdown ----
    print("\n" + "=" * 72)
    print("  RESULTS BY QUESTION TYPE")
    print("=" * 72)

    for qtype in ["single-session-user", "single-session-assistant",
                   "single-session-preference", "multi-session",
                   "temporal-reasoning", "knowledge-update"]:
        rlist = by_type.get(qtype, [])
        if not rlist:
            continue
        n = len(rlist)
        ability = ABILITY_MAP.get(qtype, "??")
        print(f"\n  {qtype} ({ability}):  {n} questions")

        if has_r5:
            r5 = _avg([r.get("recall_at_5", 0) for r in rlist])
            r10 = _avg([r.get("recall_at_10", 0) for r in rlist])
            ndcg5 = _avg([r.get("ndcg_at_5", 0) for r in rlist])
            print(f"    R@5: {r5*100:.1f}%  R@10: {r10*100:.1f}%  "
                  f"NDCG@5: {ndcg5*100:.1f}%")

        if has_judge:
            judged = [r for r in rlist if r.get("judge_correct") is not None]
            if judged:
                jc = sum(1 for r in judged if r["judge_correct"])
                print(f"    Judge accuracy:  {jc}/{len(judged)} "
                      f"({jc/len(judged)*100:.1f}%)")

    # ---- Per-ability breakdown ----
    print("\n" + "=" * 72)
    print("  RESULTS BY MEMORY ABILITY")
    print("=" * 72)

    totals = {"n": 0, "r5": 0, "r10": 0, "ndcg5": 0, "ndcg10": 0,
              "sr": 0, "jc": 0, "jn": 0}

    for aid in ["IE", "MR", "TR", "KU"]:
        rlist = by_ability.get(aid, [])
        if not rlist:
            continue
        n = len(rlist)
        totals["n"] += n

        name = ABILITY_NAMES.get(aid, aid)
        print(f"\n  {aid}: {name}  ({n} questions)")

        if has_r5:
            r5 = _avg([r.get("recall_at_5", 0) for r in rlist])
            r10 = _avg([r.get("recall_at_10", 0) for r in rlist])
            ndcg5 = _avg([r.get("ndcg_at_5", 0) for r in rlist])
            ndcg10 = _avg([r.get("ndcg_at_10", 0) for r in rlist])
            sr = _avg([r.get("session_recall", 0) for r in rlist])
            totals["r5"] += sum(r.get("recall_at_5", 0) for r in rlist)
            totals["r10"] += sum(r.get("recall_at_10", 0) for r in rlist)
            totals["ndcg5"] += sum(r.get("ndcg_at_5", 0) for r in rlist)
            totals["ndcg10"] += sum(r.get("ndcg_at_10", 0) for r in rlist)
            totals["sr"] += sum(r.get("session_recall", 0) for r in rlist)
            print(f"    R@5: {r5*100:.1f}%  R@10: {r10*100:.1f}%  "
                  f"NDCG@5: {ndcg5*100:.1f}%  Session: {sr*100:.1f}%")

        if has_judge:
            judged = [r for r in rlist if r.get("judge_correct") is not None]
            if judged:
                jc = sum(1 for r in judged if r["judge_correct"])
                totals["jc"] += jc
                totals["jn"] += len(judged)
                print(f"    Judge accuracy:  {jc}/{len(judged)} "
                      f"({jc/len(judged)*100:.1f}%)")

    # ---- Overall ----
    tn = totals["n"]
    print(f"\n{'='*72}")
    print(f"  OVERALL ({tn} questions, {variant} variant)")
    print(f"{'='*72}")

    if has_r5 and tn > 0:
        print(f"\n  Retrieval (mnemond):")
        print(f"    R@5:              {totals['r5']/tn*100:.1f}%")
        print(f"    R@10:             {totals['r10']/tn*100:.1f}%")
        print(f"    NDCG@5:           {totals['ndcg5']/tn*100:.1f}%")
        print(f"    NDCG@10:          {totals['ndcg10']/tn*100:.1f}%")
        print(f"    Session recall:   {totals['sr']/tn*100:.1f}%")

    if has_judge and totals["jn"] > 0:
        overall_judge = totals["jc"] / totals["jn"] * 100
        jnames = ', '.join(judge_names) if judge_names else "unknown"
        print(f"\n  Answer quality (agent + judges):")
        print(f"    Judge accuracy:   {totals['jc']}/{totals['jn']} "
              f"({overall_judge:.1f}%)  [{jnames}]")

    # ---- Performance ----
    search = [r["search_ms"] for r in results if "search_ms" in r]
    ingest = [r["ingest_ms"] for r in results if "ingest_ms" in r]
    agent = [r["agent_ms"] for r in results if r.get("agent_ms", 0) > 0]
    inf = [r["agent_inference_ms"] for r in results
           if r.get("agent_inference_ms", 0) > 0]
    tools = [r["agent_tool_ms"] for r in results
             if r.get("agent_tool_ms", 0) > 0]
    turns = [r["agent_turns"] for r in results
             if r.get("agent_turns", 0) > 0]
    corpus = results[0].get("corpus_size", 0) if results else 0
    events = results[0].get("events_total", 0) if results else 0

    print(f"\n  Performance (mnemond):")
    if corpus:
        print(f"    Corpus:           {corpus} sessions, {events} events")
    if ingest:
        total_ingest = sum(ingest)
        print(f"    Ingest:           {_avg(ingest):.1f}ms/session amortized  "
              f"({total_ingest/1000:.1f}s total)")
    if search:
        print(f"    Search:           {_avg(search):.1f}ms avg  "
              f"(p50={sorted(search)[len(search)//2]:.1f}ms  "
              f"p99={sorted(search)[int(len(search)*0.99)]:.1f}ms)")
    if tools:
        print(f"    Tool execution:   {_avg(tools):.1f}ms avg")
    if agent:
        print(f"\n  Performance (agent):")
        print(f"    Total agent:      {_avg(agent):.0f}ms avg")
    if inf:
        print(f"    LLM inference:    {_avg(inf):.0f}ms avg")
    if turns:
        print(f"    Avg turns:        {_avg(turns):.1f}")

    print(f"{'='*72}")


# =========================================================================
# Main
# =========================================================================

def run_benchmark():
    if not os.path.exists(BINARY):
        print(f"ERROR: {BINARY} not found. Run from build/ directory.")
        sys.exit(1)

    # Parse mode from argv
    mode = "both"
    if len(sys.argv) > 1:
        if sys.argv[1] in ("generate", "gen", "g"):
            mode = "generate"
        elif sys.argv[1] in ("judge", "j"):
            mode = "judge"
        elif sys.argv[1] in ("report", "r"):
            mode = "report"

    variant = os.environ.get("MNEMON_LONGMEMEVAL_VARIANT", "oracle")
    verbose = os.environ.get("MNEMON_LONGMEMEVAL_VERBOSE", "0") == "1"
    top_k = int(os.environ.get("MNEMON_LONGMEMEVAL_TOPK", "10"))
    limit = int(os.environ.get("MNEMON_LONGMEMEVAL_LIMIT", "0"))
    agent_mode = os.environ.get("MNEMON_LONGMEMEVAL_AGENT", "auto")
    judge_mode = os.environ.get("MNEMON_LONGMEMEVAL_JUDGES", "auto")
    model_override = os.environ.get("MNEMON_LONGMEMEVAL_MODEL", "")
    pace = float(os.environ.get("MNEMON_LONGMEMEVAL_PACE", "2.0"))
    results_path = os.environ.get("MNEMON_LONGMEMEVAL_RESULTS", "")

    if not results_path:
        results_path = f"longmemeval_{variant}_{agent_mode}.jsonl"

    # Header
    print("=" * 72)
    print("  LongMemEval Benchmark for mnemond")
    print("  (Wu et al., ICLR 2025 -- real dataset)")
    print("=" * 72)

    # Hardware
    hw = detect_hardware()
    print(f"\n  Hardware:")
    print(f"    CPU:  {hw['cpu_model']} ({hw['cpu_cores']} cores)")
    print(f"    RAM:  {hw['ram_total_gb']:.0f}GB total, "
          f"{hw['ram_avail_gb']:.0f}GB available")
    if hw["gpu_vendor"] != "none":
        gpu_str = f"    GPU:  {hw['gpu_model']} ({hw['gpu_vram_gb']:.0f}GB VRAM"
        if hw["gpu_gtt_gb"]:
            gpu_str += f", {hw['gpu_gtt_gb']:.0f}GB GTT"
        if hw["has_rocm"]:
            gpu_str += ", ROCm"
        print(gpu_str + ")")

    # Model setup
    model_path = None
    model_info = None
    if mode in ("generate", "both") and agent_mode == "auto":
        if not os.path.exists(LLAMA_BINARY):
            print(f"\n  WARN: {LLAMA_BINARY} not found, falling back to none")
            agent_mode = "none"
        else:
            models_dir = os.path.expanduser("~/.local/share/mnemond/models")
            model_info = select_chat_model(hw, models_dir, LLAMA_BINARY)
            print(f"\n  Chat model: {model_info['name']}")
            if model_override:
                model_path = model_override
            else:
                try:
                    model_path = ensure_chat_model(model_info, models_dir)
                except RuntimeError as e:
                    print(f"  WARN: {e}")
                    agent_mode = "none"
            if model_path:
                print(f"    Path: {model_path}")

    if agent_mode.startswith("api:"):
        print(f"\n  Agent: {agent_mode} (API-backed ReAct with mnemond tools)")

    # -----------------------------------------------------------------------
    # Phase 1: Generate
    # -----------------------------------------------------------------------
    if mode in ("generate", "both"):
        dataset = load_dataset(variant)
        if limit > 0:
            dataset = dataset[:limit]

        print(f"\n  Variant: {variant}  |  Questions: {len(dataset)}")
        type_counts = defaultdict(int)
        for q in dataset:
            type_counts[q["question_type"]] += 1
        for qt, count in sorted(type_counts.items()):
            print(f"    {qt:<30s} ({ABILITY_MAP.get(qt, '??')}): {count}")

        print(f"\n  Phase 1: Generating answers → {results_path}")
        phase_generate(dataset, variant, agent_mode, model_path, model_info,
                       top_k, verbose, results_path)

    # -----------------------------------------------------------------------
    # Phase 2: Judge
    # -----------------------------------------------------------------------
    judge_panel = None
    judged_results = None

    if mode in ("judge", "both") and judge_mode != "none":
        if not os.path.exists(results_path):
            print(f"\n  ERROR: Results file not found: {results_path}")
            print(f"  Run 'generate' phase first.")
            return 1

        try:
            if judge_mode == "auto":
                judge_panel = JudgePanel()
            else:
                judge_panel = JudgePanel(judge_mode.split(","))
        except ValueError as e:
            print(f"\n  WARN: {e}")

        if judge_panel:
            print(f"\n  Phase 2: Judging with "
                  f"[{', '.join(judge_panel.names)}]")
            judged_results, judged_path = phase_judge(
                results_path, judge_panel, pace, verbose)

    # -----------------------------------------------------------------------
    # Report
    # -----------------------------------------------------------------------
    if judged_results:
        print_report(judged_results, variant, judge_panel.names)
        if judge_panel:
            print(f"\n  Judge API calls: {judge_panel.calls} "
                  f"({judge_panel.errors} errors)")
    elif mode == "report":
        # Load existing results
        rpath = results_path
        judged = rpath.replace(".jsonl", ".judged.jsonl")
        if os.path.exists(judged):
            rpath = judged
        if not os.path.exists(rpath):
            print(f"  ERROR: No results file found: {rpath}")
            return 1
        with open(rpath) as f:
            results = [json.loads(line) for line in f if line.strip()]
        print_report(results, variant)
    elif os.path.exists(results_path):
        with open(results_path) as f:
            results = [json.loads(line) for line in f if line.strip()]
        print_report(results, variant)

    return 0


if __name__ == "__main__":
    sys.exit(run_benchmark())
