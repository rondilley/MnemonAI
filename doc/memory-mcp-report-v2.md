# Memory MCP Servers: Practitioner's Landscape Guide

**Prepared for:** Ron Dilley | **Date:** April 2, 2026 (Revised)
**Classification:** Practitioner Landscape Guide — Memory Layer Infrastructure for AI Agents on UNIX Systems
**Version:** 2.0 — Incorporates corrections from independent peer reviews (ChatGPT, Gemini)
**Evidence Retrieval Date:** April 2, 2026 — All star counts, version numbers, and registry data current as of this date

---

## Revision Notes

This is a substantially revised version of the original report (v1.0, April 2, 2026). Two independent peer reviews identified critical issues in the original draft: factual errors, ranking inconsistencies, product conflation, missing citations, and insufficient separation of ecosystem-level security risks from product-specific observations. This revision addresses those findings. Changes are summarized in Appendix A.

**Scope & Limitations:** This document is an opinionated practitioner guide synthesizing documentation review, community analysis, and editorial judgment — not a peer-reviewed empirical study. It does not include reproducible benchmarks, standardized test harnesses, or controlled experiments. Scoring is editorial and reflects the author's assessment of fitness-for-purpose for UNIX-based AI agent deployments. Where claims are not directly verified against primary sources, they are marked accordingly.

---

## Executive Summary

Memory MCP servers solve a fundamental limitation of Large Language Models: every conversation starts from zero. By exposing persistent storage through the Model Context Protocol (MCP) standard, these servers give AI agents the ability to store, retrieve, and reason over accumulated knowledge across sessions.

