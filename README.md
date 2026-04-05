# MnemonAI

A high-performance, local-only memory MCP server written in C11.

## What It Does

MnemonAI gives LLM agents persistent, searchable memory via the [Model Context Protocol](https://modelcontextprotocol.io/) (MCP). The daemon (`mnemond`) combines a bi-temporal knowledge graph, full-text search, and vector similarity search into a single process with zero cloud dependencies.

Every piece of data stays on your local filesystem. No API keys required for core operation. No Docker. No cloud.

## Key Features

- **Hybrid Search** -- graph traversal + vector similarity + BM25 keyword search, fused via Reciprocal Rank Fusion (RRF). Three rankers run in parallel via persistent reader pool or ad-hoc pthreads.
- **Bi-Temporal Knowledge Graph** -- every fact tracks when it was true (domain time) and when it was recorded (transaction time), with non-destructive updates
- **Local Embeddings** -- llama.cpp in-process with nomic-embed-text-v1.5 (no external API)
- **Hardware-Aware** -- auto-detects AMD/NVIDIA/Intel GPU, AMD XDNA NPU, SIMD (AVX2/AVX-512), and NUMA at runtime
- **Network MCP Server** -- Streamable HTTP transport (MCP 2025-03-26 spec) with TLS, Bearer auth, session management, SSE streaming, and Origin validation. Run one instance on a server, connect from any machine.
- **Memory Lifecycle** -- multi-tier memory (episodic/semantic/procedural), Hebbian importance decay, episodic-to-semantic consolidation with automatic entity deduplication and merge
- **Bulk Import** -- ingest email (mbox), CSV, JSONL, Markdown, and plain text with rich source metadata, configurable chunking, recursive directory traversal, and background job tracking
- **32 MCP Tools** -- 28 functional tools (memory CRUD, entity/graph, four search modes, temporal queries, bulk import, maintenance, hardware introspection) + 4 honeypot decoy tools
- **Security** -- FSM-based secret detection, prompt injection scanner (integrated into store path, score >= 7.0 blocks storage), content size limits, auth brute-force detection (per-IP rate limiting wired into HTTP layer), canary records, decoy admin tools, enumeration detection, credential query detection, OWASP MCP Top 10 mitigations
- **Crash-Safe** -- LMDB (ACID, mmap, zero-copy), write-ahead intent log, rebuildable derived indexes (FTS5 + usearch)

## Architecture

```mermaid
graph TB
    subgraph Clients
        CC[Claude Code / Cursor<br/>Any MCP Client]
    end

    subgraph "MnemonAI (mnemond)"
        D[mnemond]
    end

    subgraph Storage
        LMDB[LMDB<br/>KG + memories]
        FTS[SQLite FTS5<br/>BM25 keyword]
        US[usearch<br/>HNSW vector]
    end

    subgraph Embedding
        LLAMA[llama.cpp<br/>local embeddings]
    end

    CC -- "stdio or HTTP" --> D
    D --> LMDB
    D --> FTS
    D --> US
    D --> LLAMA
```

**Storage:** LMDB is the source of truth. FTS5 and usearch are derived indexes, rebuildable at any time via `rebuild_indexes`.

**Logging:** Foreground mode logs status to stdout and errors to stderr. Daemon mode logs to syslog. Stdio MCP mode logs to stderr only (stdout is the JSON-RPC channel).

## Status

**All phases complete. No known gaps.** Dual transport (stdio + Streamable HTTP with SSE), 32 MCP tools (28 functional + 4 honeypot), parallel hybrid search with RRF fusion, network server with TLS/auth/sessions, hardware detection (AMD/NVIDIA GPU, AMD NPU, SIMD), auto-model download, entity extraction, prompt injection detection integrated into store path, auth brute-force detection, admission control, audit logging, persistent reader pool, recursive imports with background job tracking, and entity merging in consolidation.

- 12,500+ lines of C source across 30 modules
- 343 tests (196 C unit + 105 Python stdio + 42 Python HTTP)
- 32 MCP tools, all tested at API, stdio, and HTTP levels
- GPU-accelerated embedding via ROCm HIP (AMD) or CUDA (NVIDIA)
- AVX-512/AVX2 SIMD for vector distance computation and L2 normalization
- Persistent reader thread pool for parallel search dispatch
- Performance: 417 ops/sec store, 10us retrieve, 530us search at 1K memories
- Zero segfaults, zero warnings (`-Werror=implicit-function-declaration`)

See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for the full architecture document.

## Getting Started

### Step 1: Install Build Dependencies

```bash
# Ubuntu / Debian
sudo apt-get install build-essential cmake git

# Fedora / RHEL
sudo dnf install gcc gcc-c++ cmake git
```

### Step 2: Build and Install llama.cpp

MnemonAI uses llama.cpp for local embedding generation. Build it from source:

```bash
git clone https://github.com/ggerganov/llama.cpp
cd llama.cpp
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/.local
make -j$(nproc)
make install
cd ../..
```

### Step 3: Build MnemonAI

```bash
git clone https://github.com/rdilley/MnemonAI.git
cd MnemonAI
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=$HOME/.local
make -j$(nproc)
```

CMake will print a feature summary showing what was detected. If libcurl is found, entity extraction is enabled. If not, it prints the install command.

### Step 4: Run Tests

```bash
ctest --output-on-failure     # 156 C unit tests
LD_LIBRARY_PATH=$HOME/.local/lib python3 ../test/test_mcp_client.py   # 97 MCP tests
```

### Step 5: Install

```bash
sudo make install             # installs to /usr/local/bin/mnemond
```

Or run from the build directory without installing:

```bash
LD_LIBRARY_PATH=$HOME/.local/lib ./mnemond --version
```

### Step 6: First Run

On first run, mnemond auto-detects your hardware and downloads the recommended embedding model (~140MB from HuggingFace):

```bash
mnemond --stdio               # starts and downloads model if needed
```

To skip the model download (run without embeddings):

```bash
echo "model_path = none" >> ~/.config/mnemond/mnemond.conf
```

### Step 7: Verify

```bash
mnemond --check-config        # validate configuration
mnemond --version             # show version + git commit
```

---

## Deployment Options

### Option A: Local (stdio) -- Single Machine

The MCP client launches `mnemond` as a child process. Communication is over stdin/stdout. No network, no auth, no configuration needed.

```mermaid
graph LR
    Client["AI Tool<br/>(Claude Code, Cursor, etc.)"] -- "stdin/stdout<br/>JSON-RPC 2.0" --> D["mnemond<br/>(child process)"]
    D --> Storage["LMDB + FTS5 + usearch"]
```

### Option B: Network Server (HTTP) -- Multiple Machines, Shared Memory

Run `mnemond` as a daemon on one machine (e.g., a Framework desktop with GPU/NPU). AI tools on any machine -- Windows, macOS, Linux -- connect to the shared memory server over HTTP. All agents and tools share the same knowledge base.

```mermaid
graph TB
    subgraph "Framework Desktop (Linux)"
        D["mnemond<br/>(HTTP on port 3847)"]
        Storage["LMDB + FTS5 + usearch"]
        D --> Storage
    end

    subgraph "Windows 11 Workstation"
        CC[Claude Code]
        CX[Codex CLI]
    end

    subgraph "macOS Laptop"
        CR[Cursor]
        GM[Gemini CLI]
    end

    subgraph "Other Systems"
        CD[Claude Desktop]
        HM[Hermes]
    end

    CC -- "HTTP POST /mcp" --> D
    CX -- "HTTP POST /mcp" --> D
    CR -- "HTTP POST /mcp" --> D
    GM -- "HTTP POST /mcp" --> D
    CD -- "HTTP POST /mcp" --> D
    HM -- "HTTP POST /mcp" --> D
```

#### Setting Up the Network Server

1. **Install mnemond** on the server (Steps 1-5 above)

2. **Configure for network access:**

```bash
mkdir -p ~/.config/mnemond
cat > ~/.config/mnemond/mnemond.conf << 'EOF'
[general]
data_dir = ~/.local/share/mnemond
log_level = info

[lmdb]
map_size_gb = 10

[embedding]
# Leave empty for auto-download on first run
model_path =

[http]
enabled = true
bind = 0.0.0.0
port = 3847
auth_token = YOUR-SECRET-TOKEN-HERE
EOF
```

The `auth_token` is required when binding to non-localhost. Generate one with: `openssl rand -hex 32`

3. **Start the server:**

```bash
# Foreground (see logs on terminal)
mnemond --foreground

# Or install as a systemd service
sudo cp etc/mnemond.service /etc/systemd/system/
sudo systemctl enable --now mnemond
```

4. **Verify the server is running:**

```bash
curl -X POST http://localhost:3847/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR-SECRET-TOKEN-HERE" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'
```

You should see a JSON response with `protocolVersion` and `serverInfo`.

---

## Client Configuration

### Local Mode (stdio)

For AI tools running on the same machine as mnemond. The tool launches mnemond as a child process.

#### Claude Code

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "mnemond",
      "args": ["--stdio"]
    }
  }
}
```

Save to `~/.claude/settings.json` or project `.claude/settings.json`.

#### Claude Desktop

Settings > Developer > MCP Servers > Add:

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "/usr/local/bin/mnemond",
      "args": ["--stdio"]
    }
  }
}
```

