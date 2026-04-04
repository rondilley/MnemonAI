# mnemon_ai: Research Synthesis for Memory MCP Design

**Prepared by:** Claude Code research pipeline
**Date:** April 3, 2026
**Status:** Pre-architecture research -- all findings to inform design phase
**Inputs:** Practitioner landscape report (v2), 5 parallel deep-research sweeps

---

## Table of Contents

1. [Project Goal](#1-project-goal)
2. [State of the Art: What Exists Today](#2-state-of-the-art-what-exists-today)
3. [Academic Frontier: Key Papers](#3-academic-frontier-key-papers)
4. [Architecture Taxonomy of Memory Systems](#4-architecture-taxonomy-of-memory-systems)
5. [C Implementation Stack: Available Libraries](#5-c-implementation-stack-available-libraries)
6. [Hardware-Aware Design: Detection and Dispatch](#6-hardware-aware-design-detection-and-dispatch)
7. [Feature Matrix: Best-of-Breed Capabilities](#7-feature-matrix-best-of-breed-capabilities)
8. [Key Design Decisions Ahead](#8-key-design-decisions-ahead)
9. [Sources and References](#9-sources-and-references)

---

## 1. Project Goal

Build an ultra-high-speed, ultra-efficient, ultra-secure **local-only memory MCP server** written in **C** that:

- Runs as a UNIX daemon (threaded, no Docker)
- Uses the MCP protocol (JSON-RPC 2.0 over stdio and/or streamable HTTP)
- Incorporates best-of-breed features from the entire memory MCP landscape
- Is hardware-aware: auto-detects and leverages GPUs, SIMD, AMX, NUMA, NVMe
- Keeps all data local -- zero cloud dependency
- Targets AI-focused workstation hardware

---

## 2. State of the Art: What Exists Today

### 2.1 Production Memory MCP Servers (from landscape report)

| Server | Architecture | Backend | Strengths | Weaknesses |
|--------|-------------|---------|-----------|------------|
| **Zep/Graphiti** | Hybrid temporal KG + vector | Neo4j + embeddings | Only bi-temporal tracking; hybrid retrieval | Heavy (Neo4j); LLM extraction cost |
| **Mem0 Platform** | Managed semantic memory | Cloud | Turnkey; intelligent extraction | Data leaves machine; vendor lock-in |
| **OpenMemory Local** | Local semantic memory | Qdrant + local LLM | Data sovereignty; built-in UI | Docker stack; still needs OpenAI key |
| **Basic Memory** | File/Markdown + FTS | Markdown + SQLite FTS5 | Human-readable; auditable; lightweight | No semantic search; keyword only |
| **Cognee** | Graph RAG pipeline | Configurable (Neo4j/Qdrant) | Deepest doc processing | Heaviest footprint; complex setup |
| **Chroma MCP** | Vector DB with MCP | Chroma (embedded or server) | Best retrieval engine | No memory abstractions |
| **Official Memory** | Knowledge graph | In-memory JSON, JSONL file | Simplest; reference impl | No search; full-graph dump; no scale |

### 2.2 Emerging Projects Worth Noting

| Project | Differentiator |
|---------|---------------|
| **Shodh Memory** | Hebbian associative memory; single 30MB binary; works offline on Pi/Jetson; 37 MCP tools |
| **neural-memory** | Neuron/synapse/fiber associations (not key-value); SQLite-backed |
| **Codebase Memory MCP** | Single static binary; 66 languages; sub-ms queries |
| **Hindsight + Ollama** | Fully local KG memory with local embeddings |
| **A-MEM** | Zettelkasten-inspired interconnected knowledge networks (NeurIPS 2025) |
| **EverMemOS** | Engram-inspired lifecycle: episodic trace -> semantic consolidation -> reconstructive recall |
| **MemOS (MemTensor)** | Memory as OS resource; MemCube units; 159% improvement in temporal reasoning over OpenAI |

### 2.3 Competitive Gaps -- What Nobody Does Well

1. **No C-native implementation** -- everything is Python, TypeScript, or Rust
2. **No hardware awareness** -- none detect or leverage GPU/SIMD/AMX/NUMA
3. **No single-binary simplicity with advanced features** -- you get simple+weak or complex+heavy
4. **No hybrid retrieval without external DBs** -- graph+vector+keyword always needs Neo4j/Qdrant/etc.
5. **No embedded temporal KG** -- Graphiti's temporal model requires Neo4j
6. **No zero-dependency local embeddings in a memory server** -- all need external LLM APIs or separate services
7. **No UNIX daemon model** -- everything is either a CLI subprocess or a Docker stack

---

## 3. Academic Frontier: Key Papers

### 3.1 Foundational Surveys

| Paper | Key Contribution |
|-------|-----------------|
| **"Memory in the Age of AI Agents"** (arXiv:2512.13564, Dec 2025) | Taxonomy: factual/experiential/working memory; dynamics: formation/evolution/retrieval |
| **"Memory for Autonomous LLM Agents"** (arXiv:2603.07670, Mar 2026) | Write-manage-read loop; five mechanism families; three-dimensional taxonomy |
| **"Multi-Agent Memory from CompArch Perspective"** (arXiv:2603.10062, Mar 2026) | Memory hierarchy (I/O, cache, registers) applied to agent memory |
| **ICLR 2026 MemAgents Workshop** (April 27, 2026) | First major workshop on memory for LLM agents |

### 3.2 Architectural Innovations

| Paper/System | Innovation | Relevance to mnemon_ai |
|-------------|-----------|----------------------|
| **Graphiti/Zep** (arXiv:2501.13956) | Bi-temporal KG with edge invalidation | Core temporal model to replicate |
| **Graph-Native Cognitive Memory** (arXiv:2603.17244) | Formal belief revision for versioned memory | Extends Graphiti with formal semantics |
| **A-MEM** (arXiv:2502.12110, NeurIPS 2025) | Zettelkasten-style interconnected notes with dynamic indexing | Memory linking/association model |
| **EverMemOS** (arXiv:2601.02163) | Engram lifecycle: episodic->semantic consolidation | Consolidation pipeline design |
| **MemOS** (arXiv:2507.03724) | MemCube units (content+metadata); memory as OS resource | Data model inspiration |
| **AgeMem** (arXiv:2601.01885) | Memory ops as tool-based actions; learned policy for store/retrieve/discard | MCP tool design |
| **SAGE** (Ebbinghaus forgetting curve) | Dynamic importance scoring with temporal decay | Decay/pruning strategy |
| **A-MAC** (ICLR 2026) | Learned admission control for long-term memory | What to remember |
| **AutoSchemaKG** (arXiv:2505.23628) | Autonomous KG schema induction from text | Entity extraction without predefined schema |
| **A-RAG** (arXiv:2602.03442) | Hierarchical retrieval: keyword + semantic + chunk read | Hybrid retrieval tool design |

### 3.3 Key Architectural Patterns Emerging

1. **Multi-tier memory** (dominant paradigm): Working (in-context) -> Session (medium-term) -> Long-term (persistent)
2. **Memory lifecycle**: Formation -> Consolidation -> Retrieval -> Decay/Pruning
3. **Temporal tracking**: Every fact has valid_from/valid_to + created_at/expired_at (bi-temporal)
4. **Hebbian strengthening**: Memories strengthen with use, decay without it
5. **Hybrid retrieval**: Graph traversal + vector similarity + keyword search, fused via RRF
6. **Admission control**: Not everything should be remembered -- learned or rule-based filtering
7. **Conflict resolution**: New facts invalidate old facts (non-lossy) rather than overwriting

---

## 4. Architecture Taxonomy of Memory Systems

### 4.1 Memory Types (from survey literature)

| Type | Description | Storage Pattern |
|------|-------------|-----------------|
| **Episodic** | Raw experiences/conversations with timestamps | Append-only log -> consolidation |
| **Semantic** | Extracted facts, entities, relationships | Knowledge graph |
| **Procedural** | How to do things; learned strategies | Indexed patterns/templates |
| **Working** | Currently relevant context; active reasoning | In-memory, size-limited |

### 4.2 Memory Operations (MCP Tool Surface)

From AgeMem and the survey literature, the core operations are:

| Operation | Description |
|-----------|-------------|
| **store** | Ingest new information (episodic trace) |
| **retrieve** | Find relevant memories for a query |
| **update** | Modify existing memory (with temporal tracking) |
| **consolidate** | Merge/summarize related memories |
| **forget/prune** | Remove low-value memories based on decay |
| **relate** | Create/strengthen associations between memories |
| **search** | Multi-modal search (keyword + semantic + graph) |
| **introspect** | Query memory statistics, health, conflicts |

---

## 5. C Implementation Stack: Available Libraries

### 5.1 MCP Protocol Layer

| Option | Language | Transports | Maturity | Notes |
|--------|----------|-----------|----------|-------|
| **SupaMCP** | C | stdio, TCP, HTTP, Streamable HTTP, MQTT | Active dev | Broadest transport coverage in C |
| **mcpc** | C (C23/C11) | stdio only | Early | Minimal pure-C SDK |
| **Build from scratch** | C | stdio (+HTTP via libmicrohttpd) | N/A | ~500-1000 lines for stdio; cJSON + JSON-RPC 2.0 dispatch |
| **gopher-mcp** | C++ with C FFI | stdio, HTTP/SSE, WebSocket, TCP | Production-oriented | C API via opaque handles |

**JSON libraries:** cJSON (MIT, most popular), jansson, mjson (embedded-friendly with JSON-RPC engine)

### 5.2 Storage Layer

| Component | Recommendation | Rationale |
|-----------|---------------|-----------|
| **Primary KV/Graph store** | **LMDB** | Zero-copy reads via mmap; MVCC (readers never block); crash-safe ACID; single-writer/multi-reader fits daemon; OpenLDAP license (permissive) |
| **Full-text search** | **SQLite FTS5** | BM25 built-in; contentless mode avoids duplication; zero deployment cost; production-grade |
| **Vector index** | **usearch** (primary) or **FAISS** (large scale) | usearch: C99 single-header, Apache 2.0, SIMD-optimized, mmap support. FAISS: GPU-accelerated, C API, handles billion-scale |
| **Alternative vector** | **hnswlib** | C++ header-only, Apache 2.0, simpler than FAISS, excellent for <10M vectors |

### 5.3 Embedding Generation

| Option | Language | GPU Support | Model Compatibility | API Stability |
|--------|----------|-------------|-------------------|---------------|
| **llama.cpp** | C/C++ (C API) | CUDA, ROCm, Metal, Vulkan | GGUF: nomic-embed, BGE, MiniLM, GTE, mxbai, e5 | Good (pin releases) |
| **ONNX Runtime** | C/C++ (C API) | CUDA, TensorRT, ROCm | Any ONNX-exported model | Excellent (versioned vtable) |

**Recommended model:** nomic-embed-text-v1.5 at Q8_0 or F16 via llama.cpp
- 137M params, 768-dim embeddings
- Q8_0: ~150MB, <1% quality loss
- 2,000-5,000 embeddings/sec on RTX 4090 in batch mode

### 5.4 Graph Structures

| Approach | Use Case | Performance |
|----------|----------|-------------|
| **LMDB with composite keys** | Persistent graph storage | Millions of point reads/sec; zero-copy |
| **CSR + delta buffer** | Hot-path traversal | 2-10x faster than adjacency lists for BFS |
| **SuiteSparse:GraphBLAS** | Heavy graph analytics | Sparse matrix algebra; billions of edges |
| **Interval tree** | Temporal queries (valid_from/valid_to) | O(log n + k) stabbing queries |

### 5.5 Hybrid Retrieval Fusion

**Reciprocal Rank Fusion (RRF)** -- the standard approach:
```
RRF_score(d) = sum over rankers r: 1 / (k + rank_r(d))    [k=60]
```
- Parameter-free, no score normalization needed
- Used by Elasticsearch, Weaviate, and others
- Trivial to implement in C

---

## 6. Hardware-Aware Design: Detection and Dispatch

### 6.1 Detection APIs

| Hardware | Library/API | Method |
|----------|-----------|--------|
| **NVIDIA GPU** | NVML (`libnvidia-ml.so.1`) | `dlopen()` at runtime; query model, VRAM, compute capability |
| **AMD GPU** | ROCm SMI / AMD SMI | `dlopen()` at runtime; query VRAM, device model |
| **CPU SIMD** | CPUID + XCR0, or Google `cpu_features` | Detect AVX2, AVX-512, AMX at startup |
| **RAM/NUMA** | `sysinfo()` + `/proc/meminfo` + `libnuma` | Total/available RAM, NUMA topology |
| **Storage** | sysfs (`/sys/block/nvme*/queue/rotational`) | NVMe detection |
| **Topology** | `hwloc` (optional) | Full cache/NUMA/PCI topology |

### 6.2 Runtime Strategy Selection

```
Embedding inference:
  GPU (compute >= 7.0, sufficient VRAM) -> AMX -> AVX-512 VNNI -> AVX-512 -> AVX2 -> scalar

Vector similarity search:
  GPU (large index, VRAM available) -> AVX-512 -> AVX2 -> scalar

Memory allocation:
  NUMA-aware placement -> huge pages (if available) -> standard mmap
```

### 6.3 Implementation Pattern

- Compile SIMD code paths in separate `.c` files with `-mavx2` / `-mavx512f` flags
- Dispatch at runtime via function pointers populated at startup
- GPU libraries loaded via `dlopen()` -- daemon compiles and runs without GPU drivers
- Hardware capability struct populated once at daemon init, immutable thereafter

---

## 7. Feature Matrix: Best-of-Breed Capabilities

These are the features we should aim to incorporate, drawn from the best implementations:

### 7.1 Core Memory Features

| Feature | Source/Inspiration | Priority |
|---------|-------------------|----------|
| Bi-temporal knowledge graph | Graphiti/Zep | Must have |
| Multi-tier memory (working/session/long-term) | Shodh, Letta, EverMemOS | Must have |
| Hybrid retrieval (graph + vector + keyword) | Graphiti, A-RAG | Must have |
| RRF rank fusion | Elasticsearch, academic standard | Must have |
| Local embedding generation | llama.cpp + nomic-embed | Must have |
| Entity extraction from conversation | Graphiti, Mem0, AutoSchemaKG | Must have |
| Relationship tracking with types | Graphiti, Official Memory | Must have |
| Memory consolidation | EverMemOS, MemOS | Should have |
| Hebbian strengthening/decay | Shodh, SAGE | Should have |
| Importance scoring | SAGE, A-MAC | Should have |
| Conflict detection and resolution | Mem0g, Graphiti | Should have |
| Admission control | A-MAC | Nice to have |
| Formal belief revision | arXiv:2603.17244 | Nice to have |

### 7.2 System Features

| Feature | Rationale | Priority |
|---------|-----------|----------|
| UNIX daemon (systemd-compatible) | Production deployment | Must have |
| stdio MCP transport | Claude Code, Cursor compatibility | Must have |
| Streamable HTTP transport | Multi-client access | Should have |
| Hardware auto-detection | Leverage available acceleration | Must have |
| GPU-accelerated embeddings | Speed on AI workstations | Must have |
| SIMD-optimized vector search | Fast retrieval on any hardware | Must have |
| mmap-based storage (LMDB) | Zero-copy, OS-managed caching | Must have |
| Thread-safe concurrent access | Multi-client daemon | Must have |
| Crash-safe persistence (ACID) | Data integrity | Must have |
| Memory-mapped vector index | Large indexes without proportional RAM | Should have |
| NUMA-aware allocation | Multi-socket workstations | Nice to have |
| Huge page support | Reduced TLB pressure for large stores | Nice to have |

### 7.3 Security Features

| Feature | Rationale | Priority |
|---------|-----------|----------|
| Zero network exposure (stdio default) | Attack surface minimization | Must have |
| No cloud dependency | Data sovereignty | Must have |
| No secrets in memory store | OWASP MCP01 | Must have |
| Context budget enforcement (Top-K) | OWASP MCP03 prevention | Must have |
| Input sanitization | OWASP command injection | Must have |
| Encrypted at-rest storage | Data protection | Should have |
| Memory access audit log | Compliance/debugging | Should have |
| Token budget limits on retrieval | Prevent context over-sharing | Must have |

### 7.4 MCP Tool Surface (Proposed)

Drawing from AgeMem, Shodh (37 tools), Graphiti, and the survey literature:

| Tool Category | Tools |
|--------------|-------|
| **Memory CRUD** | `store_memory`, `retrieve_memories`, `update_memory`, `delete_memory` |
| **Entity/Graph** | `create_entity`, `add_observation`, `create_relation`, `search_entities`, `get_entity_graph` |
| **Search** | `search_semantic`, `search_keyword`, `search_hybrid`, `search_temporal` |
| **Temporal** | `get_history`, `get_state_at_time`, `get_changes_since` |
| **Maintenance** | `consolidate_memories`, `prune_stale`, `get_memory_stats`, `get_conflicts` |
| **System** | `get_hardware_info`, `get_index_stats`, `health_check` |

---

## 8. Key Design Decisions Ahead

These are the architectural questions we need to resolve in the design phase:

### 8.1 MCP Protocol Layer
- **Build vs. adopt?** SupaMCP provides broad transport support but is early-stage. Building stdio from scratch on cJSON is ~500 lines and gives full control. Streamable HTTP adds significant complexity.
- **Transport priority?** stdio is required for Claude Code/Cursor. Streamable HTTP enables multi-client access from a daemon.

### 8.2 Entity Extraction
- **LLM-powered vs. rule-based?** Graphiti/Mem0 use LLM calls for extraction (high quality, but adds latency and cost). Rule-based NER is faster but lower quality. Hybrid approach possible.
- **Where does the LLM run?** The daemon could call an external LLM (via llama.cpp server or Ollama), or embed a small model directly for extraction.

### 8.3 Embedding Strategy
- **Embedded vs. external?** Embedding llama.cpp directly gives lowest latency but couples the daemon to the inference runtime. External (HTTP call to llama-server) is cleaner but adds latency.
- **Model selection:** nomic-embed-text-v1.5 (768-dim, Q8_0, ~150MB) is the leading candidate. Matryoshka support allows dimension reduction (768->512->256) for storage/speed tradeoffs.

### 8.4 Storage Architecture
- **LMDB + SQLite FTS5 + usearch/FAISS** is the emerging recommendation. Key question: how to coordinate transactions across three storage engines?
- **Graph encoding in LMDB:** Composite key scheme `(source_id, edge_type, target_id)` with `MDB_DUPSORT` for range scans. Reverse index for incoming edges.

### 8.5 Memory Lifecycle
- **How aggressive should consolidation be?** EverMemOS consolidates episodic traces into semantic scenes. Shodh uses Hebbian decay. Both have merit.
- **Who triggers consolidation?** Background thread on timer? On memory count threshold? On explicit MCP tool call? All three?

### 8.6 Concurrency Model
- **Single-writer / multi-reader** (LMDB's native model) is the simplest. Writers queue through a single thread; readers are lock-free.
- **Thread pool for search?** Hybrid retrieval (graph + vector + keyword) can parallelize across threads with RRF fusion at the end.

### 8.7 Security Model
- **Encryption at rest:** LMDB does not natively encrypt. Options: encrypted filesystem, custom encryption layer, or SQLCipher for the FTS5 component.
- **Secret detection:** Scan incoming memories for patterns (API keys, passwords) before storage.

---

## 9. Sources and References

### 9.1 Landscape Report
- memory-mcp-report-v2.md (in doc/ folder)

### 9.2 Key Papers
- "Memory in the Age of AI Agents" -- arXiv:2512.13564
- "Memory for Autonomous LLM Agents" -- arXiv:2603.07670
- "Multi-Agent Memory from CompArch Perspective" -- arXiv:2603.10062
- "Zep: A Temporal Knowledge Graph Architecture" -- arXiv:2501.13956
- "Graph-Native Cognitive Memory" -- arXiv:2603.17244
- "A-MEM: Agentic Memory for LLM Agents" -- arXiv:2502.12110 (NeurIPS 2025)
- "EverMemOS" -- arXiv:2601.02163
- "MemOS" -- arXiv:2507.03724
- "AgeMem" -- arXiv:2601.01885
- "SAGE" -- ScienceDirect (Ebbinghaus forgetting curve)
- "A-MAC" -- arXiv:2603.04549 (ICLR 2026 MemAgents Workshop)
- "AutoSchemaKG" -- arXiv:2505.23628
- "A-RAG" -- arXiv:2602.03442
- "RAG Comprehensive Survey" -- arXiv:2506.00054
- "Reciprocal Rank Fusion" -- Cormack et al., SIGIR 2009
- "Timeline Index" -- Kaufmann et al., SIGMOD 2013

### 9.3 C Libraries Identified
- **MCP:** SupaMCP (github.com/nxtreaming/SupaMCP), mcpc (github.com/micl2e2/mcpc), gopher-mcp (github.com/GopherSecurity/gopher-mcp)
- **JSON:** cJSON, jansson, mjson (github.com/cesanta/mjson)
- **JSON-RPC:** mjson JSON-RPC engine, jsonrpc-c, cjson-rpc
- **Storage:** LMDB (github.com/LMDB/lmdb), SQLite FTS5 (sqlite.org)
- **Vector search:** usearch (github.com/unum-cloud/usearch), FAISS (github.com/facebookresearch/faiss), hnswlib (github.com/nmslib/hnswlib)
- **Embeddings:** llama.cpp (github.com/ggerganov/llama.cpp), ONNX Runtime (github.com/microsoft/onnxruntime)
- **Graph:** SuiteSparse:GraphBLAS (github.com/DrTimothyAldenDavis/GraphBLAS), liburcu (liburcu.org)
- **Hardware:** NVML, Google cpu_features (github.com/google/cpu_features), hwloc, libnuma
- **HTTP:** libmicrohttpd, mongoose

### 9.4 Existing MCP Projects Studied
- Official Memory Server -- github.com/modelcontextprotocol/servers
- Zep/Graphiti -- github.com/getzep/graphiti
- Mem0 -- github.com/mem0ai/mem0
- Basic Memory -- github.com/basicmachines-co/basic-memory
- Cognee -- github.com/topoteretes/cognee
- Chroma MCP -- github.com/chroma-core/chroma-mcp
- Shodh Memory -- github.com/varun29ankuS/shodh-memory
- A-MEM -- github.com/agiresearch/A-mem
- EverMemOS -- github.com/EverMind-AI/EverMemOS
- MemOS -- github.com/MemTensor/MemOS
- Codebase Memory MCP -- github.com/DeusData/codebase-memory-mcp
- Hindsight -- hindsight.vectorize.io
- RuVector -- github.com/ruvnet/ruvector/

### 9.5 Security References
- OWASP MCP Top 10 -- owasp.org/www-project-mcp-top-10/
- Equixly MCP audit -- dev.to/mistaike_ai
- Invariant Labs MCP-Scan -- github.com/invariantlabs-ai/mcp-scan
