# Integrating MnemonAI (mnemond) with Claude

A practical guide to wiring the mnemond MCP memory server into Claude Code, Claude Desktop, and Claude.ai — covering local stdio, network HTTP, and cloud connector deployment modes.

**Prerequisites:** mnemond v0.4.0+ installed and functional (`mnemond --version`). See the [MnemonAI README](https://github.com/rondilley/MnemonAI) for build and install instructions.

---

## Deployment Modes at a Glance

mnemond supports two MCP transports. Which one you use depends on which Claude surface you're targeting and whether the client is on the same machine.

| Claude Surface | Same Machine | Different Machine | Transport |
|----------------|-------------|-------------------|-----------|
| Claude Code    | stdio or HTTP | HTTP | stdio (local), URL (remote) |
| Claude Desktop | stdio or HTTP | HTTP via bridge | stdio (local), stdio bridge (remote) |
| Claude.ai (web/mobile) | N/A | HTTP (public internet) | Remote MCP connector |

**Recommended architecture:** Run one `mnemond` instance as a systemd user service on a dedicated machine (e.g., a GPU-equipped desktop) with HTTP enabled. Point all Claude surfaces at it over the network. This gives every client a shared knowledge graph with a single source of truth.

---

## 1. Claude Code — Local Mode (stdio)

The simplest integration. Claude Code launches `mnemond` as a child process on the same machine.

### Configuration

Add to `~/.claude/settings.json` (global) or `<project>/.claude/settings.json` (project-scoped):

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

This assumes `~/.local/bin` is in your PATH. If not, use the full path:

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "/home/YOUR_USER/.local/bin/mnemond",
      "args": ["--stdio"]
    }
  }
}
```

### Alternative: CLI Registration

```bash
claude mcp add mnemond --command mnemond --args "--stdio"
```

### Verification

From within a Claude Code session:

```
> ask Claude to run health_check via mnemond
```

You should see a tool call returning `{"status": "ok", "version": "v0.4.0", ...}`.

---

## 2. Claude Code — Network Mode (HTTP)

For connecting Claude Code on any machine to a remote `mnemond` instance running as a network daemon.

### Server Setup

On the machine running `mnemond`, create or update the config file (`~/.local/etc/mnemond/mnemond.conf`):

```ini
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
auth_token = YOUR-SECRET-TOKEN-HERE
```

Generate a strong token:

```bash
openssl rand -hex 32
```

Start or restart the service:

```bash
systemctl --user restart mnemond
```

Verify the server is reachable:

```bash
curl -X POST http://HOSTNAME:3847/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR-SECRET-TOKEN-HERE" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'
```

### Client Configuration — Native HTTP

Add to `~/.claude/settings.json`:

```json
{
  "mcpServers": {
    "mnemond": {
      "type": "url",
      "url": "http://HOSTNAME:3847/mcp",
      "headers": {
        "Authorization": "Bearer YOUR-SECRET-TOKEN-HERE"
      }
    }
  }
}
```

Replace `HOSTNAME` with the server's hostname or IP. Use `https://` if TLS is configured.

### Client Configuration — stdio Bridge Fallback

If Claude Code doesn't negotiate HTTP transport cleanly, use a bridge script that translates stdio to HTTP.

Create `~/bin/mnemond-remote`:

```bash
#!/bin/bash
# Bridges stdio JSON-RPC to mnemond HTTP server
SERVER="http://HOSTNAME:3847/mcp"
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

Then in `~/.claude/settings.json`:

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "/home/YOUR_USER/bin/mnemond-remote",
      "args": []
    }
  }
}
```

---

## 3. Claude Desktop — Local Mode (stdio)

Claude Desktop supports MCP servers via its configuration file. Desktop apps do not inherit your shell PATH, so always use absolute paths.

### Configuration

Open Claude Desktop → Settings → Developer → MCP Servers → Add, or edit the config file directly.

