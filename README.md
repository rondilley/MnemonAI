# MnemonAI

A local-only memory MCP server written in C11.

## What It Does

MnemonAI gives LLM agents persistent, searchable memory via the [Model Context Protocol](https://modelcontextprotocol.io/) (MCP). The daemon (`mnemond`) combines a bi-temporal knowledge graph, full-text search, and vector similarity search into a single process with zero cloud dependencies.

Every piece of data stays on your local filesystem. No API keys required for core operation. No Docker. No cloud.

## Key Features

- **Hybrid Search** -- graph traversal + vector similarity + BM25 keyword search, fused via Reciprocal Rank Fusion (RRF). Three rankers run in parallel via persistent reader pool or ad-hoc pthreads. Tiered FTS queries (AND → NEAR → OR with stopword filtering). Chunked vector indexing splits long content into ~800-byte turn-aware passages for precise embedding.
- **Bi-Temporal Knowledge Graph** -- every fact tracks when it was true (domain time) and when it was recorded (transaction time), with non-destructive updates
- **Local Embeddings** -- llama.cpp in-process with nomic-embed-text-v1.5 (no external API). Task-prefixed asymmetric retrieval (`search_document:`/`search_query:`). Bookend truncation for context overflow.
- **Hardware-Aware** -- auto-detects AMD/NVIDIA/Intel GPU, AMD XDNA NPU, SIMD (AVX2/AVX-512), and NUMA at runtime
- **Network MCP Server** -- Streamable HTTP transport (MCP 2025-03-26 spec) with TLS, Bearer auth, session management, SSE streaming, and Origin validation. Run one instance on a server, connect from any machine.
- **Memory Lifecycle** -- multi-tier memory (episodic/semantic/procedural), Hebbian importance decay, episodic-to-semantic consolidation with automatic entity deduplication and merge
- **Temporal Event System** -- extract dated events from conversation text, search by event date (not storage time), deterministic date arithmetic. Backdatable `created_at` timestamps for historical import.
- **Bulk Import** -- ingest email (mbox), CSV, JSONL, Markdown, and plain text with rich source metadata, configurable chunking, recursive directory traversal, background job tracking, and source timestamp preservation
- **35 MCP Tools** -- Memory CRUD, entity/graph, four search modes, temporal queries and event tools, bulk import, maintenance, hardware introspection, and admin tools
- **Security** -- FSM-based secret detection, prompt injection scanner (integrated into store path), content size limits, auth brute-force detection (per-IP rate limiting), IP allow list, enumeration detection, credential query detection, audit logging, OWASP MCP Top 10 mitigations
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

## Numbers

- 35 MCP tools, 12,500+ lines of C, 343 tests
- Hybrid search (with GPU embedding) at 1M memories, single client: p50 18ms, p95 22ms, p99 24ms
- Hybrid search at 1M + 10 concurrent clients (realistic AI-agent profile): p50 47ms, p99 104ms
- Store throughput (GPU-embedded): ~140 ops/sec sustained through 1M corpus -- backfill of ~100K items in ~12 min
- Memory footprint: ~7.5KB/memory RAM (768-dim embedding + indexes). 1M memories fits in ~8GB
- Saturation ceiling: ~69 ops/sec hybrid, ~95 ops/sec keyword-only (well above the <=10 concurrent design profile)
- LongMemEval benchmark: 100% R@5 oracle, 27.6% R@5 at 19K corpus ([details](doc/BENCHMARK-REPORT.md))

See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for the full architecture document.

## Getting Started

Everything installs under your home directory. No root required. No system files touched.

### What Goes Where

```
~/.local/
    bin/mnemond                         # the binary
    lib/libllama.so, libggml*.so        # llama.cpp libraries
    share/mnemond/
        data/                           # LMDB database (memories, entities, edges)
        fts/                            # SQLite FTS5 keyword index
        vectors/                        # usearch HNSW vector index
        models/                         # embedding model (~150MB, auto-downloaded)
~/.local/etc/mnemond/
    mnemond.conf                        # configuration (optional -- all settings have defaults)
```

### Step 1: Install Build Dependencies

```bash
# Ubuntu / Debian
sudo apt-get install build-essential cmake git libmicrohttpd-dev libcurl4-openssl-dev

# Fedora / RHEL
sudo dnf install gcc gcc-c++ cmake git libmicrohttpd-devel libcurl-devel
```

### Step 2: Build and Install llama.cpp to ~/.local

MnemonAI uses llama.cpp for local embedding generation:

```bash
git clone https://github.com/ggerganov/llama.cpp
cd llama.cpp
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/.local
make -j$(nproc)
make install
cd ../..
```

### Step 3: Build and Install MnemonAI to ~/.local

```bash
git clone https://github.com/rdilley/MnemonAI.git
cd MnemonAI
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$HOME/.local -DCMAKE_INSTALL_PREFIX=$HOME/.local
make -j$(nproc)
make install
```

This installs the `mnemond` binary to `~/.local/bin/`. CMake prints a feature summary at the end showing what was detected.

### Step 4: Add ~/.local/bin to PATH

Add this to your `~/.bashrc` (or `~/.zshrc`):

```bash
export PATH="$HOME/.local/bin:$PATH"
```

Then reload:

```bash
source ~/.bashrc
```

### Step 5: Verify the Install

```bash
mnemond --version             # should print: mnemond v0.7.0 (...)
mnemond --check-config        # should print: Configuration is valid.
```

### Step 6: First Run and Model Download

On first run, mnemond auto-detects your hardware and downloads the recommended embedding model (~150MB from HuggingFace):

```bash
mnemond --stdio               # starts and downloads model if needed
```

Press `Ctrl+C` to stop. The model and data directories are created automatically under `~/.local/share/mnemond/`.

To skip the model download (run without embeddings):

```bash
mkdir -p ~/.local/etc/mnemond
echo -e "[embedding]\nmodel_path = none" > ~/.local/etc/mnemond/mnemond.conf
```

### Step 7: GPU Warmup (AMD/NVIDIA GPU Only)

If you have an AMD or NVIDIA GPU and built llama.cpp with ROCm or CUDA, the first embedding triggers JIT (just-in-time) compilation of GPU kernels for your specific GPU. **This is a one-time cost** but can take 5-15 minutes depending on the GPU target. Subsequent starts are fast (~2 seconds).

Run the warmup before starting the daemon so you can see progress:

```bash
mnemond --warmup
```

This loads the model, compiles GPU kernels, runs one test embedding, and exits. You'll see output like:

```
2026-04-05T05:18:56Z [INFO] mnemond 0.7.0 starting
2026-04-05T05:18:56Z [INFO] CPU: AMD RYZEN AI MAX+ 395 w/ Radeon 8060S (32 cores) SIMD: avx512
2026-04-05T05:18:56Z [INFO] loading embedding model: ~/.local/share/mnemond/models/nomic-embed-text-v1.5.Q8_0.gguf
2026-04-05T05:18:56Z [INFO] embedding model loaded: 768 dimensions
2026-04-05T05:18:56Z [INFO] warmup: running test embedding (this may take several minutes on first run with GPU -- ROCm/CUDA JIT compilation)...
2026-04-05T05:28:12Z [INFO] warmup: complete (768 dimensions). GPU kernels are compiled and cached.
```

**No GPU?** Skip this step -- CPU inference starts instantly with no JIT step.

**Want verbose model loading details?** Set `log_level = debug` in `~/.local/etc/mnemond/mnemond.conf` to see per-tensor and metadata output. At the default `info` level, llama.cpp's verbose output is suppressed.

### Step 8: Run Tests (Optional)

```bash
cd MnemonAI/build
ctest --output-on-failure                                                  # 196 C unit tests
python3 ../test/test_mcp_client.py                                         # 105 MCP tests
```

---

## Troubleshooting: AMD GPU Hangs (gfx1151 / Strix Halo)

AMD Strix Halo APUs (Ryzen AI MAX+ 395, Radeon 8060S, gfx1151) may hang indefinitely during GPU embedding under certain kernel and firmware configurations. The hang occurs when ROCm issues `hipMemset` or `hipMemcpy` to the GPU, which triggers a MES (Micro-Engine Scheduler) firmware deadlock. Symptoms:

- `mnemond --warmup` hangs at "warmup: running test embedding..."
- 100% CPU usage on one core, no GPU activity
- Must be killed with `kill -9` or `SIGTERM` x3

This affects any ROCm HIP workload on gfx1151 (not specific to mnemond or llama.cpp).

### Root Cause

A CWSR (Compute Wave Save/Restore) bug in the kernel's amdgpu driver (kernels < 6.18-rc6) causes a MES firmware deadlock on gfx1151 when dispatching GPU compute kernels. The `amdgpu-dkms-firmware` package (shipped with `amdgpu-install`) can also supply broken MES firmware blobs (versions 0x80/0x83) that exacerbate the issue.

Tracked in: ROCm issues [#5590](https://github.com/ROCm/ROCm/issues/5590), [#5724](https://github.com/ROCm/ROCm/issues/5724), [#5991](https://github.com/ROCm/ROCm/issues/5991), [#6027](https://github.com/ROCm/ROCm/issues/6027) and llama.cpp issues [#15889](https://github.com/ggml-org/llama.cpp/issues/15889), [#19482](https://github.com/ggml-org/llama.cpp/issues/19482).

### Fix

Two changes are needed. Apply both, then reboot:

**1. Disable CWSR** (required for kernels < 6.18):

```bash
# Add kernel boot parameter
sudo sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 amdgpu.cwsr_enable=0"/' /etc/default/grub
sudo update-grub
```

Or for systemd-boot:

```bash
sudo kernelstub -a "amdgpu.cwsr_enable=0"
```

**2. Remove amdgpu-dkms-firmware** (if installed via `amdgpu-install`):

```bash
sudo apt autoremove --purge amdgpu-dkms-firmware
```

This package can override the kernel's built-in firmware with broken MES blobs. The kernel's native firmware (shipped with `linux-firmware`) is correct for gfx1151.

**3. Reboot and verify:**

```bash
sudo reboot
# After reboot:
cat /proc/cmdline | grep cwsr    # should show: amdgpu.cwsr_enable=0
mnemond --warmup                 # should complete in seconds, not hang
```

### Permanent Fix

Upgrade to kernel 6.18+ which includes the CWSR fix for gfx1151. Once on 6.18+, the `amdgpu.cwsr_enable=0` parameter can be removed.

### Fallback: CPU-Only Mode

If GPU issues persist, force CPU-only embedding by setting `gpu_layers = 0` in the config:

```ini
# ~/.local/etc/mnemond/mnemond.conf
[embedding]
gpu_layers = 0
```

This bypasses ROCm initialization entirely. CPU inference is slower but starts instantly with no JIT step.

---

## Auto-Start with systemd (User Service)

Run mnemond as a background daemon that starts automatically on login. No root required -- this uses systemd's per-user service manager.

### Step 1: Install the User Service File

A ready-made service file is included in the repo:

```bash
mkdir -p ~/.config/systemd/user
cp etc/mnemond-user.service ~/.config/systemd/user/mnemond.service
```

`%h` in the service file expands to your home directory automatically. The service runs as your user, not root.

### Step 2: Enable and Start

```bash
systemctl --user daemon-reload
systemctl --user enable mnemond        # start on login
systemctl --user start mnemond         # start now
```

### Step 3: Verify

```bash
systemctl --user status mnemond        # should show "active (running)"
journalctl --user -u mnemond -f        # follow logs
```

### Step 4: Enable Lingering (Optional -- Run Without Login)

By default, user services stop when you log out. To keep mnemond running even when you're not logged in (useful for the HTTP server):

```bash
loginctl enable-linger $USER
```

### Managing the Service

```bash
systemctl --user stop mnemond          # graceful stop (SIGTERM, 10s timeout)
systemctl --user restart mnemond       # restart (e.g., after config change)
systemctl --user disable mnemond       # don't start on login
journalctl --user -u mnemond --since today  # today's logs
kill -HUP $(pgrep mnemond)            # reload config without restart
kill -USR1 $(pgrep mnemond)           # dump stats to log
```

### Signal Reference

| Signal | Action |
|--------|--------|
| `SIGTERM` | Graceful shutdown (10s timeout, then forced exit) |
| `SIGINT` | Same as SIGTERM |
| `SIGHUP` | Reload config (log level, decay params) without restart |
| `SIGUSR1` | Dump stats to log: memories/entities/indexes, plus HTTP gauges (sessions, open connections, in-flight requests, slow-request count) and reader-pool depth |
| 3x `SIGTERM` | Immediate forced exit (emergency) |

### Diagnostic Logging

When the HTTP transport is enabled, every request emits a completion log line.
Long-running requests (>= 1 s) and abnormally-terminated requests are logged at
`WARNING`. The TCP connection gauge logs a `WARNING` when crossing 75 / 90 / 100 %
of `max_connections` (with a 60 s cooldown to avoid spam).

Idle connections are reaped after `[http] connection_timeout` (default 120 s),
and accepted sockets get `SO_KEEPALIVE` with a ~120 s dead-peer detection
window so half-open client connections do not pin slots indefinitely. Use
`[http] per_ip_connection_limit` to bound concurrent connections per client IP.

Set `[diag] heartbeat_secs = N` to log the same gauges every N seconds at INFO,
useful for post-mortem when nobody is watching live. SIGUSR1 dumps them on demand
regardless of this setting.

---

## Deployment Modes

### Mode A: Local (stdio) -- Single Machine

The MCP client launches `mnemond` as a child process. Communication is over stdin/stdout. No network, no auth, no configuration needed. This is the simplest setup.

```mermaid
graph LR
    Client["AI Tool<br/>(Claude Code, Cursor, etc.)"] -- "stdin/stdout<br/>JSON-RPC 2.0" --> D["mnemond<br/>(child process)"]
    D --> Storage["LMDB + FTS5 + usearch"]
```

### Mode B: Network Server (HTTP) -- Multiple Machines, Shared Memory

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

1. **Create a config file with HTTP enabled:**

```bash
mkdir -p ~/.local/etc/mnemond
cat > ~/.local/etc/mnemond/mnemond.conf << 'EOF'
[general]
data_dir = ~/.local/share/mnemond
log_level = info

[embedding]
# Leave empty for auto-download on first run
model_path =

[http]
enabled = true
bind = 0.0.0.0
port = 3847

# Access control (use one or both):
# auth_token = YOUR-SECRET-TOKEN-HERE
allow_ips = 192.168.1.0/24, 10.0.0.0/8
EOF
```

**Access control options:**

- **`allow_ips`** -- Comma-separated CIDR allow list. Only IPs matching an entry can connect. Supports single IPs (`10.0.0.5`), subnets (`192.168.1.0/24`), and broad ranges (`10.0.0.0/8`). Connections from all other IPs are rejected with a 403. Leave empty or omit to allow all IPs.

- **`auth_token`** -- Static Bearer token. Clients must send `Authorization: Bearer <token>` on every request. Generate one with: `openssl rand -hex 32`

Both can be used together (IP filter runs first, then auth). For a trusted local network, `allow_ips` alone is sufficient. For exposure to untrusted networks, use `auth_token` (with TLS).

**Connection-pool tuning** (defaults are fine for most setups):

- **`max_connections`** (default 32) -- Global concurrent TCP connection ceiling. New connections beyond this are refused at the OS level.
- **`connection_timeout`** (default 120 s) -- Idle HTTP connections are reaped after this many seconds. Combined with per-socket TCP keepalive (~120 s dead-peer detection), this prevents half-open client sockets from pinning slots indefinitely. Set to `0` to disable, but note that without it, dead clients can fill the pool until they're noticed.
- **`per_ip_connection_limit`** (default 0 = unlimited) -- Cap on concurrent connections from a single client IP. Set to e.g. `8` if you want one misbehaving client to be unable to consume the whole `max_connections` budget.

2. **Start the server** (or restart the systemd service if already enabled):

```bash
# If using systemd (recommended):
systemctl --user restart mnemond

# Or foreground for debugging:
mnemond --foreground
```

3. **Verify the server is running:**

```bash
curl -X POST http://localhost:3847/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR-SECRET-TOKEN-HERE" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'
```

You should see a JSON response with `protocolVersion` and `serverInfo`.

4. **Open firewall port** (if connecting from other machines):

```bash
# Ubuntu (ufw)
sudo ufw allow 3847/tcp

# Fedora (firewalld)
sudo firewall-cmd --add-port=3847/tcp --permanent && sudo firewall-cmd --reload
```

#### Enabling TLS (HTTPS)

For encrypted connections, mnemond supports TLS via libmicrohttpd. You can use certificates from a real CA (Let's Encrypt, etc.) or generate self-signed certificates for internal/development use.

**Option A: Self-signed certificate (quick, internal use)**

Generate a certificate authority (CA) and server certificate. The CA cert can be distributed to clients so they trust the server without `--insecure` flags.

```bash
TLSDIR=~/.local/etc/mnemond/tls
mkdir -p "$TLSDIR"

# 1. Create a local CA (one-time setup)
openssl genrsa -out "$TLSDIR/ca.key" 4096
openssl req -x509 -new -nodes -key "$TLSDIR/ca.key" -sha256 -days 3650 \
  -subj "/CN=mnemond Local CA" -out "$TLSDIR/ca.crt"

# 2. Create a server key and certificate signing request (CSR)
#    Replace "your-server.example.com" with your server's hostname or IP.
openssl genrsa -out "$TLSDIR/server.key" 2048
openssl req -new -key "$TLSDIR/server.key" \
  -subj "/CN=your-server.example.com" -out "$TLSDIR/server.csr"

# 3. Sign the server cert with the CA
#    Add Subject Alternative Names for all hostnames/IPs clients will use.
cat > "$TLSDIR/server.ext" << EOF
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
subjectAltName = @alt_names

[alt_names]
DNS.1 = your-server.example.com
DNS.2 = tars.local
DNS.3 = localhost
IP.1 = 127.0.0.1
EOF

openssl x509 -req -in "$TLSDIR/server.csr" \
  -CA "$TLSDIR/ca.crt" -CAkey "$TLSDIR/ca.key" \
  -CAcreateserial -out "$TLSDIR/server.crt" \
  -days 825 -sha256 -extfile "$TLSDIR/server.ext"

# 4. Restrict permissions
chmod 600 "$TLSDIR/server.key" "$TLSDIR/ca.key"
chmod 644 "$TLSDIR/server.crt" "$TLSDIR/ca.crt"

# Clean up intermediates
rm "$TLSDIR/server.csr" "$TLSDIR/server.ext" "$TLSDIR/ca.srl"
```

This produces:
- `ca.crt` -- CA certificate. Install on clients that need to trust this server.
- `ca.key` -- CA private key. Keep safe; only needed to sign new server certs.
- `server.crt` -- Server certificate. Referenced in mnemond config.
- `server.key` -- Server private key. Referenced in mnemond config.

**Option B: Let's Encrypt (public-facing servers)**

Use [certbot](https://certbot.eff.org/) or [acme.sh](https://github.com/acmesh-official/acme.sh) to obtain certificates, then point the config at the resulting files (typically `/etc/letsencrypt/live/DOMAIN/fullchain.pem` and `privkey.pem`).

**Add TLS to the config:**

Add the `tls_cert` and `tls_key` lines to the `[http]` section:

```ini
[http]
enabled = true
bind = 0.0.0.0
port = 3847
auth_token = YOUR-SECRET-TOKEN-HERE
tls_cert = ~/.local/etc/mnemond/tls/server.crt
tls_key = ~/.local/etc/mnemond/tls/server.key
```

Restart the daemon:

```bash
systemctl --user restart mnemond
```

The log should now show `tls=yes`:

```
[INFO] HTTP transport started: 0.0.0.0:3847/mcp (auth=yes, tls=yes, allow_ips=all, max_conn=32, idle_timeout=120s, per_ip_limit=0)
```

**Verify TLS is working:**

```bash
# With self-signed CA:
curl --cacert ~/.local/etc/mnemond/tls/ca.crt \
  -X POST https://localhost:3847/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR-SECRET-TOKEN-HERE" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"health_check","arguments":{}}}'

# Or skip verification for quick testing (not recommended for production):
curl -k -X POST https://localhost:3847/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR-SECRET-TOKEN-HERE" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"health_check","arguments":{}}}'
```

**Installing the CA cert on client machines:**

Copy `ca.crt` to each client machine and install it in the system trust store. See [Installing the CA Certificate on Client Machines](#installing-the-ca-certificate-on-client-machines) in the Client Configuration section below for detailed instructions for Ubuntu, Fedora, Arch, macOS, and Windows.

---

## Client Configuration

### Local Mode (stdio)

For AI tools running on the same machine as mnemond. The tool launches mnemond as a child process. If `~/.local/bin` is in your PATH (see Step 4 above), use `mnemond`. Otherwise, use the full path.

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
      "command": "/home/YOU/.local/bin/mnemond",
      "args": ["--stdio"]
    }
  }
}
```

Desktop apps don't inherit shell PATH, so use the full path to the binary. Replace `/home/YOU` with your actual home directory.

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
https://your-server:3847/mcp
```

For MCP clients that only support stdio transport, use a local wrapper script that bridges stdio to HTTP (see below).

#### Installing the CA Certificate on Client Machines

If the mnemond server uses a self-signed TLS certificate (see [Enabling TLS](#enabling-tls-https)), each client machine must trust the CA that signed it. Copy the `ca.crt` file from the server to the client, then install it:

**Ubuntu / Debian:**

```bash
sudo cp ca.crt /usr/local/share/ca-certificates/mnemond-ca.crt
sudo update-ca-certificates
```

This updates the system trust store. `curl`, `python3`, `wget`, Node.js, and most MCP clients will trust the mnemond server immediately.

**Fedora / RHEL / CentOS / Rocky:**

```bash
sudo cp ca.crt /etc/pki/ca-trust/source/anchors/mnemond-ca.crt
sudo update-ca-trust
```

**Arch Linux:**

```bash
sudo cp ca.crt /etc/ca-certificates/trust-source/anchors/mnemond-ca.crt
sudo update-ca-trust
```

**macOS:**

```bash
sudo security add-trusted-cert -d -r trustRoot \
  -k /Library/Keychains/System.keychain ca.crt
```

Or open Keychain Access, drag `ca.crt` into "System", double-click it, expand "Trust", and set "When using this certificate" to "Always Trust".

**Windows:**

Option 1 -- GUI:

1. Double-click `ca.crt` to open the Certificate Import Wizard.
2. Select **Local Machine** (requires admin) or **Current User**.
3. Choose **Place all certificates in the following store** > **Browse** > **Trusted Root Certification Authorities**.
4. Click **Finish**.

Option 2 -- PowerShell (as Administrator):

```powershell
Import-Certificate -FilePath .\ca.crt -CertStoreLocation Cert:\LocalMachine\Root
```

Option 3 -- Command Prompt (as Administrator):

```cmd
certutil -addstore "Root" ca.crt
```

**Verifying trust is working:**

After installing the CA cert, verify from the client machine:

```bash
# Should succeed without --cacert or -k flags:
curl -X POST https://your-server:3847/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR-TOKEN" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"health_check","arguments":{}}}'
```

If you see `SSL certificate problem: unable to get local issuer certificate`, the CA cert was not installed correctly. Re-check the steps above for your OS.

#### Stdio-to-HTTP Wrapper Scripts

For MCP clients that only support stdio transport, use a local wrapper script that bridges stdio to HTTP:

**Linux/macOS -- `~/bin/mnemond-remote`:**

```bash
#!/bin/bash
# Bridges stdio to mnemond HTTP server
# Reads JSON-RPC from stdin, POSTs to server, writes response to stdout
SERVER="https://your-server:3847/mcp"
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
set SERVER=https://your-server:3847/mcp
set TOKEN=YOUR-SECRET-TOKEN-HERE
:loop
set /p LINE=
curl -s -X POST %SERVER% -H "Content-Type: application/json" -H "Authorization: Bearer %TOKEN%" -d "%LINE%"
echo.
goto loop
```

Note: The wrapper scripts use `https://`. If the system CA trust store has the mnemond CA cert installed (see above), no extra flags are needed. If you cannot install the CA cert system-wide, add `--cacert /path/to/ca.crt` to the `curl` command in the script.

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
{"status": "ok", "version": "v0.7.0 (...)", "storage_ok": true}
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

INI format. **All settings have defaults** -- mnemond works out of the box with zero configuration. A config file is only needed to change defaults (enable HTTP, set auth token, tune search, etc.).

Config search path:

1. `--config <path>` (command line)
2. `$XDG_CONFIG_HOME/mnemond/mnemond.conf`
3. `~/.config/mnemond/mnemond.conf`
4. `SYSCONFDIR/mnemond.conf` (compiled-in, typically `~/.local/etc/mnemond/mnemond.conf`)
5. `/etc/mnemond/mnemond.conf`

When creating a new config (e.g., `--gen-key` with no existing file), the default location is `SYSCONFDIR/mnemond.conf`.

Default data directory: `~/.local/share/mnemond/` (created automatically on first run).

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
auth_token =                          # optional Bearer token
allow_ips =                           # optional CIDR allow list (e.g., 192.168.1.0/24)
tls_cert = /path/to/cert.pem          # TLS certificate (optional)
tls_key = /path/to/key.pem            # TLS private key (optional)

[threads]
reader_pool_size = 4                  # persistent reader threads (0 = ad-hoc)

[security]
detect_secrets = true                 # reject content containing API keys, tokens
max_memory_size_kb = 64               # per-memory content size limit

[diag]
heartbeat_secs = 0                    # 0 = off; N>0 logs HTTP/reader-pool gauges every N seconds
```

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

- [MCP Reference](doc/MCP-REFERENCE.md) -- complete protocol, tools, and API reference (for developers and AI agents)
- [Architecture](doc/ARCHITECTURE.md) -- full system design (v0.4, peer-reviewed)
- [Benchmark Report](doc/BENCHMARK-REPORT.md) -- LongMemEval retrieval quality and performance analysis
- [Retrieval Analysis](doc/RETRIEVAL-ANALYSIS.md) -- Root cause analysis of ranking quality at scale
- [Research Synthesis](doc/research-synthesis.md) -- landscape analysis and academic survey
- [Landscape Report](doc/memory-mcp-report-v2.md) -- practitioner guide to existing memory MCP servers
- [Token Efficiency Guide](doc/claude-automation-efficiency-2026.pdf) -- Claude automation and token optimization strategies

## License

GPL-3.0-only -- see [LICENSE](LICENSE)