Use the full path -- desktop apps don't inherit shell PATH.

#### Cursor

Settings > MCP > Add Server:

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "mnemond",
      "args": ["--stdio"]
    }
  }
}
```

#### Gemini CLI

In `~/.gemini/settings.json`:

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "mnemond",
      "args": ["--stdio"]
    }
  }
}
```

#### OpenAI Codex CLI

In `~/.codex/config.json`:

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "mnemond",
      "args": ["--stdio"]
    }
  }
}
```

### Network Mode (HTTP)

For AI tools running on a different machine from the mnemond server. The tool connects to the server's HTTP endpoint.

MCP clients that support HTTP/SSE transport can connect directly using the server URL:

```
http://framework-desktop:3847/mcp
```

For MCP clients that only support stdio transport, use a local wrapper script that bridges stdio to HTTP:

**Linux/macOS -- `~/bin/mnemond-remote`:**

```bash
#!/bin/bash
# Bridges stdio to mnemond HTTP server
# Reads JSON-RPC from stdin, POSTs to server, writes response to stdout
SERVER="http://framework-desktop:3847/mcp"
TOKEN="YOUR-SECRET-TOKEN-HERE"
while IFS= read -r line; do
    curl -s -X POST "$SERVER" \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer $TOKEN" \
        -d "$line"
    echo