**macOS:** `~/Library/Application Support/Claude/claude_desktop_config.json`
**Linux:** `~/.config/Claude/claude_desktop_config.json`
**Windows:** `%APPDATA%\Claude\claude_desktop_config.json`

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "/home/YOUR_USER/.local/bin/mnemond",
      "args": ["--stdio"]
    }
  }
}
```

**Restart Claude Desktop** after saving. An MCP server indicator should appear in the bottom-right of the chat input box. Click it to see the tools mnemond exposes.

---

## 4. Claude Desktop — Network Mode (HTTP)

Claude Desktop's MCP config uses stdio-based server definitions. To connect to a remote `mnemond` HTTP server, use the same bridge script described in the Claude Code section.

### Configuration

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "/home/YOUR_USER/bin/mnemond-remote",
      "args": []
    }
  }
}
```

On Windows, use the batch file equivalent:

**`C:\Users\YOU\bin\mnemond-remote.bat`:**

```batch
@echo off
set SERVER=http://HOSTNAME:3847/mcp
set TOKEN=YOUR-SECRET-TOKEN-HERE
:loop
set /p LINE=
curl -s -X POST %SERVER% -H "Content-Type: application/json" -H "Authorization: Bearer %TOKEN%" -d "%LINE%"
echo.
goto loop
```

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "C:\\Users\\YOU\\bin\\mnemond-remote.bat",
      "args": []
    }
  }
}
```

---

## 5. Claude.ai (Web/Mobile) — Remote MCP Connector

Claude.ai connects to MCP servers from Anthropic's cloud infrastructure, not from your local machine. This means `mnemond` must be publicly reachable over the internet with TLS.

### Server Requirements

1. **Public endpoint with TLS.** Put `mnemond` behind a reverse proxy (Caddy, nginx) with a real certificate, or configure TLS directly in `mnemond.conf`:

```ini
[http]
enabled = true
bind = 0.0.0.0
port = 3847
auth_token = YOUR-SECRET-TOKEN-HERE
tls_cert = /path/to/fullchain.pem
tls_key = /path/to/privkey.pem
```

2. **Firewall.** Allow inbound traffic on your MCP port from Anthropic's IP ranges. See [Anthropic IP addresses](https://support.claude.com) for current ranges.

3. **DNS.** Point a hostname at your server (e.g., `mnemond.yourdomain.com`).

### Reverse Proxy Example (Caddy)

If you prefer to terminate TLS at the proxy:

```
mnemond.yourdomain.com {
    reverse_proxy localhost:3847

    @anthropic_ips remote_ip <ANTHROPIC_IP_RANGES>
    handle @anthropic_ips {
        reverse_proxy localhost:3847
    }
    handle {
        respond "Forbidden" 403
    }
}
```

Caddy handles Let's Encrypt certificates automatically.

### Adding the Connector in Claude.ai

**Individual users (Pro plan):**

1. In claude.ai, click the **"+"** button in the lower-left of the chat input
2. Click **"Connectors"**
3. Click **"Add"**
4. Enter the MCP server URL: `https://mnemond.yourdomain.com/mcp`
5. Configure authentication under **Advanced settings**

**Team/Enterprise plans:**

1. An Owner or Primary Owner goes to **Organization Settings → Connectors**
2. Click **"Add"**
3. Enter the MCP server URL
4. Configure OAuth Client ID and Client Secret under **Advanced settings**
5. Click **"Add"** to save
6. Individual users then enable the connector via the **"+" → "Connectors"** menu per conversation

### Authentication Caveat

Claude.ai's custom connector flow expects OAuth-based authentication. mnemond uses Bearer token auth. To bridge this gap, you have a few options:

- **OAuth2 proxy:** Place `oauth2-proxy` or a similar shim in front of mnemond that handles the OAuth dance and forwards requests with the Bearer token.
- **Caddy/nginx auth shim:** A lightweight auth endpoint that exchanges OAuth credentials for your static Bearer token.
- **Monitor Anthropic's connector auth updates:** The connector system is actively evolving, and direct Bearer/API key support may be added.

