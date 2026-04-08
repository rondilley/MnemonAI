# MnemonAI Benchmark Report

**Date**: 2026-04-08
**Version**: mnemond v0.4 (35 MCP tools)
**Hardware**: AMD Ryzen AI MAX+ 395, 64GB UMA, Radeon 8060S (gfx1151), ROCm 7.2.1
**Benchmark**: LongMemEval (Wu et al., ICLR 2025) -- 500 questions, 7 question types

## Executive Summary

mnemond achieves **100% R@5** on the oracle variant (evidence sessions only) and **27.6% R@5** on the realistic S variant (19K sessions with distractors). The oracle result confirms that mnemond's retrieval engine is fundamentally sound. The S variant result reveals that **ranking quality under distractor load is the primary area for improvement**.

| Variant | Corpus | R@5 | R@10 | NDCG@5 | Search Latency |
|---------|--------|-----|------|--------|----------------|
| Oracle (evidence only) | 1-3 sessions | **100%** | 100% | 100% | <1ms |
| S (with distractors) | 19,062 sessions | **27.6%** | 40.6% | 13.8% | 127ms avg |

For comparison, MemPalace reports 96.6% R@5 on the S variant using ChromaDB with default embeddings, and 100% with hybrid search + LLM reranking.

---

## 1. Test Configuration

### Dataset

LongMemEval provides multi-turn conversation sessions with ground-truth answer session IDs, enabling retrieval metric computation without an LLM judge.

| Variant | Questions | Total Sessions | Unique Sessions | Avg Sessions/Question |
|---------|-----------|---------------|-----------------|----------------------|
| Oracle | 500 | 948 | 948 | 1.9 |
| S | 500 | 23,867 | 19,195 | 47.7 |

### Question Types (5 Memory Abilities)

| Type | Ability | Count | Description |
|------|---------|-------|-------------|
| single-session-user | IE | 70 | Recall facts from user messages |
| single-session-assistant | IE | 56 | Recall facts from assistant responses |
| single-session-preference | IE | 30 | Recall user preferences |
| multi-session | MR | 133 | Synthesize across multiple sessions |
| temporal-reasoning | TR | 133 | Time-aware queries, date ordering |
| knowledge-update | KU | 78 | Track evolving information |

### mnemond Configuration

- **Storage**: LMDB (4GB map), FTS5 (WAL mode), usearch HNSW
- **Embedding**: nomic-embed-text-v1.5 Q8_0 (768-dim, GPU-accelerated)
- **Embedding context**: 2048 tokens (model training limit)
- **Search**: Hybrid RRF fusion (keyword + vector + graph)
- **Event extraction**: Regex-based date parser + entity creation

---

## 2. Oracle Variant Results (Retrieval Validation)

Single persistent mnemond instance, no embedding model, keyword-only search.

### Retrieval Metrics

| Question Type | R@5 | R@10 | NDCG@5 |
|---------------|-----|------|--------|
| single-session-user | 100% | 100% | 100% |
| single-session-assistant | 100% | 100% | 100% |
| single-session-preference | 100% | 100% | 100% |
| multi-session | 100% | 100% | 100% |
| temporal-reasoning | 100% | 100% | 100% |
| knowledge-update | 100% | 100% | 100% |
| **Overall** | **100%** | **100%** | **100%** |

### Performance

| Metric | Value |
|--------|-------|
| Total benchmark time | 24 seconds |
| Ingest (948 sessions) | 3.7 seconds (3.9ms/session) |
| Search (500 queries) | <1ms average |
| Knowledge graph | 1,071 entities, 18 relations |

### Interpretation

With only evidence sessions in the corpus (no distractors), mnemond's FTS5 keyword search finds the correct session in the top-5 results 100% of the time. This validates that the retrieval engine is functionally correct.

**Bug found and fixed**: Two sessions were previously blocked by the secret detection FSM flagging URLs and markdown links as high-entropy strings. Fix: exempt words containing `http://` or `https://` from high-entropy detection.

---

## 3. S Variant Results (Realistic Retrieval)

Single persistent mnemond instance, GPU-accelerated embeddings, hybrid search against 19K sessions.

### Retrieval Metrics

| Question Type | R@5 | R@10 | NDCG@5 | Session Recall |
|---------------|-----|------|--------|----------------|
| single-session-user | 11.4% | 25.7% | 5.2% | 25.7% |
| single-session-assistant | **87.5%** | **94.6%** | 46.3% | 94.6% |
| single-session-preference | 3.3% | 6.7% | 1.4% | 6.7% |
| multi-session | 17.3% | 29.3% | 8.3% | 15.6% |
| temporal-reasoning | 15.0% | 33.8% | 7.1% | 18.8% |
| knowledge-update | **47.4%** | **59.0%** | 23.5% | 43.6% |
| **Overall** | **27.6%** | **40.6%** | **13.8%** | **30.5%** |

### Performance

