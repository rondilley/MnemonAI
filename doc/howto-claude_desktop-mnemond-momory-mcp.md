# How To: Wire In and Use the Memory MCP (mnemond)

**mnemond** is a local, C-based memory server that speaks MCP (Model Context Protocol). It gives Claude persistent memory across conversations — keyword search, vector similarity, a knowledge graph, and time-based retrieval — all running on your own hardware with zero cloud dependencies.

Current version: **v0.4.0**

---

## 1. Prerequisites

- **mnemond** compiled and running on a host reachable over HTTP (the daemon exposes an `/mcp` endpoint)
- **Claude Desktop** (macOS or Windows) with MCP support enabled
- **Python 3** with the `requests` library installed on the machine running Claude Desktop
- The stdio-to-HTTP shim script (see below) saved to `$HOME/bin/`

---

## 2. Architecture: The Stdio-to-HTTP Shim

Claude Desktop only speaks **stdio** to MCP servers — it launches a local process, writes JSON-RPC to its stdin, and reads responses from its stdout. mnemond, however, runs as a persistent HTTP service (e.g., on a dedicated workstation or server) exposing its MCP endpoint at `http://<host>:<port>/mcp`.

The bridge between these two worlds is a small Python shim that Claude Desktop launches as its "MCP server." The shim reads JSON-RPC lines from stdin, POSTs them to mnemond's HTTP endpoint, and writes the responses back to stdout.

```mermaid
graph LR
    A["Claude Desktop"] <-->|stdio\nstdin/stdout| B["mnemond-remote.py\n($HOME/bin/)"]
    B <-->|HTTP POST\n/mcp| C["mnemond\n(remote host)"]
```

---

## 3. Install the Shim

Save the following as `$HOME/bin/mnemond-remote.py` (adjust `SERVER` to point at your mnemond instance):

```python
import sys
import json
import requests

SERVER = "http://<mnemond-host>:3847/mcp"
#TOKEN = "<your-bearer-token>"
#CA_CERT = r"C:\Users\<username>\.claude\mnemond-ca.pem"

session = requests.Session()
session.headers.update({
    "Content-Type": "application/json"
#    "Authorization": f"Bearer {TOKEN}"
})
#session.verify = CA_CERT

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        json.loads(line)  # validate it's JSON
        r = session.post(SERVER, data=line)
        response = r.text.strip()
        if response:
            sys.stdout.write(response + "\n")
            sys.stdout.flush()
    except Exception as e:
        sys.stderr.write(f"mnemond shim error: {e}\n")
        sys.stderr.flush()
```

The commented-out `TOKEN`, `Authorization` header, and `CA_CERT` lines show where to enable TLS client verification and bearer-token authentication when you're ready to harden the connection. For now, the shim runs plain HTTP.

---

## 4. Claude Desktop Configuration

Edit your Claude Desktop config file:

- **macOS:** `~/Library/Application Support/Claude/claude_desktop_config.json`
- **Windows:** `%APPDATA%\Claude\claude_desktop_config.json`