For now, Claude Code and Claude Desktop with HTTP are the most frictionless paths. Claude.ai integration works but requires the OAuth bridging step.

---

## 6. Making Claude Actually Use Memory

Wiring up the MCP server gives Claude *access* to memory tools. It does not give Claude *behavior* around when and how to use them. The model discovers the 35 tools via the MCP handshake, but without explicit guidance, it won't proactively store or retrieve memories.

### The CLAUDE.md Memory Protocol

Add this to your global `~/.claude/CLAUDE.md` or project-level `CLAUDE.md`:

```markdown
## Memory (mnemond)

You have access to a persistent memory server via MCP. Use it to maintain
context across sessions and build a knowledge graph of projects, decisions,
and conventions.

### Always do at session start
- Run `hybrid_search` for context relevant to the current task or project
- Review results before proceeding to avoid re-discovering known information

### When to store memories
- Architectural decisions and their rationale
- User preferences, conventions, and standards discovered during work
- Significant errors encountered and their root cause/resolution
- Project-specific terminology, acronyms, and definitions
- Key relationships between systems, people, and components
- Task outcomes and lessons learned

### When to retrieve memories
- Before making architectural or design decisions
- When encountering unfamiliar project conventions or terminology
- When the user references prior work or asks to continue something
- When context from a previous session would improve the current response

### Tool selection
- `hybrid_search` — default search mode. Fuses BM25 keyword, vector
  similarity, and graph traversal via Reciprocal Rank Fusion. Use this
  unless you have a specific reason to use another mode.
- `keyword_search` — when you need exact term matching (error codes,
  specific identifiers, configuration keys)
- `vector_search` — when searching by semantic meaning rather than
  exact terms
- `search_by_entity` — when exploring relationships between concepts,
  projects, technologies, or people
- `get_related_entities` — when traversing the knowledge graph to find
  connected concepts
- `retrieve_memory` — when you have a specific memory ID from a
  previous search result
- `store_memory` — to persist new knowledge

### Memory tiers
- **episodic** — events, conversations, debugging sessions, things that
  happened. These decay over time via Hebbian importance scoring unless
  accessed. Default tier.
- **semantic** — durable facts, definitions, preferences, conventions.
  Use this for knowledge that should persist indefinitely.
- **procedural** — how-to knowledge, workflows, standard operating
  procedures. Use this for repeatable processes.

### Tagging conventions
- Always include 2-5 descriptive tags
- Use consistent tag vocabulary: project names, technology names,
  category labels (e.g., "architecture", "debugging", "preference",
  "convention", "decision")
- Include the project name as a tag when storing project-specific context

### What NOT to store
- Ephemeral information (temporary file paths, one-off calculations)
- Sensitive credentials, API keys, or secrets (mnemond will reject these
  automatically via secret detection)
- Verbatim copies of large files (store summaries and references instead)
```

### Session Resumption Slash Command

Create `.claude/commands/resume.md` in your project:

```markdown
Search memory for recent context about the current project. Look for:
1. Recent architectural decisions
2. Work in progress or incomplete tasks
3. Known issues or blockers
4. Relevant conventions and preferences

Summarize what you find, then ask how to proceed.
```

Usage in Claude Code:

```
> /resume
```

### Example Tool Call Flow

When properly configured, a typical Claude Code session looks like this:

```
> /resume

⏺ mnemond:hybrid_search(query="current project context recent work")
  → 4 results returned

⏺ mnemond:search_by_entity(name="MCP Firewall")
  → 3 related entities: [Policy Engine, Tool Schema Validation, ...]

Based on your recent work, here's where things stand:
- The MCP Firewall v1.1 architecture doc is complete
- The bidirectional proxy implementation is in progress
- Open item: tool schema validation needs test coverage
- Convention: you prefer process-before-platform thinking

How would you like to proceed?

> Let's add the test coverage for schema validation

⏺ mnemond:hybrid_search(query="tool schema validation MCP Firewall tests")
  → 2 results with prior context on validation approach

⏺ [... normal coding work happens ...]

⏺ mnemond:store_memory(
    content="Added 14 unit tests for MCP Firewall tool schema validation covering: malformed JSON-RPC, missing required fields, type mismatches, oversized payloads, and injection attempts in tool names.",
    source_type="conversation",
    tags=["mcp-firewall", "testing", "schema-validation"],
    tier="episodic"
  )
```