done
```

```bash
chmod +x ~/bin/mnemond-remote
```

**Windows -- `C:\Users\you\bin\mnemond-remote.bat`:**

```bat
@echo off
REM Requires curl (included in Windows 10+)
set SERVER=http://framework-desktop:3847/mcp
set TOKEN=YOUR-SECRET-TOKEN-HERE
:loop
set /p LINE=
curl -s -X POST %SERVER% -H "Content-Type: application/json" -H "Authorization: Bearer %TOKEN%" -d "%LINE%"
echo.
goto loop
```

Then configure any MCP client to use the wrapper:

#### Claude Code (remote)

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "mnemond-remote",
      "args": []
    }
  }
}
```

#### Claude Desktop on Windows (remote)

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "C:\\Users\\you\\bin\\mnemond-remote.bat",
      "args": []
    }
  }
}
```

#### All Other Tools (Cursor, Gemini, Codex, Hermes -- remote)

Same pattern -- point `command` at `mnemond-remote`:

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "mnemond-remote",
      "args": []
    }
  }
}
```

### Verify Connection

From any configured client, ask your AI tool to run `health_check`. It should return:

```json
{"status": "ok", "version": "v0.1.0 (...)", "storage_ok": true}
```

Or test directly:

```bash
# Local stdio
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"health_check","arguments":{}}}' | mnemond --stdio

# Remote HTTP
curl -s -X POST http://server:3847/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR-TOKEN" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"health_check","arguments":{}}}'
```

---

## Configuration

INI format. All settings have defaults. Config search path:

1. `--config <path>` (command line)
2. `$XDG_CONFIG_HOME/mnemond/mnemond.conf`
3. `~/.config/mnemond/mnemond.conf`
4. `/etc/mnemond/mnemond.conf`

See [etc/mnemond.conf.example](etc/mnemond.conf.example) for all options.

### Key Configuration Options

```ini
[general]
data_dir = ~/.local/share/mnemond    # where LMDB, FTS5, and vector indexes live
log_level = info                      # debug, info, warn, error

[embedding]
model_path = none                     # "none" disables, empty = auto-download
dimensions = 768                      # must match the GGUF model
gpu_layers = 99                       # layers to offload to GPU (99 = all)

[search]
default_top_k = 10                    # results per search
rrf_k = 60                           # RRF fusion constant

[http]
enabled = true                        # enable HTTP transport
bind = 127.0.0.1                      # bind address
port = 3847                           # listen port
auth_token = YOUR-SECRET-TOKEN        # required for non-localhost binding
tls_cert = /path/to/cert.pem          # TLS certificate (optional)
tls_key = /path/to/key.pem            # TLS private key (optional)

[threads]
reader_pool_size = 4                  # persistent reader threads (0 = ad-hoc)

[security]
detect_secrets = true                 # reject content containing API keys, tokens
max_memory_size_kb = 64               # per-memory content size limit
```

## Implementation Phases

| Phase | Scope | Status |
|-------|-------|--------|
| 1. Foundation | Core storage, stdio MCP, hybrid search, import, secret detection | **Complete** (22 tools) |
| 2. Temporal + Lifecycle | Bi-temporal queries, Hebbian decay, consolidation, writer queue | **Complete** (26 tools) |
| 3. Hardware + Daemon | GPU/NPU/SIMD detection, daemon polish, SIGHUP/SIGUSR1 | **Complete** (28 tools) |
| 4. Extraction | External LLM entity extraction via libcurl | **Complete** (requires `-DENABLE_CURL=ON`) |
| 5. Advanced | Admission control, audit log, auto-model download | **Complete** |
| 6. HTTP Transport | Streamable HTTP (MCP 2025-03-26), multi-session, Bearer auth | **Complete** (libmicrohttpd) |
| 7. Honeypot | Prompt injection scanner, canary records, decoy tools, brute-force detection, enumeration detection | **Complete** |
| 8. GPU/SIMD Acceleration | ROCm HIP GPU embedding, batch embedding, SIMD distance functions wired into live code | **Complete** |
| 9. Integration | Parallel search, TLS config, SSE streaming, recursive imports, background jobs, injection scanner wiring, auth brute-force wiring, entity merging, reader pool | **Complete** |

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

**Optional (auto-detected at build time):**
- libmicrohttpd -- HTTP transport for network MCP access (`sudo apt-get install libmicrohttpd-dev`)
- libcurl -- entity extraction via external LLM + auto-model download (`sudo apt-get install libcurl4-openssl-dev`)
- libsystemd -- sd_notify integration
- libnuma -- NUMA-aware allocation

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
