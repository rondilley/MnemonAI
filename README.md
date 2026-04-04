# mnemon_ai

A high-performance, local-only memory MCP server written in C.

## What It Does

mnemon_ai gives LLM agents persistent, searchable memory via the [Model Context Protocol](https://modelcontextprotocol.io/) (MCP). It combines a bi-temporal knowledge graph, full-text search, and vector similarity search into a single UNIX daemon with zero cloud dependencies.

Every piece of data stays on your local filesystem. No API keys required for core operation. No Docker. No cloud.

## Key Features

- **Hybrid Search** -- graph traversal + vector similarity + BM25 keyword search, fused via Reciprocal Rank Fusion (RRF)
- **Bi-Temporal Knowledge Graph** -- every fact tracks when it was true (domain time) and when it was recorded (transaction time), with non-destructive updates
- **Local Embeddings** -- llama.cpp in-process with nomic-embed-text-v1.5 (no external API)
- **Hardware-Aware** -- auto-detects and leverages GPU (CUDA), SIMD (AVX2/AVX-512), and NUMA at runtime
- **Memory Lifecycle** -- multi-tier memory (episodic/semantic/procedural), Hebbian importance decay, episodic-to-semantic consolidation
- **Bulk Import** -- ingest email (mbox), Slack exports, transcripts, CSV, JSONL, Markdown, and plain text with rich source metadata
- **28 MCP Tools** -- memory CRUD, entity/graph management, four search modes, temporal queries, maintenance, bulk import, system introspection
- **Security** -- FSM-based secret detection (write + read paths), OWASP MCP Top 10 mitigations, systemd hardening, context budget enforcement
- **Crash-Safe** -- LMDB (ACID, mmap, zero-copy), write-ahead intent log, rebuildable derived indexes

## Architecture

```
                    MCP Clients (Claude Code, Cursor, etc.)
                              |
                        stdio / HTTP
                              |
                    +-------------------+
                    |   mnemon_ai       |
                    |   (C daemon)      |
                    +---+-------+---+---+
                        |       |   |
              +---------+   +---+   +----------+
              |             |                  |
          LMDB          SQLite FTS5        usearch
     (KG + memories)    (BM25 keyword)   (HNSW vector)
              |
        llama.cpp
     (local embeddings)
```

**Storage:** LMDB is the source of truth. FTS5 and usearch are derived indexes, rebuildable at any time.

**Concurrency:** Single writer thread (LMDB's native model) + reader thread pool for parallel hybrid search. Lock-free read path.

## Status

**Pre-implementation.** Architecture designed and peer-reviewed (Gemini 2.5 Pro, OpenAI o3, Grok 3, Mistral Large). Research phase complete. Implementation starts Phase 1.

See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for the full architecture document.

## Planned Implementation Phases

| Phase | Scope | Tools |
|-------|-------|-------|
| 1. Foundation | Core storage, stdio MCP, hybrid search, bulk import, secret detection | 16 of 28 |
| 2. Temporal + Lifecycle | Bi-temporal edges, Hebbian decay, consolidation, threading | 25 of 28 |
| 3. Hardware + HTTP | GPU/SIMD dispatch, HTTP transport, systemd daemon | 28 of 28 |
| 4. Extraction | External LLM entity extraction, conflict detection | Polish |
| 5. Advanced | Admission control, NUMA, quantization, audit log | Stretch |

## Requirements

**Build:**
- C11 compiler (GCC or Clang)
- CMake >= 3.16
- SQLite3 (system package, with FTS5)
- llama.cpp (system install)

**Vendored (included):**
- cJSON (MIT) -- JSON parsing
- LMDB (OpenLDAP) -- primary storage
- usearch (Apache 2.0) -- vector index

**Optional:**
- libcurl -- entity extraction via external LLM
- libsystemd -- sd_notify integration
- libnuma -- NUMA-aware allocation

**Runtime:**
- nomic-embed-text-v1.5 Q8_0 GGUF model (~150MB) for embeddings
- Optional: llama-server or compatible endpoint for entity extraction

## Usage

```bash
# stdio mode (for Claude Code / Cursor)
mnemon_ai --stdio

# systemd daemon mode
mnemon_ai --foreground

# Claude Code MCP config
{
  "mcpServers": {
    "mnemon_ai": {
      "command": "mnemon_ai",
      "args": ["--stdio"]
    }
  }
}
```

## Documentation

- [Architecture](doc/ARCHITECTURE.md) -- full system design (v0.2, peer-reviewed)
- [Research Synthesis](doc/research-synthesis.md) -- landscape analysis and academic survey
- [Landscape Report](doc/memory-mcp-report-v2.md) -- practitioner guide to existing memory MCP servers

## Competitive Position

mnemon_ai is the only memory MCP server that combines:
- C-native single-binary with systemd integration
- All three search modalities (graph + vector + keyword) without external databases
- Local embedding generation (zero API key dependency)
- Bulk import from external sources (email, documents, exports)
- Memory lifecycle management (consolidation, decay, multi-tier)
- Hardware acceleration (GPU/SIMD/NUMA auto-detection)
- OWASP MCP Top 10 security coverage (5/5 relevant categories)

See [Architecture Section 17](doc/ARCHITECTURE.md#17-competitive-comparison) for detailed comparison against all evaluated competitors.

## License

GPL-3.0 -- see [LICENSE](LICENSE)
