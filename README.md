# mnemon_ai

A high-performance, local-only memory MCP server written in C11.

## What It Does

mnemon_ai gives LLM agents persistent, searchable memory via the [Model Context Protocol](https://modelcontextprotocol.io/) (MCP). It combines a bi-temporal knowledge graph, full-text search, and vector similarity search into a single UNIX daemon with zero cloud dependencies.

Every piece of data stays on your local filesystem. No API keys required for core operation. No Docker. No cloud.

## Key Features

- **Hybrid Search** -- graph traversal + vector similarity + BM25 keyword search, fused via Reciprocal Rank Fusion (RRF)
- **Bi-Temporal Knowledge Graph** -- every fact tracks when it was true (domain time) and when it was recorded (transaction time), with non-destructive updates
- **Local Embeddings** -- llama.cpp in-process with nomic-embed-text-v1.5 (no external API)
- **Hardware-Aware** -- auto-detects and leverages GPU (CUDA via NVML dlopen), SIMD (AVX2/AVX-512), and NUMA at runtime
- **Memory Lifecycle** -- multi-tier memory (episodic/semantic/procedural), Hebbian importance decay, episodic-to-semantic consolidation
- **Bulk Import** -- ingest email (mbox), CSV, JSONL, Markdown, and plain text with rich source metadata and configurable chunking
- **28 MCP Tools** -- memory CRUD, entity/graph management, four search modes, temporal queries, bulk import (JSONL/CSV/mbox/text), maintenance, hardware introspection, consolidation, pruning
- **Security** -- FSM-based secret detection, content size limits, FTS5 query sanitization, import path validation, OWASP MCP Top 10 mitigations
- **Crash-Safe** -- LMDB (ACID, mmap, zero-copy), write-ahead intent log, rebuildable derived indexes (FTS5 + usearch)

## Architecture

```
                    MCP Clients (Claude Code, Cursor, etc.)
                              |
                        stdio / HTTP
                              |
                    +-------------------+
                    |   mnemon_ai       |
                    |   (C11 daemon)    |
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

**Storage:** LMDB is the source of truth. FTS5 and usearch are derived indexes, rebuildable at any time via `rebuild_indexes`.

**Logging:** Foreground mode logs status to stdout and errors to stderr. Daemon mode logs to syslog. Stdio MCP mode logs to stderr only (stdout is the JSON-RPC channel).

## Status

**All 5 phases complete.** 28 MCP tools, hybrid search (graph+vector+keyword with RRF fusion), bulk import, secret detection, hardware detection (AMD/NVIDIA GPU, AMD NPU, SIMD), auto-model download, entity extraction, admission control, and audit logging.

- 10,000+ lines of C source across 28 modules
- 253 tests (156 C unit + 97 Python end-to-end)
- 28 MCP tools, all tested at both API and wire-protocol levels
- Performance: 417 ops/sec store, 10us retrieve, 530us search at 1K memories
- Zero segfaults, zero warnings (`-Werror=implicit-function-declaration`)

See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for the full architecture document.

## Build

```bash
# Prerequisites: C11 compiler, CMake >= 3.16, llama.cpp installed
# llama.cpp: git clone, cmake, make, make install (to ~/.local or /usr/local)

mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=$HOME/.local
make -j$(nproc)
ctest                    # run all tests
```

If llama.cpp is installed to a non-standard prefix, set `CMAKE_PREFIX_PATH` or `LLAMA_ROOT`.

## Usage

```bash
# stdio mode (for Claude Code / Cursor MCP clients)
mnemon_ai --stdio

# foreground mode (for systemd Type=notify or manual operation)
mnemon_ai --foreground

# daemon mode (traditional UNIX double-fork, logs to syslog)
mnemon_ai --daemon

# validate configuration
mnemon_ai --check-config

# rebuild derived indexes from LMDB source of truth
mnemon_ai --rebuild-indexes
```

### Client Configuration

mnemon_ai uses the standard MCP stdio transport (JSON-RPC 2.0 over stdin/stdout). Any MCP-compatible client can connect by launching the binary with `--stdio`.

#### Claude Code (CLI)

Add to `~/.claude/claude_desktop_config.json` or your project's `.claude/settings.json`:

```json
{
  "mcpServers": {
    "mnemon_ai": {
      "command": "mnemon_ai",
      "args": ["--stdio"]
    }
  }
}
```

#### Claude Desktop / claude.ai

Settings > Developer > MCP Servers > Add:

```json
{
  "mcpServers": {
    "mnemon_ai": {
      "command": "/usr/local/bin/mnemon_ai",
      "args": ["--stdio", "--config", "/home/you/.config/mnemon_ai/mnemon_ai.conf"]
    }
  }
}
```

Use the full path to the binary since the desktop app doesn't inherit your shell PATH.

#### Cursor

Settings > MCP > Add Server:

```json
{
  "mcpServers": {
    "mnemon_ai": {
      "command": "mnemon_ai",
      "args": ["--stdio"]
    }
  }
}
```

#### Gemini CLI

```bash
# In ~/.gemini/settings.json or project config:
{
  "mcpServers": {
    "mnemon_ai": {
      "command": "mnemon_ai",
      "args": ["--stdio"]
    }
  }
}
```

#### OpenAI Codex CLI

```bash
# In ~/.codex/config.json:
{
  "mcpServers": {
    "mnemon_ai": {
      "command": "mnemon_ai",
      "args": ["--stdio"]
    }
  }
}
```

#### ChatGPT (via MCP bridge)

ChatGPT does not natively support MCP. Use an MCP-to-function-calling bridge:

```bash
# Run mnemon_ai as a stdio server, pipe through an MCP bridge
# See: https://github.com/anthropics/anthropic-cookbook for bridge examples
mnemon_ai --stdio
```

#### Any MCP Client

The stdio protocol is universal. Launch the process and exchange newline-delimited JSON:

```bash
# Manual test
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}' | mnemon_ai --stdio
```

## Configuration

INI format. All settings have defaults. Config search path:

1. `--config <path>` (command line)
2. `$XDG_CONFIG_HOME/mnemon_ai/mnemon_ai.conf`
3. `~/.config/mnemon_ai/mnemon_ai.conf`
4. `/etc/mnemon_ai/mnemon_ai.conf`

See [etc/mnemon_ai.conf.example](etc/mnemon_ai.conf.example) for all options.

## Implementation Phases

| Phase | Scope | Status |
|-------|-------|--------|
| 1. Foundation | Core storage, stdio MCP, hybrid search, import, secret detection | **Complete** (22 tools) |
| 2. Temporal + Lifecycle | Bi-temporal queries, Hebbian decay, consolidation, writer queue | **Complete** (26 tools) |
| 3. Hardware + Daemon | GPU/NPU/SIMD detection, daemon polish, SIGHUP/SIGUSR1 | **Complete** (28 tools) |
| 4. Extraction | External LLM entity extraction via libcurl | **Complete** (requires `-DENABLE_CURL=ON`) |
| 5. Advanced | Admission control, audit log, auto-model download | **Complete** |
| Remaining | HTTP transport (libmicrohttpd), parallel search, GPU embedding | In progress |

## Dependencies

**Build (required):**
- C11 compiler (GCC 10+ or Clang 11+)
- CMake >= 3.16
- llama.cpp (system install -- provides embedding inference)

**Vendored (included in third_party/):**
- cJSON 1.7.18 (MIT) -- JSON parsing
- LMDB 0.9.x (OpenLDAP Public License) -- primary key-value storage
- usearch 2.x (Apache 2.0) -- HNSW vector index
- SQLite 3.45.1 amalgamation (public domain) -- FTS5 full-text search

**Optional:**
- libcurl -- entity extraction via external LLM (Phase 4)
- libsystemd -- sd_notify integration
- libnuma -- NUMA-aware allocation (Phase 5)

**Runtime:**
- nomic-embed-text-v1.5 Q8_0 GGUF model (~150MB) for embeddings
- Optional: llama-server or OpenAI-compatible endpoint for entity extraction

## Documentation

- [Architecture](doc/ARCHITECTURE.md) -- full system design (v0.2, peer-reviewed)
- [Research Synthesis](doc/research-synthesis.md) -- landscape analysis and academic survey
- [Landscape Report](doc/memory-mcp-report-v2.md) -- practitioner guide to existing memory MCP servers
- [Token Efficiency Guide](doc/claude-automation-efficiency-2026.pdf) -- Claude automation and token optimization strategies

## License

GPL-3.0-only -- see [LICENSE](LICENSE)