| Metric | Value |
|--------|-------|
| Corpus size | 19,062 memories, 2,376 event entities, 19,062 vectors |
| Ingest (19,195 sessions) | 1,057 seconds (55ms/session with GPU embedding) |
| Ingest errors | 133 (secret detection false positives) |
| Search p50 | 90ms |
| Search p90 | 275ms |
| Search p99 | 618ms |
| Search max | 807ms |

### Per-Ability Analysis

**Information Extraction (IE)**: 37.2% R@5

The dramatic split between `single-session-assistant` (87.5%) and `single-session-user` (11.4%) is the most revealing finding. Assistant responses contain distinctive vocabulary (tool names, technical explanations) that BM25 and embedding similarity match well. User messages are conversational and generic ("I'm thinking of getting...", "Can you help me..."), making them hard to distinguish from 19K other similar conversations.

**Preference questions** (3.3% R@5) are worst because the questions are meta-level ("Can you recommend...") and the answer content is about implicit preferences embedded in casual conversation.

**Multi-Session Reasoning (MR)**: 17.3% R@5

MR requires finding information scattered across 2-4 sessions. The search retrieves based on the question text, which may match only one of the relevant sessions. The knowledge graph should help here (entity connections bridge sessions), but the current regex NER creates limited entity coverage.

**Temporal Reasoning (TR)**: 15.0% R@5

TR questions often reference events by description ("the workshop I attended") rather than exact keywords. The event dates are embedded in conversation text, not indexed for temporal search. The new `extract_events` tool creates event entities, but the search doesn't yet prioritize them.

**Knowledge Updates (KU)**: 47.4% R@5

KU performs best among the hard categories because update questions reference specific topics ("PostgreSQL version", "CI/CD pipeline") that have distinctive keywords.

---

## 4. Answer Quality (Agent + Judges)

With a GPT-4o API agent using mnemond's MCP tools (ReAct loop) and an independent judge panel (Claude, Gemini, xAI -- majority vote):

| Ability | Judge Accuracy | Notes |
|---------|---------------|-------|
| IE | **84.6%** (132/156) | Strong on factual recall |
| MR | 56.4% (75/133) | Struggles with aggregation |
| TR | 56.4% (75/133) | Date arithmetic is hard for LLMs |
| KU | **76.9%** (60/78) | Good at finding updates |
| **Overall** | **68.4%** (342/500) | Oracle variant, GPT-4o agent |

**Context**: This was measured on the oracle variant (100% retrieval). The bottleneck is LLM reasoning quality, not retrieval.

### Comparison with Published Systems

| System | Metric | Score | Notes |
|--------|--------|-------|-------|
| Naive RAG baseline | QA accuracy | ~52% | From paper |
| GPT-4o long-context | QA accuracy | 60-64% | All sessions in context |
| **mnemond + GPT-4o** | **QA accuracy** | **68.4%** | Oracle, independent judges |
| LongMemEval best RAG | QA accuracy | ~72% | Paper's best approach |
| EmergenceMem | QA accuracy | 82-86% | Turn-level + cross-encoder |
| mnemond | R@5 (oracle) | **100%** | Retrieval only |
| mnemond | R@5 (S variant) | **27.6%** | Retrieval only, 19K corpus |
| MemPalace | R@5 (S variant) | 96.6% | ChromaDB raw |
| MemPalace hybrid+rerank | R@5 (S variant) | 100% | + Claude Haiku reranking |

---

## 5. Performance Profile

### Ingestion

| Config | Sessions | Time | Rate | Notes |
|--------|----------|------|------|-------|
| No embedding | 948 | 3.7s | **3.9ms/session** | FTS5 + LMDB only |
| GPU embedding | 19,195 | 1,057s | **55ms/session** | nomic-embed + FTS5 + LMDB + usearch |

GPU embedding dominates ingestion time. Without embeddings, mnemond ingests at ~250 sessions/second.

### Search Latency (19K corpus, hybrid search)

| Percentile | Latency |
|------------|---------|
| p10 | 42ms |
| p50 | 90ms |
| p90 | 275ms |
| p99 | 618ms |
| max | 807ms |

Search latency includes FTS5 BM25 + usearch HNSW + graph traversal + RRF fusion.

### Resource Usage

| Resource | Value |
|----------|-------|
| mnemond RSS | 663MB (19K memories with embeddings) |
| LMDB map | 4GB allocated |
| Embedding model VRAM | ~512MB |

---

## 6. Bugs Found and Fixed