### Entity Graph: The Compounding Value

As memories accumulate, mnemond's knowledge graph connects concepts automatically. After enough stored memories, the entity graph becomes the primary value — not individual memory retrieval but relationship traversal.

Example: searching for `search_by_entity(name="PostgreSQL")` might return connections to specific projects, migration decisions, version preferences, performance tuning notes, and related technologies — all linked through the graph without requiring you to remember exactly what you stored or when.

The bi-temporal model means the graph also tracks *when* facts were true and *when* they were recorded, so you can query historical state ("what did we believe about the database architecture in January?") without that information being overwritten by later updates.

---

## 7. Verification Checklist

After completing integration, verify each layer:

**Server health:**

```bash
# Local stdio
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"health_check","arguments":{}}}' | mnemond --stdio

# Remote HTTP
curl -s -X POST http://HOSTNAME:3847/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR-TOKEN" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"health_check","arguments":{}}}'
```

Expected: `{"status": "ok", "version": "v0.4.0", "storage_ok": true}`

**Tool discovery (from Claude Code):**

Ask Claude to "list all available mnemond tools." It should report 35 tools.

**Store and retrieve round-trip:**

Ask Claude to "store a test memory with content 'integration test' and tag 'test', then search for it." Confirm the memory is stored, searchable, and retrievable by ID.

**Embedding verification:**

Ask Claude to "run a vector_search for 'integration test'." If embeddings are working (model loaded), this should return results ranked by semantic similarity. If embeddings are disabled, vector_search will return an appropriate error.

**Temporal event tools:**

Ask Claude to "use extract_events to find dates in 'I attended the workshop on January 10th and the meeting was January 17th', then calculate_duration between those two dates." Expected: 7 days.

---

## 8. Troubleshooting

**Tools don't appear in Claude Code:**
Restart Claude Code after editing `settings.json`. Check that the command path is correct and `mnemond --stdio` runs without errors when executed manually.

**Tools appear but Claude doesn't use them:**
Add the CLAUDE.md memory protocol section. Without behavioral guidance, the model has no reason to call memory tools proactively.

**HTTP connection refused:**
Verify `mnemond` is running with HTTP enabled (`systemctl --user status mnemond`). Check firewall rules. Test with `curl` directly.

**Authentication failures on claude.ai:**
The custom connector expects OAuth. Bearer token auth requires an OAuth proxy shim. See section 5 for details.

**GPU warmup hangs (AMD Strix Halo):**
Known issue with gfx1151 under certain kernel/firmware combinations. Disable CWSR (`amdgpu.cwsr_enable=0`), remove `amdgpu-dkms-firmware`, and reboot. See the MnemonAI README troubleshooting section for full details.

**Secret detection blocking legitimate content:**
Use `skip_secret_check: true` in the `store_memory` arguments (use with caution). Or adjust the secret detection patterns in the config if you're getting false positives on non-sensitive content.

---

## References

- [MnemonAI GitHub](https://github.com/rondilley/MnemonAI)
- [MCP Reference (full tool documentation)](https://github.com/rondilley/MnemonAI/blob/main/doc/MCP-REFERENCE.md)
- [Architecture Document](https://github.com/rondilley/MnemonAI/blob/main/doc/ARCHITECTURE.md)
- [Model Context Protocol Specification](https://modelcontextprotocol.io/)
- [Claude Code MCP Documentation](https://code.claude.com/docs/en/mcp)
- [Claude Desktop MCP Setup](https://support.claude.com/en/articles/10949351-getting-started-with-local-mcp-servers-on-claude-desktop)
- [Claude.ai Custom Connectors](https://support.claude.com/en/articles/11175166-get-started-with-custom-connectors-using-remote-mcp)

