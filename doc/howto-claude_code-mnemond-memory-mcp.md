# How To: Wire In and Use the Memory MCP (mnemond)

mnemond is a persistent memory server that gives Claude Code long-term recall across sessions. It stores memories with vector embeddings, keyword indexes, and a knowledge graph — then exposes everything over MCP so Claude can search, store, and connect information without you lifting a finger.

This guide walks through how to configure it in Claude Code and how to instruct Claude to actually use it well.

---

## Prerequisites

- A running mnemond instance (accessible over HTTP)
- Claude Code installed and working

---

## Step 1: Register mnemond as an MCP Server

Edit (or create) your Claude Code settings file at `~/.claude/settings.json`:

```json
{
  "mcpServers": {
    "mnemond": {
      "type": "url",
      "url": "http://your-mnemond-host:3847/mcp",
      "headers": {
        "Content-Type": "application/json",
        "Origin": "http://your-client-hostname"
      }
    }
  }
}
```

| Field | Purpose |
|-------|---------|
| `type` | Must be `"url"` — tells Claude Code this is a remote MCP server, not a local stdio process |
| `url` | The mnemond MCP endpoint. Default port is `3847`. The path is `/mcp`. |
| `headers.Content-Type` | Required. Always `application/json`. |
| `headers.Origin` | Identifies the calling machine to mnemond. Use your client's hostname. |

That's it for wiring. Claude Code will discover all mnemond tools automatically at session start.

---

## Step 2: Instruct Claude How to Use It

The MCP connection gives Claude *access* to memory tools, but without instructions it won't know *when* or *how* to use them. That's what `CLAUDE.md` is for.

Add a memory section to your global `~/.claude/CLAUDE.md`. This file is loaded into every Claude Code conversation as system instructions.

### Recommended CLAUDE.md Memory Block

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
- `hybrid_search` -- default search mode. Fuses BM25 keyword, vector
  similarity, and graph traversal via Reciprocal Rank Fusion. Use this
  unless you have a specific reason to use another mode.
- `keyword_search` -- when you need exact term matching (error codes,
  specific identifiers, configuration keys)
- `vector_search` -- when searching by semantic meaning rather than
  exact terms
- `search_by_entity` -- when exploring relationships between concepts,
  projects, technologies, or people
- `get_related_entities` -- when traversing the knowledge graph to find
  connected concepts
- `retrieve_memory` -- when you have a specific memory ID from a
  previous search result
- `store_memory` -- to persist new knowledge

### Memory tiers
- **episodic** -- events, conversations, debugging sessions, things that
  happened. These decay over time via Hebbian importance scoring unless
  accessed. Default tier.
- **semantic** -- durable facts, definitions, preferences, conventions.
  Use this for knowledge that should persist indefinitely.
- **procedural** -- how-to knowledge, workflows, standard operating
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

### Why This Structure Matters

Without explicit instructions, Claude will either ignore the memory tools entirely or use them haphazardly. The CLAUDE.md block solves three problems:

1. **Session-start behavior** -- tells Claude to search memory *before* starting work, so it doesn't rediscover things it already knows.
2. **Store/retrieve triggers** -- gives Claude clear heuristics for when memory is worth the round-trip.
3. **Tool selection guide** -- mnemond exposes multiple search strategies. Without guidance, Claude defaults to the first one it sees. The guide steers it toward `hybrid_search` as the default (which is the best general-purpose option) and explains when to use specialized searches.

---

## Step 3: Verify the Connection

Start a new Claude Code session and ask:

```
check the mnemond health
```

Claude should call the `health_check` tool and report back component status. If it works, you're wired in.

You can also ask:

```
show me mnemond memory stats
```