| Issue | Root Cause | Fix |
|-------|-----------|-----|
| Secret detection false positives on URLs | High-entropy check flagged URL strings >32 chars | Skip words containing `http://` or `https://` |
| Secret detection false positives on markdown links | `[text](https://...)` treated as single word | Search for URL scheme anywhere in word, not just start |
| Embedding crash on long sessions | `n_ubatch` (512) < token count, causing `GGML_ASSERT` | Set `n_ctx`, `n_batch`, `n_ubatch` from model's `n_ctx_train` |
| Embedding context too small | Hardcoded `n_ctx=2048` instead of reading from model | Use `llama_model_n_ctx_train()` |
| GDB backtrace corrupts MCP channel | `ggml_print_backtrace()` spawns GDB which inherits stdout | Set `GGML_NO_BACKTRACE=1` env var |
| stderr pipe deadlock with 19K sessions | Warning output fills 64KB pipe buffer, blocking both processes | Redirect stderr to log file instead of pipe |
| `store_memory` / `import_batch` cannot backdate | `created_at` hardcoded to `mnemon_time_ms()` | Accept optional `created_at` ISO8601 parameter |

---

## 7. Identified Gaps and Improvement Roadmap

### Critical (High Impact on R@5)

1. **Semantic search quality**: With 19K sessions, BM25 keyword matching is insufficient. The embedding model (nomic-embed, 2048 token context) truncates long sessions, losing information. Consider:
   - Chunking long sessions into overlapping segments before embedding
   - Using a longer-context embedding model (e.g., nomic-embed-text-v2 if available)
   - Re-ranking the top-50 BM25 results using embedding similarity

2. **User message retrieval**: 11.4% R@5 on user messages vs 87.5% on assistant messages. User turns are conversational and generic. Consider:
   - Indexing user and assistant turns separately
   - Extracting key facts/entities from user turns and storing as searchable metadata
   - Query expansion: reformulate search queries to include likely assistant vocabulary

3. **Preference retrieval**: 3.3% R@5. Preferences are implicit in conversation context. Consider:
   - Explicit preference extraction during ingestion (entity type "preference")
   - Separate preference search tool

### Important (Medium Impact)

4. **Multi-session synthesis**: 17.3% R@5. The search returns sessions matching the query, but MR questions need data from multiple sessions that individually may not match. Consider:
   - Graph-based retrieval: find entities mentioned in the query, traverse to related sessions
   - Multi-hop search: search, extract entities, search again for related content

5. **Temporal event search integration**: Event entities are created but `search_events` isn't used by default search. Consider:
   - Including event entity results in `search_hybrid` RRF fusion
   - Boosting sessions that contain matching event dates

6. **Embedding context window**: 2048 tokens (~8KB) truncates ~30% of S-variant sessions. Consider:
   - Chunked embedding with overlap
   - Longer-context embedding models
   - Mean pooling across chunks

### Nice-to-Have

7. **Secret detection tuning**: 133 false positives out of 19,195 sessions (0.7%). The remaining triggers are likely code snippets and formatted data that look high-entropy.

8. **Search latency**: p99 of 618ms is acceptable but could be improved. Profile to identify if FTS5, usearch, or graph traversal is the bottleneck.

9. **Ingestion rate with embeddings**: 55ms/session (18/s) is GPU-bound. Batch embedding (`mnemon_embed_batch`) should improve throughput significantly.

---

## 8. Benchmark Architecture

The benchmark is two-phase for clean separation of concerns:

```
Phase 1: Generate (local, no network)
  - Start mnemond with embeddings
  - Ingest all unique sessions (dedup by ID)
  - Extract event entities (regex NER + date parser)
  - Query each question against full corpus
  - Optionally run ReAct agent (local LLM or API)
  - Save results to JSONL

Phase 2: Judge (API, rate-limited)
  - Read JSONL results
  - Send to multi-judge panel (majority vote)
  - Pace API calls to avoid rate limits
  - Save judged results to .judged.jsonl
```

### Running the Benchmark

```bash
cd build

# Retrieval only (fastest, no API needed)
MNEMON_LONGMEMEVAL_VARIANT=s MNEMON_LONGMEMEVAL_AGENT=none \
  MNEMON_LONGMEMEVAL_JUDGES=none \
  python3 ../test/test_longmemeval.py generate

# With API agent + judges
MNEMON_LONGMEMEVAL_AGENT=api:claude \
  python3 ../test/test_longmemeval.py generate
MNEMON_LONGMEMEVAL_JUDGES=gpt-4o,gemini,xai MNEMON_LONGMEMEVAL_PACE=2.0 \
  python3 ../test/test_longmemeval.py judge

# Re-read results anytime
python3 ../test/test_longmemeval.py report
```

---

## 9. Metrics Definitions

| Metric | Definition |
|--------|-----------|
| **R@K** (Recall@K) | Did any ground-truth session appear in the top-K search results? Binary per question, averaged across questions. |
| **NDCG@K** | Normalized Discounted Cumulative Gain. Measures ranking quality: correct session at rank 1 scores higher than at rank 5. |
| **Session Recall** | Fraction of ground-truth sessions appearing anywhere in search results. |
| **Judge Accuracy** | Fraction of questions where a majority of independent LLM judges agreed the generated answer was correct. Uses official LongMemEval prompt templates. |

---

*Report generated from LongMemEval benchmark runs on 2026-04-07/08. Dataset: Wu et al., "LongMemEval: Benchmarking Long-Term Memory in AI Assistants", ICLR 2025.*