As of April 2026, PulseMCP lists over 410 memory-focused MCP servers [[PulseMCP](https://www.pulsemcp.com/servers?q=memory)], a number that has grown rapidly from under 380 just weeks earlier, reflecting explosive community interest. The ecosystem spans four architectural approaches — Knowledge Graph, Semantic Vector, File/Markdown-based, and Hybrid — each with distinct tradeoffs for retrieval quality, operational complexity, and security posture.

This guide evaluates six production-viable options for UNIX-based deployments, provides setup guidance, comparative analysis, and a security assessment grounded in the OWASP MCP Top 10 framework (currently in Phase 3 beta [[OWASP](https://owasp.org/www-project-mcp-top-10/)]).

**Key finding:** No single server dominates. The right choice depends on whether you need temporal fact tracking (Zep/Graphiti), turnkey semantic memory (Mem0 Platform or OpenMemory local MCP), human-readable knowledge files (Basic Memory), or raw retrieval infrastructure to build custom memory patterns on (Chroma). The Official Memory server remains a useful reference implementation for learning MCP memory concepts but is not positioned by its maintainers as production-grade.

---

## Part 1: What Memory MCP Is and How It Works

### The Problem: Stateless Intelligence

Large Language Models have no persistent memory. Each conversation begins with a blank context window. Users must re-explain preferences, re-describe projects, and re-establish context. For AI agents performing multi-step tasks across sessions, this limitation is crippling.

### The MCP Solution

The Model Context Protocol, introduced by Anthropic in November 2024 and subsequently donated to the Linux Foundation's Agentic AI Foundation [[Anthropic Announcement](https://www.anthropic.com/news/model-context-protocol); [Linux Foundation Agentic AI Foundation](https://lfaidata.foundation/)], provides a standardized JSON-RPC 2.0 interface between AI clients and external tools. Memory MCP servers implement this protocol to expose persistent storage — the agent can `create_entities`, `add_observations`, `search_memory`, and `delete` knowledge through a uniform tool interface. `[Confidence: Verified — primary sources]`

**How memory flows through an MCP session:**

1. User sends message to AI client (Claude Desktop, Cursor, etc.)
2. Client recognizes memory-relevant context and calls the memory MCP server's tools
3. Memory server stores entities, relationships, or embeddings in its backend
4. On subsequent sessions, client queries the memory server for relevant context
5. Retrieved memories are injected into the LLM's context window as tool results
6. LLM incorporates remembered context into its response

### Architecture Taxonomy

**Knowledge Graph (Entity-Relationship):** Stores structured entities with named relationships and temporal metadata. Best for: tracking facts about people, projects, and how they connect. Representatives: Official Memory, Zep/Graphiti.

**Semantic Vector (Embedding-based):** Converts memories into high-dimensional vectors for similarity search. Best for: finding conceptually related information across large memory stores. Representatives: Mem0, Chroma MCP.

**File/Markdown-based:** Stores memories as human-readable files with search indexes. Best for: knowledge that humans need to audit, edit, or version-control. Representatives: Basic Memory.

**Hybrid:** Combines multiple approaches — typically graph + vector + keyword. Best for: complex use cases requiring both structured relationships and semantic retrieval. Representatives: Zep/Graphiti, Cognee.

### What Memory MCP Is Good For

Memory MCP excels at personal preference retention (coding style, communication preferences), project continuity across sessions, relationship and entity tracking, progressive knowledge building, and cross-tool context sharing (same memory accessible from Claude Desktop, Cursor, VS Code, etc.).

### What Memory MCP Is Bad For

Memory MCP is not well-suited for real-time collaboration (most implementations are single-user, local-first), large-scale document storage (context window limits constrain retrieval), high-frequency transactional data, or use cases requiring strong consistency guarantees. Additionally, servers that use LLMs for memory extraction (Mem0, Zep, Cognee) incur per-memory API costs that can become significant at high write volumes — effectively a "tax on remembering" that should be factored into operational cost models. `[Confidence: Verified — architectural analysis]`

### Security Considerations (Ecosystem-Level)

The OWASP MCP Top 10, currently in Phase 3 beta release [[OWASP MCP Top 10](https://owasp.org/www-project-mcp-top-10/)], identifies ten categories of MCP-specific risk. Three are directly relevant to memory servers:

- **MCP03: Context Over-sharing** — Memory retrieval can dump large volumes of stored context into the LLM's inference window, consuming token budget and potentially exposing sensitive historical data to prompt injection.
- **MCP01: Token Mismanagement & Secret Exposure** — Memories may inadvertently capture and persist API keys, credentials, or sensitive data from prior sessions.
- **MCP06: Tool Poisoning** — A compromised memory server could inject malicious instructions that persist across sessions and influence future agent behavior (memory poisoning).

**Important note on sourcing:** The OWASP MCP Top 10 is a living document in beta, not a finalized published standard. Equixly's widely cited finding that 43% of MCP servers are vulnerable to command injection is an ecosystem-wide statistic across all server categories, not a per-product audit of the memory servers reviewed here [[DEV Community summary](https://dev.to/mistaike_ai/owasp-just-published-an-mcp-top-10-heres-what-it-means-5ebi)]. Product-specific security observations are provided separately in Part 5.

---

## Part 2: Methodology

### Evaluation Approach

This guide evaluates memory MCP servers through documentation review, community analysis, and editorial assessment. It is not based on a reproducible test harness with standardized workloads.

**Sources consulted (by evidence class):**

- **Primary documentation** `[Primary Doc]`: Official READMEs, API docs, and configuration guides for each product
- **Repository analysis** `[Repo]`: GitHub issues, PRs, star counts, release cadence, contributor activity
- **Academic/preprint literature** `[Preprint]`: arXiv papers (not peer-reviewed unless explicitly noted)
- **Community directories** `[Community]`: PulseMCP, ChatForest, Builder.io, Toolradar server listings
- **Security research** `[Security]`: OWASP MCP Top 10, Equixly MCP audit, Invariant Labs MCP-Scan, Amine Raji's practitioner threat model
- **Industry analysis** `[Industry]`: AIMultiple MCP Memory Benchmark (March 2026)

**What this evaluation does NOT include:** Standardized ingestion benchmarks, controlled query/retrieval tests, latency measurements under load, cost-per-memory calculations, or multi-user concurrency testing. Scores in the comparative ranking reflect editorial judgment informed by the sources above, not empirical measurement.

### Selection Criteria

Servers were selected for detailed evaluation based on: production viability on UNIX systems, active maintenance (commits within 90 days), community adoption (GitHub stars, PulseMCP visitor data), and architectural distinctiveness within the taxonomy.

---

## Part 3: Detailed Server Evaluations

### 1. Official Knowledge Graph Memory Server

**Repository:** [modelcontextprotocol/servers](https://github.com/modelcontextprotocol/servers/tree/main/src/memory) `[Primary Doc]`
**Architecture:** Knowledge Graph (Entity → Observation → Relation)
**Backend:** In-memory JSON graph, persisted to JSONL file
**License:** MIT
**Transport:** stdio
**GitHub Stars (parent repo):** ~17,000

> **⚠️ Reference Implementation Disclaimer:** The MCP reference-server repository explicitly positions its servers as educational examples and reference implementations, not production-ready solutions `[Repo — repository README]`. This should be considered when comparing against purpose-built systems.

**How it works:** Stores knowledge as entities (nodes) with observations (facts about entities) and relations (edges between entities). The `read_graph` tool dumps the entire graph into the LLM's context window — there is no selective retrieval, pagination, or relevance filtering. This creates a hard scaling ceiling.

**Setup (UNIX):**
```bash
# Claude Desktop config (~/.config/claude/claude_desktop_config.json)
{
  "mcpServers": {
    "memory": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-memory"],
      "env": { "MEMORY_FILE_PATH": "/home/user/.mcp/memory.json" }
    }
  }
}
```

**Strengths:**
- Zero dependencies beyond Node.js — fastest path to understanding MCP memory concepts
- Clean, readable codebase (~500 lines) — excellent for learning the protocol
- Maintained by the MCP specification authors

**Weaknesses:**
- `read_graph` dumps the entire knowledge store with no filtering, no pagination — becomes a context window liability beyond ~100 entities `[Repo — architectural analysis]`
- No semantic search, no vector retrieval, no relevance scoring
- JSONL file backend has no indexing — retrieval is O(n) full scan
- **Known issue:** A memory leak has been reported in long-running MCP reference servers (GitHub issue #2912 documents a 10GB+ RAM / 6-8 hour issue affecting `sequentialthinking`, not `server-memory` specifically, but the shared infrastructure raises concerns) `[Repo — issue #2912]` `[Confidence: Needs direct verification for server-memory specifically]`

**Best for:** Learning MCP memory concepts, prototyping, small personal knowledge stores (<100 entities).

---

### 2. Zep / Graphiti MCP

**Repository:** [getzep/graphiti](https://github.com/getzep/graphiti) `[Primary Doc]`
**Architecture:** Hybrid — Temporal Knowledge Graph + Vector (Neo4j + embedding layer)
**Backend:** Neo4j graph database with HNSW vector indexes
**License:** Apache 2.0
**Transport:** stdio (Claude Desktop via direct integration) and HTTP/SSE (Docker MCP server, requires `mcp-remote` gateway for HTTP-only deployments) `[Repo — MCP README]`
**GitHub Stars:** ~5,400
**Backed by:** Y Combinator (W24)

**How it works:** Graphiti builds a temporal knowledge graph where every fact has a valid_from/valid_to timestamp. When you tell the agent "I moved from SF to NYC," Graphiti doesn't just add the new fact — it invalidates the old one and creates a temporal edge. This makes it uniquely capable of answering "When did X change?" queries. Entity and edge extraction is LLM-powered (cost implications noted below).

**Setup (UNIX — Docker):**
```bash
# Clone and configure
git clone https://github.com/getzep/graphiti.git
cd graphiti

# Start Neo4j + Graphiti
docker compose -f docker-compose.mcp.yml up -d

# Claude Desktop config (stdio transport — direct)
{
  "mcpServers": {
    "graphiti": {
      "command": "uv",
      "args": ["run", "--directory", "/path/to/graphiti/mcp_server", "graphiti-mcp-server"],
      "env": {
        "NEO4J_URI": "bolt://localhost:7687",
        "OPENAI_API_KEY": "sk-..."
      }
    }
  }
}
```

**Strengths:**
- Only server with true bi-temporal fact tracking — critical for knowledge that changes over time
- Hybrid retrieval: graph traversal + vector similarity + BM25 keyword search
- Reported benchmarks show strong performance on knowledge recall tasks (arXiv preprint `arXiv:2501.13956` — note: preprint, not peer-reviewed) `[Preprint]`
- Active development cadence, strong documentation
- Apache 2.0 license — enterprise-friendly

**Weaknesses:**
- Heaviest infrastructure requirement: Neo4j container (~2-4GB RAM baseline)
- LLM-dependent extraction means per-memory API cost (every `add_memory` call triggers entity/relationship extraction via OpenAI or compatible LLM) `[Confidence: Verified — architecture requires LLM for extraction]`
- Steeper learning curve than simpler alternatives
- Community is smaller than Mem0's; fewer third-party integrations

**Estimated retrieval latency:** Graph traversal + vector search typically 200-800ms for moderate-sized graphs, depending on query complexity and Neo4j instance sizing `[Confidence: Inferred from architecture — not benchmarked]`

**Best for:** Use cases where temporal accuracy matters — tracking evolving project states, changing team structures, research that accumulates and supersedes over time.

---

### 3. Mem0 Ecosystem

> **Product family clarification:** "Mem0" encompasses several distinct products that should not be conflated. This section evaluates them separately based on their materially different architectures, deployment models, and maturity levels.

#### 3a. Mem0 Platform (Cloud-Hosted MCP)

**Documentation:** [docs.mem0.ai/platform/mem0-mcp](https://docs.mem0.ai/platform/mem0-mcp) `[Primary Doc]`
**Architecture:** Managed semantic memory — vector + graph (on Pro tier)
**Backend:** Managed cloud infrastructure (Mem0-hosted)
**License:** Proprietary (API-key access)
**Transport:** HTTP (cloud-hosted MCP endpoint at `mcp.mem0.ai/mcp`)
**Pricing:** Free tier (10K memories, 1K retrievals/mo) → Starter $19/mo → Pro $249/mo

**How it works:** Fully managed memory service. You send memories via API or MCP, Mem0's cloud handles extraction, categorization, deduplication, conflict resolution, and retrieval. No local infrastructure to manage.

**Setup (UNIX):**
```bash
npx mcp-add --name mem0-mcp --type http \
  --url "https://mcp.mem0.ai/mcp" \
  --clients "claude,cursor,windsurf"
# Requires MEM0_API_KEY
```

**Strengths:**
- Lowest operational complexity — zero local infrastructure
- Intelligent extraction, deduplication, and conflict resolution built in
- Graph memory available on Pro tier
- AWS selected Mem0 as exclusive memory provider for their Agent SDK `[Industry]`
- Strong ecosystem: 38K+ GitHub stars (parent `mem0ai/mem0` repo), v1.0 released `[Repo]`

**Weaknesses:**
- Data leaves your machine — unacceptable for some security postures
- Pro tier required for graph memory ($249/mo)
- Vendor lock-in: memories stored in Mem0's cloud
- Pricing jumps are steep (free → $19 → $249)

**Best for:** Teams wanting turnkey memory with zero operational overhead who are comfortable with cloud-hosted data.

#### 3b. OpenMemory Local MCP (Mem0 Ecosystem)

**Repository:** [mem0ai/mem0/openmemory](https://github.com/mem0ai/mem0/tree/main/openmemory) `[Primary Doc]`
**Architecture:** Local semantic memory with built-in UI
**Backend:** Qdrant (vector) + local LLM extraction
**License:** Apache 2.0 (within the mem0ai/mem0 repo)
**Transport:** stdio + HTTP

**How it works:** A local-first memory server that runs entirely on your machine. Part of the Mem0 ecosystem but architecturally distinct from the cloud platform — no data leaves your machine, no cloud sync. Docker-based deployment with Qdrant vector DB, API server, and MCP server components. Includes a built-in web UI for browsing and managing memories.

**Setup (UNIX):**
```bash
git clone https://github.com/mem0ai/mem0.git
cd openmemory
echo "OPENAI_API_KEY=your_key_here" > api/.env
make build && make up
# Optional: UI at localhost with `make ui`
```

**Strengths:**
- All data stays local — strong data sovereignty
- Built-in web UI for memory management
- Cross-tool memory sharing (Claude Desktop, Cursor, VS Code, etc.)
- Active development within the Mem0 parent project

**Weaknesses:**
- Docker stack (API + Qdrant + MCP server) — heavier than single-binary options
- Still requires OpenAI API key for extraction (LLM cost applies)
- Less mature than the cloud platform; documentation is thinner `[Confidence: Verified — docs.mem0.ai]`
- Community support trails the platform product

**Best for:** Practitioners who want Mem0-quality memory with local data sovereignty. The best option in the Mem0 ecosystem for security-conscious UNIX deployments.

#### 3c. Mem0 OSS Framework

**Repository:** [mem0ai/mem0](https://github.com/mem0ai/mem0) `[Primary Doc]`
**Architecture:** Embeddable memory SDK — vector-based with pluggable backends
**License:** Apache 2.0

The Mem0 OSS framework is a Python SDK for adding memory to any AI application. It supports multiple vector store backends (Qdrant, Chroma, Pinecone, Weaviate, and others), not a fixed Qdrant+Postgres stack. This is the building block — not itself an MCP server, but the foundation that OpenMemory and third-party MCP wrappers are built on. Developers building custom memory systems should start here.

> **Note:** The `mem0-mcp` wrapper repository (`mem0ai/mem0-mcp`) now redirects users to the official cloud-hosted MCP endpoint. The standalone wrapper is no longer the recommended path `[Repo — mem0-mcp README]`.

---

### 4. Basic Memory

**Repository:** [basicmachines-co/basic-memory](https://github.com/basicmachines-co/basic-memory) `[Primary Doc]`
**Architecture:** File/Markdown-based with SQLite search index
**Backend:** Markdown files + SQLite FTS5 full-text search
**License:** AGPL-3.0 `[Repo — LICENSE file]`
**Transport:** stdio
**GitHub Stars:** ~2,000

**How it works:** Stores all knowledge as Markdown files in a standard directory structure. Entities, observations, and relations are expressed in a human-readable Markdown format that can be opened in any text editor or Obsidian. A SQLite index provides full-text and semantic search across the Markdown corpus.

**Setup (UNIX):**
```bash
# Install and run
uvx basic-memory mcp

# Claude Desktop config
{
  "mcpServers": {
    "basic-memory": {
      "command": "uvx",
      "args": ["basic-memory", "mcp"]
    }
  }
}
```

**Strengths:**
- Human-readable everything — memories are Markdown files you can read, edit, grep, version-control
- Obsidian-compatible — opens directly in Obsidian for visual graph exploration
- Lightweight: no Docker, no external databases, no LLM API keys for basic operation
- SQLite FTS5 provides fast full-text search without external dependencies
- Good for knowledge workers who want to own and audit their AI's memory

**Weaknesses:**
- No semantic/vector search in the base configuration (keyword-based retrieval only)
- Markdown parsing means lower entity extraction precision than LLM-powered alternatives
- Single-user, single-machine by design — no multi-agent memory sharing
- AGPL-3.0 license has copyleft implications for commercial integration `[Confidence: Verified — repo LICENSE]`
- SQLite may encounter write locks under high-frequency concurrent access `[Confidence: Inferred — SQLite concurrency limitation is well-documented]`

**Estimated retrieval latency:** <50ms for full-text search on typical personal knowledge stores (SQLite FTS5 is very fast for keyword queries) `[Confidence: Inferred from architecture]`

**Best for:** Knowledge workers who want auditable, human-readable AI memory. Ideal for document-heavy workflows, personal knowledge management, and environments where data transparency is paramount.

---

### 5. Cognee MCP

**Repository:** [topoteretes/cognee](https://github.com/topoteretes/cognee) `[Primary Doc]`
**Architecture:** Hybrid — Graph RAG pipeline (LLM extraction → knowledge graph + vector store)
**Backend:** Configurable (Neo4j or FalkorDB for graph; Qdrant, Weaviate, or Chroma for vector)
**License:** Apache 2.0
**GitHub Stars:** ~3,200

**How it works:** Cognee is a cognitive architecture pipeline — it ingests documents, extracts entities and relationships via LLM, builds a knowledge graph, generates vector embeddings, and exposes the result through MCP tools. It's the most "batteries-included" approach but also the heaviest.

**Strengths:**
- Deepest document processing pipeline — handles PDFs, audio, images, not just text
- Configurable backends give deployment flexibility
- Graph + vector hybrid retrieval

**Weaknesses:**
- Heaviest resource footprint: full ingestion pipeline reported at ~40 minutes for 1GB of documents (hardware specifications for this benchmark not disclosed in source) `[Industry — AIMultiple benchmark, March 2026]` `[Confidence: Reported — hardware specs unknown]`
- Most complex setup — multiple backend dependencies
- Smallest community among the options evaluated
- LLM extraction costs apply to every ingested document

**Best for:** Research-heavy workflows requiring deep document understanding with structured knowledge extraction.

---

### 6. Shodh Memory (Emerging — Honorable Mention)

**Repository:** Available on PulseMCP `[Community]`
**Architecture:** Rust-based associative memory using Hebbian learning
**Status:** Early-stage, classified as "official" on PulseMCP

**Why it's worth watching:** Shodh represents a fundamentally different approach — Hebbian learning where concept connections strengthen through co-activation rather than explicit entity extraction. This bio-inspired model could produce emergent memory behaviors that deterministic systems cannot. However, it is too early-stage for production evaluation. `[Confidence: Inferred from limited documentation]`

---

## Part 4: Comparative Analysis

### Recommendation Matrix (By Use Case)

| Use Case | Recommended Server | Why |
|---|---|---|
| Learning MCP memory concepts | Official Memory | Simplest codebase, maintained by spec authors |
| Turnkey cloud memory, zero ops | Mem0 Platform | Managed service, intelligent extraction |
| Local-first with data sovereignty | OpenMemory Local MCP | Full Mem0 intelligence, data stays on machine |
| Temporal fact tracking | Zep/Graphiti | Only server with bi-temporal knowledge graph |
| Human-readable, auditable memory | Basic Memory | Markdown files, grep-able, Obsidian-compatible |
| Deep document processing | Cognee | Multi-modal ingestion pipeline |
| Custom memory infrastructure | Chroma MCP (see below) | Best retrieval engine, build your own abstractions |

### Scoring Rubric

Scores are editorial assessments on a 1-5 scale based on documentation review and architectural analysis. They are not empirical measurements.

| Dimension | What It Measures |
|---|---|
| **Setup Simplicity** | Time and complexity from zero to working MCP memory |
| **Memory Intelligence** | Quality of extraction, deduplication, conflict resolution, temporal tracking |
| **Search Quality** | Retrieval relevance: semantic understanding, hybrid search, precision |
| **Scalability** | Ability to handle growing memory stores without degradation |
| **Maturity** | Documentation quality, community size, issue resolution cadence, API stability |
| **Security Posture** | Default security stance, encryption, access controls, data sovereignty |

### Memory System Rankings

These rankings reflect editorial assessment of fitness as **memory systems** — products that manage the full lifecycle of storing, extracting, relating, and retrieving knowledge for AI agents.

| Rank | Server | Setup | Intelligence | Search | Scale | Maturity | Security | Overall |
|---|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **1** | **Zep/Graphiti** | 2 | 5 | 5 | 5 | 4 | 4 | **4.2** |
| **2** | **Mem0 Platform** | 5 | 5 | 4 | 4 | 4 | 3 | **4.2** |
| **3** | **OpenMemory Local** | 3 | 4 | 4 | 3 | 3 | 4 | **3.5** |
| **4** | **Basic Memory** | 5 | 2 | 3 | 3 | 3 | 5 | **3.5** |
| **5** | **Cognee** | 1 | 4 | 4 | 3 | 2 | 3 | **2.8** |
| **6** | **Official Memory** | 5 | 2 | 1 | 1 | 3 | 2 | **2.3** |

**Ranking rationale:**

**Zep/Graphiti and Mem0 Platform tie at 4.2.** Zep takes the #1 position because temporal knowledge tracking is a unique architectural capability that no other server provides — if you need it, nothing else substitutes. Mem0 Platform wins on operational simplicity and ecosystem breadth. For most practitioners who don't need temporal tracking, Mem0 Platform is the faster path to productive memory.

**OpenMemory Local at 3.5** represents the best balance for security-conscious practitioners who want Mem0-class intelligence without sending data to a cloud service.

**Basic Memory at 3.5** serves a fundamentally different user: the knowledge worker who values transparency, auditability, and human-readable storage over extraction intelligence. Its AGPL-3.0 license should be evaluated against commercial use requirements.

**The Official Memory server at 2.3** reflects its role as a reference implementation. It scores highest on setup simplicity but lowest on search quality and scalability due to architectural constraints (full-graph dump, no selective retrieval). The maintainers position it as educational, not production-grade `[Repo — repository README]`.

### Infrastructure Option: Chroma MCP

Chroma MCP is evaluated separately because it is fundamentally a **vector database with an MCP interface**, not a memory system. It provides no entity extraction, no relationship tracking, no temporal management, no memory lifecycle abstractions, and no deduplication or conflict resolution. Ranking it alongside purpose-built memory systems would create a category error.

**Repository:** [chroma-core/chroma-mcp](https://github.com/chroma-core/chroma-mcp) `[Primary Doc]`
**Architecture:** Vector database — embedding storage + hybrid retrieval
**Backend:** Chroma (embedded or client-server; optional managed Chroma Cloud)
**License:** Apache 2.0
**Parent project stars:** ~26,000 | Monthly pip downloads: ~11M `[Repo]`

**Why it matters despite not being a memory system:** Chroma has the most mature retrieval engine in this evaluation. Its hybrid search (semantic + full-text + metadata filtering), HNSW indexing, and deployment flexibility (ephemeral → embedded → server → SOC 2 Type II managed cloud with BYOC VPC) make it the strongest foundation for teams that want to build custom memory abstractions. If you need raw retrieval quality and are willing to engineer the memory layer yourself, Chroma is the infrastructure to build on.

| Dimension | Score | Notes |
|---|:---:|---|
| Setup Simplicity | 4 | Simple embedded mode; cloud requires configuration |
| Retrieval Quality | 5 | Best hybrid search in the field |
| Scalability | 5 | Proven at scale; 11M monthly downloads, 90K+ downstream repos |
| Memory Abstractions | 1 | None — raw storage primitives only |
| Maturity | 5 | Most mature codebase evaluated |
| Security Posture | 4 | Embedded mode has zero network exposure; Cloud offers SOC 2 + BYOC |

**Estimated retrieval latency:** <50ms for embedded mode with moderate collection sizes `[Confidence: Inferred from architecture — HNSW is sub-linear]`

**Best for:** Teams building custom memory systems who want the best possible retrieval engine as their foundation.

---

## Part 5: Security Assessment for Cybersecurity Practitioners

### Ecosystem-Wide MCP Risks

These risks apply broadly to the MCP ecosystem and are not specific observations about the servers reviewed here. They are sourced from the OWASP MCP Top 10 (Phase 3 beta) and independent security research.

| Risk Category | Description | Source |
|---|---|---|
| **Tool Poisoning (MCP06)** | Malicious instructions embedded in tool descriptions; 84.2% success rate with auto-approval `[Security — Invariant Labs]` | OWASP MCP Top 10 |
| **Command Injection** | 43% of 500+ scanned MCP servers lack input sanitization `[Security — Equixly ecosystem audit]` | Equixly, DEV Community |
| **Context Over-sharing (MCP03)** | Memory retrieval can dump excessive context into LLM inference window | OWASP MCP Top 10 |
| **Shadow MCP Servers (MCP10)** | Unapproved MCP deployments outside security governance | OWASP MCP Top 10 |
| **Memory Poisoning** | Persistent prompt injection through stored memories that influence future sessions | Academic research, industry analysis |

### Product-Specific Security Observations

These are observations specific to the architecture and configuration of each evaluated server. They are based on documentation review and architectural analysis, not penetration testing.

| Server | Data Sovereignty | Encryption at Rest | Auth Model | Key Concern |
|---|---|---|---|---|
| **Official Memory** | Local JSONL file | None (plaintext) | None (stdio trust) | Full graph dump via `read_graph` creates token-budget and data-exposure risk |
| **Zep/Graphiti** | Self-hosted (Neo4j) | Neo4j-configurable | Neo4j auth + API key | Neo4j default config may expose Bolt port; requires explicit network hardening |
| **Mem0 Platform** | Mem0 cloud | Managed | API key | Data leaves your machine; vendor trust dependency |
| **OpenMemory Local** | Local (Docker volumes) | Qdrant-configurable | None (local-only) | Docker network exposure requires explicit port binding restrictions |
| **Basic Memory** | Local files | Filesystem-level | None (stdio trust) | Strongest data sovereignty; files are grep-able, auditable, version-controllable |
| **Chroma MCP** | Configurable | Embedded: N/A; Cloud: SOC 2 + BYOC | API key (cloud) | Embedded mode: zero network attack surface. Cloud: strongest compliance posture |
| **Cognee** | Self-hosted | Backend-dependent | Backend-dependent | Multiple backend dependencies expand the attack surface |

### Mitigation Recommendations

**For all memory MCP deployments:**

1. **Run memory servers in isolated containers** with restricted network policies — memory servers should not have arbitrary outbound internet access `[Security — defense in depth]`
2. **Implement context budget enforcement** — deploy middleware or proxy-layer controls that cap retrieved memory to a fixed token budget (e.g., Top-K retrieval with summarization) before it reaches the LLM's inference window. Containerization protects the host but does not mitigate context over-sharing within the LLM's context window `[Confidence: Verified — architectural analysis]`
3. **Audit stored memories** for inadvertent credential/secret capture — memories can accumulate sensitive data over time without explicit user intent
4. **Pin MCP server versions** and verify package integrity — tool definitions can mutate between versions, changing server behavior
5. **Establish memory hygiene practices** — identify which servers provide tools for the agent to self-prune, de-duplicate, or expire stale memories. Without maintenance, all memory servers eventually accumulate "context noise" that degrades retrieval quality

**Memory maintenance capabilities by server:**

| Server | Self-Pruning | Deduplication | TTL/Expiry | Manual Cleanup |
|---|:---:|:---:|:---:|:---:|
| Official Memory | ❌ | ❌ | ❌ | ✅ (delete_entities) |
| Zep/Graphiti | ❌ | ✅ (temporal invalidation) | ✅ (valid_to timestamps) | ✅ |
| Mem0 Platform | ✅ (automatic) | ✅ (automatic) | ❌ | ✅ |
| OpenMemory Local | ✅ | ✅ | ❌ | ✅ (UI + API) |
| Basic Memory | ❌ | ❌ | ❌ | ✅ (edit files directly) |
| Chroma MCP | ❌ | ❌ | ❌ | ✅ (collection management) |

---

## Part 6: Selection Guide — Quick Decision Framework

**Start here:**

1. **Do you need temporal fact tracking?** → Zep/Graphiti (nothing else does this)
2. **Are you comfortable with cloud-hosted memory?** → Mem0 Platform (fastest to productive)
3. **Must data stay on your machine?** → OpenMemory Local MCP (Mem0 intelligence, local data)
4. **Do you need human-readable, auditable memory?** → Basic Memory (Markdown files, Obsidian-compatible)
5. **Are you building a custom memory system?** → Chroma MCP (best retrieval engine, build your own abstractions)
6. **Just learning how MCP memory works?** → Official Memory (simplest, reference implementation)

---

## Appendix A: Revision Changelog (v1.0 → v2.0)

| Issue | Source | Resolution |
|---|---|---|
| PulseMCP count "over 400" | ChatGPT review (verified at 374 at time of review; now 413) | Updated to "over 410" with source link and date |
| OWASP MCP Top 10 described as "published March 2026" | ChatGPT review | Corrected to "Phase 3 beta release, living document" with source |
| Mem0/OpenMemory conflated as single product | ChatGPT review | Split into Mem0 Platform, OpenMemory Local, Mem0 OSS, with separate evaluations |
| Ranking table contradicted narrative (Zep scored higher but Mem0 ranked #1) | ChatGPT review | Reordered rankings to match scores; Zep/Graphiti and Mem0 Platform now tie at 4.2 with explicit rationale for ordering |
| Basic Memory license listed as "open source" | ChatGPT review | Corrected to AGPL-3.0 with copyleft note |
| Memory leak attributed to server-memory (PR #3321) | ChatGPT review | Corrected: issue #2912 affects `sequentialthinking`; flagged as needing direct verification for server-memory |
| Graphiti described as requiring `mcp-remote` gateway | ChatGPT review | Corrected: stdio transport is supported directly; gateway needed only for HTTP deployment path |
| Graphiti benchmarks described as "peer-reviewed" | ChatGPT review | Corrected to "arXiv preprint" |
| Official server missing maintainer disclaimer | ChatGPT review | Added reference implementation warning box |
| Security section mixed ecosystem and product-level claims | ChatGPT & Gemini reviews | Split into "Ecosystem-Wide Risks" and "Product-Specific Observations" |
| Chroma ranked in memory-purpose table despite being called "not a memory system" | Gemini review | Moved to separate "Infrastructure Option" section |
| Context over-sharing mitigation only addressed host protection | Gemini review | Added context budget enforcement / Top-K retrieval recommendation |
| Missing retrieval latency estimates | Gemini review | Added estimated latency brackets with confidence markers |
| Missing memory maintenance / pruning analysis | Gemini review | Added memory maintenance capabilities table |
| Missing LLM cost implications for extraction-dependent servers | Gemini review | Added "tax on remembering" note in Part 1 and per-server cost notes |
| No inline citations | ChatGPT review | Added claim-level citations with evidence class tags throughout |
| Title "State of the Art Report" overstated | ChatGPT review | Retitled to "Practitioner's Landscape Guide" with explicit scope/limitations |
| No confidence markers on claims | ChatGPT review | Added `[Confidence: ...]` markers on key claims |

---

## Appendix B: Sources Referenced

**Primary Documentation:**
- [MCP Specification & Anthropic Announcement](https://www.anthropic.com/news/model-context-protocol)
- [MCP Reference Servers Repository](https://github.com/modelcontextprotocol/servers)
- [Zep/Graphiti Repository & MCP README](https://github.com/getzep/graphiti)
- [Mem0 Platform Docs](https://docs.mem0.ai/platform/mem0-mcp)
- [Mem0 OSS Repository](https://github.com/mem0ai/mem0)
- [Mem0 Platform vs OSS Comparison](https://docs.mem0.ai/platform/platform-vs-oss)
- [OpenMemory MCP Introduction](https://mem0.ai/blog/introducing-openmemory-mcp)
- [Basic Memory Repository](https://github.com/basicmachines-co/basic-memory)
- [Chroma MCP Repository](https://github.com/chroma-core/chroma-mcp)
- [Cognee Repository](https://github.com/topoteretes/cognee)

**Security Research:**
- [OWASP MCP Top 10 (Phase 3 Beta)](https://owasp.org/www-project-mcp-top-10/)
- [OWASP MCP Top 10 GitHub](https://github.com/OWASP/www-project-mcp-top-10)
- [Equixly MCP Security Audit / CVE Summary](https://dev.to/mistaike_ai/owasp-just-published-an-mcp-top-10-heres-what-it-means-5ebi)
- [Invariant Labs MCP-Scan](https://github.com/invariantlabs-ai/mcp-scan)
- [Amine Raji — MCP Security Top 10 Practitioner's Threat Model](https://aminrj.com/posts/owasp-mcp-top-10/)

**Community & Industry:**
- [PulseMCP Memory Server Directory](https://www.pulsemcp.com/servers?q=memory)
- [Graphiti arXiv Preprint](https://arxiv.org/abs/2501.13956)
- [MCP Reference Servers Issue #2912](https://github.com/modelcontextprotocol/servers/issues/2912)