Add (or merge) the mnemond entry into the `mcpServers` block:

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "python",
      "args": [
        "C:\\Users\\<username>\\bin\\mnemond-remote.py"
      ]
    }
  }
}
```

On macOS/Linux, adjust the path accordingly:

```json
{
  "mcpServers": {
    "mnemond": {
      "command": "python",
      "args": [
        "/home/youruser/bin/mnemond-remote.py"
      ]
    }
  }
}
```

Restart Claude Desktop after editing. You should see mnemond appear in the MCP tools list (hammer icon).

---

## 5. Tool Reference — What You Can Ask Claude To Do

Once wired in, Claude has access to 32 mnemond tools. Here's what they do, grouped by function.

### 5.1 Storing Memories

| Tool | What It Does |
|---|---|
| `store_memory` | Store a single memory. Accepts `content` (required), plus optional `tier` (episodic / semantic / procedural), `tags`, `source_type`, `source_author`, `source_id`. |
| `import_batch` | Bulk-import up to 1000 memories in one call. Each item takes `content`, `tier`, `tags`, `source_type`. |
| `import_file` | Import from a local file — supports JSONL, CSV, mbox, plain text, and markdown. Set `chunking` (paragraph / line / page / none) and `format` (auto-detected by default). |
| `get_import_status` | Check progress of an import job by `job_id`. |

**Example prompt:**
> "Store this as a procedural memory tagged 'runbook': To rotate the API keys, first disable the old key in IAM, generate a new one, update the vault, then re-enable."

### 5.2 Searching Memories

Four search modes, each optimized for different retrieval patterns:

| Tool | Mode | Best For |
|---|---|---|
| `search_keyword` | FTS5 / BM25 | Exact terms, names, error codes, specific strings |
| `search_semantic` | Vector similarity | Conceptual / fuzzy queries ("what do I know about network segmentation?") |
| `search_hybrid` | Graph + vector + keyword with RRF fusion | General-purpose — best default choice |
| `search_temporal` | Time-filtered | "What did I store last week?" — uses `since` and `until` (ISO datetime) |

All accept `top_k` to control how many results come back.

**Example prompts:**
> "Search my memory for anything about the Questco engagement."
> "What have I stored in the last 48 hours?"
> "Do a semantic search for 'privileged account naming conventions'."

### 5.3 Knowledge Graph (Entities & Relations)

mnemond maintains a knowledge graph alongside raw memories. Entities are nodes; relations are edges.

| Tool | What It Does |
|---|---|
| `create_entity` | Create a node. Requires `name` and `entity_type`. Optionally seed it with `observations` (string array). |
| `add_observation` | Append a new observation to an existing entity by `entity_id`. |
| `create_relation` | Create a directed edge between two entities. Requires `edge_type`, `source_id`, `target_id`. |
| `search_entities` | Search entities by `query`, optionally filtered by `entity_type`. |
| `get_entity_graph` | Pull an entity plus its connected neighbors, up to `depth` hops (max 5). |

**Example prompt:**
> "Create an entity of type 'project' named 'MCP Firewall' with observations: 'bidirectional proxy architecture', 'v1.1 design complete'."

### 5.4 Retrieving, Updating, Deleting

| Tool | What It Does |
|---|---|
| `retrieve_memory` | Fetch a single memory by its UUID. |
| `update_memory` | Update a memory's content (re-embeds automatically). |
| `delete_memory` | Soft-delete a memory by UUID. |

### 5.5 Time Travel & Change Tracking

| Tool | What It Does |
|---|---|
| `get_history` | Version history for an entity. Optional `since`/`until` filters. |
| `get_state_at_time` | Reconstruct an entity's state as it was at a specific timestamp. |
| `get_changes_since` | Change feed — all entities modified since a given time. Optional `entity_type` filter. |

### 5.6 Maintenance & Lifecycle

| Tool | What It Does |
|---|---|
| `consolidate_memories` | Trigger episodic → semantic consolidation. Use `dry_run: true` to preview. Optionally scope to a `topic`. |
| `prune_stale` | Find memories eligible for pruning by age and importance decay. Defaults: 90 days, importance < 0.1. `dry_run: true` by default. |
| `list_memories` | Paginated listing with optional `tier` and `source_type` filters. |

**Example prompt:**
> "Do a dry-run consolidation on the topic 'IANS poll responses'."
> "Show me stale memories older than 180 days."

### 5.7 Admin & Diagnostics

| Tool | What It Does |
|---|---|
| `health_check` | Daemon alive? Embedding model loaded? Storage OK? |
| `get_memory_stats` | Counts of memories, entities, edges. |
| `get_index_stats` | Per-engine stats: LMDB, FTS5, vector index. |
| `get_hardware_info` | CPU, GPU, SIMD, RAM, NUMA, storage detection. |
| `rebuild_indexes` | Rebuild FTS5 and/or vector indexes from LMDB source of truth. Target: `fts`, `vector`, or `all`. |
| `set_system_config` | Update runtime config key/value pairs. |
| `export_all_memories` | Full JSONL export. Optional `include_embeddings`. |
| `admin_reset_auth` | Reset auth tokens (requires admin key). |
| `debug_raw_query` | Raw LMDB query (debug mode only). |

---

## 6. Memory Tiers

mnemond organizes memories into three tiers:

- **episodic** — Event-specific, time-bound ("today's meeting notes", "this conversation's key points"). These are candidates for consolidation into semantic memories over time.
- **semantic** — Distilled, durable knowledge ("Ron's three-tier PAM naming convention", "EPSS + KEV > raw CVSS"). Long-term reference.
- **procedural** — How-to knowledge, runbooks, workflows ("to rotate API keys, do X then Y then Z").

When storing, pick the tier that matches the content's intended lifespan and purpose. If omitted, defaults to episodic.

---

## 7. Typical Workflows

### End-of-conversation capture
> "Store the key decisions from this conversation as episodic memories tagged 'project-firewall'."

### Research ingestion
> "Import the file at ~/notes/threat-intel-notes.md into memory, chunked by paragraph, tagged 'threat-intel'."

### Pre-conversation priming
> "Search hybrid for 'Vena Solutions MCP advisory' and give me a summary of what I've stored."

### Periodic maintenance
> "Run a dry-run prune for memories older than 120 days. Then consolidate episodic memories about IANS polls."

### Building a knowledge graph
> "Create entities for each security tool I've built (Vitia_Invenire_AI, wirespy, logpi, Sauron) and link them with 'built_by' relations to an entity for Ron."

---

## 8. Tips

- **`search_hybrid` is your default.** It fuses all three retrieval engines. Use the specialized search tools only when you need to force a specific mode.
- **Tag liberally.** Tags are your cheapest organizational primitive. Use them for projects, clients, topics.
- **Consolidation is not automatic.** You trigger it. Run it periodically to promote high-value episodic memories into semantic ones.
- **Prune with `dry_run: true` first.** Always preview before deleting.
- **`source_type` and `source_author`** help you trace provenance later — use them when importing from external sources (emails, docs, chat logs).
- **The knowledge graph is optional but powerful.** It shines when you have entities with evolving observations over time — projects, people, vendors, tools.

---

## 9. Troubleshooting

| Symptom | Fix |
|---|---|
| Tools don't appear in Claude Desktop | Check `claude_desktop_config.json` syntax. Restart Claude Desktop. Verify the `python` command and shim path are correct. Test the shim manually: `echo '{"jsonrpc":"2.0","method":"initialize","id":1}' | python ~/bin/mnemond-remote.py` — you should get a JSON response. |
| Shim errors on stderr | Check that mnemond is running and reachable at the `SERVER` URL. Verify `requests` is installed (`pip install requests`). |
| `health_check` shows `embedding_model_loaded: false` | The embedding model didn't load — check mnemond startup logs for the model path. |
| Search returns nothing | Run `get_memory_stats` to confirm memories exist. Run `rebuild_indexes` if counts look wrong. |
| Import seems stuck | Call `get_import_status` with the job ID to check progress. |
| Slow vector search | Run `get_hardware_info` to check SIMD support. Rebuild vector index with `rebuild_indexes(target: "vector")`. |