This calls `get_memory_stats` and confirms the database is accessible and populated (or empty, if you're starting fresh).

---

## Available Tools Reference

Once connected, Claude has access to all of these mnemond operations:

### Search

| Tool | Description | When to Use |
|------|-------------|-------------|
| `search_hybrid` | RRF fusion of keyword + vector + graph | Default for all searches |
| `search_keyword` | FTS5 BM25 keyword search | Exact terms, error codes, identifiers |
| `search_semantic` | Vector similarity search | Conceptual/meaning-based queries |
| `search_entities` | Search entities by name, type, or observations | Finding known concepts |
| `search_temporal` | Time-filtered search | "What happened last week?" |

### Store and Retrieve

| Tool | Description |
|------|-------------|
| `store_memory` | Store new memory with embedding + entity extraction |
| `retrieve_memory` | Fetch a specific memory by UUID |
| `update_memory` | Update content (re-embeds automatically) |
| `delete_memory` | Soft-delete a memory |
| `list_memories` | List memories with optional tier/source filtering |

### Knowledge Graph

| Tool | Description |
|------|-------------|
| `create_entity` | Create a node (project, person, concept, etc.) |
| `add_observation` | Append an observation to an existing entity |
| `create_relation` | Create an edge between two entities |
| `get_entity_graph` | Get an entity with its edges and neighbors (configurable depth) |
| `get_history` | Version history for an entity |
| `get_state_at_time` | Entity state at a specific point in time |
| `get_changes_since` | Change feed since a timestamp |

### Maintenance

| Tool | Description |
|------|-------------|
| `health_check` | Daemon health and component status |
| `get_memory_stats` | Counts of memories, entities, edges |
| `get_index_stats` | Detailed per-engine stats (LMDB, FTS5, vector) |
| `consolidate_memories` | Promote episodic memories to semantic tier |
| `prune_stale` | Find memories eligible for pruning by age/importance |
| `rebuild_indexes` | Rebuild FTS5 and/or vector indexes from LMDB |
| `get_hardware_info` | CPU, GPU, SIMD, RAM, NUMA detection |

### Import

| Tool | Description |
|------|-------------|
| `import_file` | Import from JSONL, CSV, mbox, text, or markdown |
| `import_directory` | Bulk import from a directory |
| `import_batch` | Import an array of memories (max 1000) |
| `get_import_status` | Check import job progress |

### Admin

| Tool | Description |
|------|-------------|
| `export_all_memories` | Full database export as JSONL |
| `admin_reset_auth` | Reset auth tokens (requires admin key) |
| `set_system_config` | Update runtime configuration |
| `debug_raw_query` | Raw LMDB query (debug mode only) |

---

## Memory Tiers Explained

mnemond organizes memories into three tiers that control retention behavior:

### Episodic (default)

What happened. Debugging sessions, conversations, one-off discoveries. These decay over time via Hebbian importance scoring -- memories that are never accessed again gradually lose importance and become eligible for pruning. Good for: "We hit this error on Tuesday" or "The deploy failed because of X."

### Semantic

Durable facts. Definitions, preferences, conventions, architectural decisions. These persist indefinitely and don't decay. Good for: "This project uses pytest, not unittest" or "The user prefers terse responses."

### Procedural

How-to knowledge. Workflows, standard operating procedures, repeatable processes. Also persistent. Good for: "To deploy this service, run X then Y then Z" or "When adding a new LLM provider, update config.py and analyzer.py."

When storing a memory, specify the tier explicitly:

```
store this as a semantic memory: the project uses faster-whisper as the preferred transcription backend
```

If you don't specify, mnemond defaults to episodic.

---

## Tips for Effective Use

**Be specific in your store requests.** "Remember that we use faster-whisper" is better than "remember this conversation." Claude will store a focused, tagged memory rather than a vague blob.

**Use tags consistently.** If your project is called "podcastorum," tell Claude to tag project-related memories with "podcastorum." This makes retrieval much more precise.

**Let consolidation work for you.** Episodic memories that prove important get naturally promoted to semantic tier via `consolidate_memories`. You don't have to manually classify everything upfront.

**Prune periodically.** Run `prune_stale` with `dry_run: true` to see what's eligible for cleanup, then run it for real to keep the database focused.

**The knowledge graph is the secret weapon.** Entities and relations let Claude traverse connections: "What projects use this library?" or "What decisions were made about authentication?" The graph search feeds into `hybrid_search` automatically via RRF fusion.

---

## Putting It All Together

The complete setup is two files:

1. **`~/.claude/settings.json`** -- registers mnemond as an MCP server (the plumbing)
2. **`~/.claude/CLAUDE.md`** -- tells Claude when and how to use memory (the behavior)

With both in place, Claude Code will automatically search for relevant context at session start, store important discoveries as you work, build a knowledge graph of your projects and decisions, and carry that context forward into every future conversation.
