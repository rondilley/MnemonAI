# mnemond MCP Reference

Complete reference for the mnemond Model Context Protocol (MCP) server. This document covers every tool, the JSON-RPC protocol, both transports, and provides request/response examples you can use directly.

**Audience:** Developers integrating mnemond into applications, and AI agents connecting their MCP clients to this memory server.

**Protocol version:** `2024-11-05`
**Transport specs:** Stdio (newline-delimited JSON) and Streamable HTTP (MCP 2025-03-26)

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Protocol Overview](#protocol-overview)
3. [Transport: Stdio](#transport-stdio)
4. [Transport: Streamable HTTP](#transport-streamable-http)
5. [Connection Lifecycle](#connection-lifecycle)
6. [Tools Reference](#tools-reference)
   - [Memory CRUD](#memory-crud)
   - [Search](#search)
   - [Entity Graph](#entity-graph)
   - [Temporal Queries](#temporal-queries)
   - [Import](#import)
   - [Consolidation and Maintenance](#consolidation-and-maintenance)
   - [Statistics and Monitoring](#statistics-and-monitoring)
   - [Index Management](#index-management)
   - [Admin Tools](#admin-tools)
7. [Error Handling](#error-handling)
8. [Limits and Constraints](#limits-and-constraints)
9. [Best Practices for AI Agents](#best-practices-for-ai-agents)

---

## Quick Start

### Stdio (AI tool launches mnemond as a child process)

```bash
# Start mnemond in stdio mode
mnemond --stdio
```

Send JSON-RPC on stdin, read responses on stdout, one JSON object per line:

```
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"my-agent","version":"1.0"}}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"store_memory","arguments":{"content":"The project uses PostgreSQL 16 for the main database.","source_type":"conversation","tags":["database","infrastructure"]}}}
```

### HTTP (mnemond running as a network daemon)

```bash
# Store a memory via HTTP
curl -X POST http://localhost:3847/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR-TOKEN" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"store_memory","arguments":{"content":"The project uses PostgreSQL 16.","tags":["database"]}}}'
```

---

## Protocol Overview

mnemond implements JSON-RPC 2.0 over the Model Context Protocol. Every request is a JSON-RPC object; every response is a JSON-RPC object.

### Supported Methods

| Method | Direction | Description |
|--------|-----------|-------------|
| `initialize` | Client -> Server | MCP handshake. Required first message. |
| `notifications/initialized` | Client -> Server | Notification after handshake. No response. |
| `tools/list` | Client -> Server | Enumerate all available tools with schemas. |
| `tools/call` | Client -> Server | Invoke a tool by name with arguments. |

### Tool Response Envelope

Every `tools/call` response wraps the result in the MCP content format:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "{\"id\":\"019...\",\"tier\":\"episodic\",\"created_at\":\"2026-04-05T12:00:00Z\"}"
      }
    ],
    "isError": false
  }
}
```

The `text` field contains a JSON string with the tool's actual result. Parse it to get the structured data. When `isError` is `true`, the `text` field contains an `error` string describing what went wrong.

---

## Transport: Stdio

**How it works:** mnemond reads JSON-RPC requests from stdin and writes responses to stdout, one complete JSON object per line (newline-delimited). Logging goes to stderr.

**Framing:** Each request/response is exactly one line terminated by `\n`. No length prefix, no HTTP headers.

**Buffer limit:** 1 MB per line. Requests exceeding this are rejected.

**Launch:**

```bash
mnemond --stdio --config /path/to/config.conf
```

**Example session:**

```
-> {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"my-client","version":"1.0"}}}
<- {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"mnemond","version":"0.4.0"}}}
-> {"jsonrpc":"2.0","method":"notifications/initialized"}
   (no response -- this is a notification)
-> {"jsonrpc":"2.0","id":2,"method":"tools/list"}
<- {"jsonrpc":"2.0","id":2,"result":{"tools":[...]}}
-> {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"health_check","arguments":{}}}
<- {"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"{\"status\":\"ok\",\"version\":\"v0.4.0\",\"storage_ok\":true,\"embedding_model_loaded\":true}"}],"isError":false}}
```

---

## Transport: Streamable HTTP

**Spec:** MCP 2025-03-26 Streamable HTTP

**Endpoint:** `POST /mcp`, `GET /mcp` (SSE), `DELETE /mcp` (session teardown)

**Default port:** 3847

### POST /mcp -- Send a request

Send a JSON-RPC request, receive a JSON-RPC response.

**Required headers:**

| Header | Value | Notes |
|--------|-------|-------|
| `Content-Type` | `application/json` | Required |
| `Origin` | Any valid origin | Validated by server |
| `Authorization` | `Bearer {token}` | Required if auth is configured |
| `Mcp-Session-Id` | UUID string | Auto-created by server if omitted. Returned in response header. |

**Request body limit:** 2 MB

**Example:**

```
POST /mcp HTTP/1.1
Host: localhost:3847
Content-Type: application/json
Authorization: Bearer YOUR-TOKEN
Origin: http://localhost

{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"health_check","arguments":{}}}
```

**Response:**

```
HTTP/1.1 200 OK
Content-Type: application/json
Mcp-Session-Id: a1b2c3d4e5f6...

{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"{\"status\":\"ok\"}"}],"isError":false}}
```

### GET /mcp -- Open SSE stream

Opens a Server-Sent Events connection for server-initiated messages.

**Required headers:** `Mcp-Session-Id` (must reference an existing session from a prior POST)

**Response:** `text/event-stream` with events in the format:

```
data: {"jsonrpc":"2.0","method":"notification","params":{...}}

```

### DELETE /mcp -- End session

Closes the SSE stream (if open) and removes the session.

**Required headers:** `Mcp-Session-Id`

**Response:** `200 OK`

### TLS

Configure TLS by setting `tls_cert` and `tls_key` in the `[http]` config section. When enabled, all HTTP traffic is encrypted.

### Session Limits

- Maximum concurrent sessions: 256
- Sessions are tracked with creation time and last-active timestamps

---

## Connection Lifecycle

Every MCP connection (stdio or HTTP) must follow this sequence:

### Step 1: Initialize

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "protocolVersion": "2024-11-05",
    "capabilities": {},
    "clientInfo": {
      "name": "your-client-name",
      "version": "1.0"
    }
  }
}
```

**Response:**

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2024-11-05",
    "capabilities": {
      "tools": {}
    },
    "serverInfo": {
      "name": "mnemond",
      "version": "0.4.0"
    }
  }
}
```

### Step 2: Send initialized notification

```json
{"jsonrpc": "2.0", "method": "notifications/initialized"}
```

No response is returned. This is a JSON-RPC notification (no `id` field).

### Step 3: Discover tools (optional)

```json
{"jsonrpc": "2.0", "id": 2, "method": "tools/list"}
```

Returns all 35 registered tools with their names, descriptions, and input schemas. Useful for dynamic tool discovery, but not required if you already know the tool names.

### Step 4: Call tools

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "tools/call",
  "params": {
    "name": "store_memory",
    "arguments": {
      "content": "The deployment target is Kubernetes 1.29.",
      "tags": ["infrastructure", "kubernetes"]
    }
  }
}
```

---

## Tools Reference

### Memory CRUD

#### store_memory

Store a new memory. Content is indexed for full-text search, embedded for vector search (if a model is loaded), and scanned for secrets and prompt injection.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `content` | string | yes | | Text content to store. Max 64 KB. |
| `source_type` | string | no | `"mcp"` | Origin label (e.g., `"conversation"`, `"document"`, `"code"`). |
| `source_id` | string | no | `""` | External reference ID (e.g., commit hash, URL). |
| `source_author` | string | no | | Who created this content. |
| `tags` | array of strings | no | `[]` | Categorization tags. Max 1000 tags. |
| `tier` | string | no | `"episodic"` | Memory tier: `"episodic"`, `"semantic"`, or `"procedural"`. |
| `skip_secret_check` | boolean | no | `false` | Bypass secret detection (use with caution). |
| `created_at` | string | no | now | ISO 8601 timestamp to assign. Use for backdating imported historical data. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 10,
  "method": "tools/call",
  "params": {
    "name": "store_memory",
    "arguments": {
      "content": "The API rate limit is 1000 requests per minute per API key.",
      "source_type": "documentation",
      "tags": ["api", "rate-limiting"],
      "tier": "procedural"
    }
  }
}
```

**Response (success):**

```json
{
  "jsonrpc": "2.0", "id": 10,
  "result": {
    "content": [{"type": "text", "text": "{\"id\":\"019570a1-2b3c-7def-8901-234567890abc\",\"tier\":\"procedural\",\"source_type\":\"documentation\",\"created_at\":\"2026-04-05T12:00:00.000Z\"}"}],
    "isError": false
  }
}
```

**Response (secret detected):**

```json
{
  "result": {
    "content": [{"type": "text", "text": "{\"error\":\"content contains potential secret (type: github_pat)\"}"}],
    "isError": true
  }
}
```

**Response (injection blocked):**

```json
{
  "result": {
    "content": [{"type": "text", "text": "{\"error\":\"content blocked: prompt injection score 8.5 >= 7.0\"}"}],
    "isError": true
  }
}
```

---

#### retrieve_memory

Retrieve a single memory by its UUID. Increments the memory's access counter.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `id` | string | yes | Memory UUID. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 20,
  "method": "tools/call",
  "params": {
    "name": "retrieve_memory",
    "arguments": {"id": "019570a1-2b3c-7def-8901-234567890abc"}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"id\":\"019570a1-2b3c-7def-8901-234567890abc\",\"content\":\"The API rate limit is 1000 requests per minute per API key.\",\"source_type\":\"documentation\",\"importance\":1.0,\"access_count\":1,\"created_at\":\"2026-04-05T12:00:00.000Z\"}"}],
  "isError": false
}
```

**Response (not found):**

```json
{
  "content": [{"type": "text", "text": "{\"error\":\"memory not found\"}"}],
  "isError": true
}
```

---

#### update_memory

Update a memory's content. Triggers re-embedding and re-indexing.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `id` | string | yes | Memory UUID. |
| `content` | string | no | New content. Triggers re-embedding if changed. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 30,
  "method": "tools/call",
  "params": {
    "name": "update_memory",
    "arguments": {
      "id": "019570a1-2b3c-7def-8901-234567890abc",
      "content": "The API rate limit was increased to 2000 requests per minute."
    }
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"id\":\"019570a1-2b3c-7def-8901-234567890abc\",\"updated\":true}"}],
  "isError": false
}
```

---

#### delete_memory

Soft-delete a memory. Removes it from all indexes.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `id` | string | yes | Memory UUID. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 40,
  "method": "tools/call",
  "params": {
    "name": "delete_memory",
    "arguments": {"id": "019570a1-2b3c-7def-8901-234567890abc"}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"deleted\":true}"}],
  "isError": false
}
```

---

### Search

All search tools return a `results` array. Each result contains the fields available for that search type. The `top_k` parameter controls how many results are returned (default: 10, max: 50).

#### search_hybrid

The primary search tool. Runs vector similarity, keyword (BM25), and graph search in parallel, then fuses results using Reciprocal Rank Fusion (RRF). **This is the recommended search tool for most use cases.**

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `query` | string | yes | | Natural language search query. |
| `top_k` | integer | no | 10 | Number of results to return. Max 50. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 50,
  "method": "tools/call",
  "params": {
    "name": "search_hybrid",
    "arguments": {"query": "database configuration", "top_k": 5}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"results\":[{\"id\":\"019...\",\"content\":\"The project uses PostgreSQL 16 for the main database.\",\"score\":0.87,\"graph_score\":0.0,\"vector_score\":0.92,\"keyword_score\":0.75,\"tier\":\"episodic\"},{\"id\":\"019...\",\"content\":\"Database connection pool is set to 20 max connections.\",\"score\":0.63,\"graph_score\":0.0,\"vector_score\":0.58,\"keyword_score\":0.71,\"tier\":\"procedural\"}],\"truncated\":false}"}],
  "isError": false
}
```

**Result fields per item:**

| Field | Type | Description |
|-------|------|-------------|
| `id` | string | Memory or entity UUID. |
| `content` | string | Content text (may be truncated for large memories). |
| `score` | number | Final fused relevance score (0.0-1.0). |
| `graph_score` | number | Knowledge graph relevance component. |
| `vector_score` | number | Semantic similarity component. |
| `keyword_score` | number | BM25 keyword relevance component. |
| `tier` | string | Memory tier (`episodic`, `semantic`, `procedural`). |

---

#### search_semantic

Vector similarity search only. Uses cosine similarity against stored embeddings. Best for finding conceptually related content even when different words are used. Requires an embedding model to be loaded.

**Arguments:** Same as `search_hybrid`.

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 51,
  "method": "tools/call",
  "params": {
    "name": "search_semantic",
    "arguments": {"query": "how we connect to the database", "top_k": 5}
  }
}
```

---

#### search_keyword

FTS5 BM25 keyword search only. Best for exact term matching (names, identifiers, error codes).

**Arguments:** Same as `search_hybrid`.

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 52,
  "method": "tools/call",
  "params": {
    "name": "search_keyword",
    "arguments": {"query": "PostgreSQL connection pool", "top_k": 5}
  }
}
```

---

#### search_temporal

Time-filtered search. Returns memories created or modified within a time range.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `since` | string | no | | ISO 8601 start time (inclusive). |
| `until` | string | no | | ISO 8601 end time (inclusive). |
| `top_k` | integer | no | 10 | Max results. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 53,
  "method": "tools/call",
  "params": {
    "name": "search_temporal",
    "arguments": {
      "since": "2026-04-01T00:00:00Z",
      "until": "2026-04-05T23:59:59Z",
      "top_k": 20
    }
  }
}
```

---

### Entity Graph

The knowledge graph stores entities (people, projects, concepts) with observations and typed relations between them.

#### create_entity

Create a new entity node in the knowledge graph.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | yes | Entity name (e.g., `"PostgreSQL"`, `"Auth Service"`). |
| `entity_type` | string | yes | Type classification (e.g., `"technology"`, `"service"`, `"person"`). |
| `observations` | array of strings | no | Initial observations about this entity. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 60,
  "method": "tools/call",
  "params": {
    "name": "create_entity",
    "arguments": {
      "name": "Auth Service",
      "entity_type": "service",
      "observations": ["Handles OAuth2 and JWT tokens", "Deployed on Kubernetes"]
    }
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"id\":\"019...\",\"name\":\"Auth Service\",\"entity_type\":\"service\"}"}],
  "isError": false
}
```

---

#### add_observation

Add a new observation to an existing entity.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `entity_id` | string | yes | Entity UUID. |
| `observation` | string | yes | New observation text. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 61,
  "method": "tools/call",
  "params": {
    "name": "add_observation",
    "arguments": {
      "entity_id": "019...",
      "observation": "Was migrated to use Redis for session storage in March 2026"
    }
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"id\":\"019...\",\"observation_count\":3}"}],
  "isError": false
}
```

---

#### create_relation

Create a directed edge (relation) between two entities.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `source_id` | string | yes | Source entity UUID. |
| `target_id` | string | yes | Target entity UUID. |
| `edge_type` | string | yes | Relation type (e.g., `"depends_on"`, `"maintained_by"`, `"part_of"`). |
| `description` | string | no | Human-readable description of the relation. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 62,
  "method": "tools/call",
  "params": {
    "name": "create_relation",
    "arguments": {
      "source_id": "019...",
      "target_id": "019...",
      "edge_type": "depends_on",
      "description": "Auth Service uses PostgreSQL for credential storage"
    }
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"id\":\"019...\",\"edge_type\":\"depends_on\"}"}],
  "isError": false
}
```

---

#### search_entities

Search entities by name, type, or observation text.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `query` | string | yes | | Search query. |
| `entity_type` | string | no | | Filter to a specific entity type. |
| `top_k` | integer | no | 10 | Max results. Max 50. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 63,
  "method": "tools/call",
  "params": {
    "name": "search_entities",
    "arguments": {"query": "authentication", "entity_type": "service", "top_k": 5}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"entities\":[{\"id\":\"019...\",\"name\":\"Auth Service\",\"entity_type\":\"service\",\"score\":0.91,\"observation_count\":3}],\"count\":1}"}],
  "isError": false
}
```

---

#### get_entity_graph

Get an entity with its edges and related entities, traversed to a configurable depth.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `entity_id` | string | yes | | Entity UUID. |
| `depth` | integer | no | 2 | Traversal depth. Max 5. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 64,
  "method": "tools/call",
  "params": {
    "name": "get_entity_graph",
    "arguments": {"entity_id": "019...", "depth": 2}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"entity\":{\"id\":\"019...\",\"name\":\"Auth Service\",\"entity_type\":\"service\",\"observation_count\":3},\"edges_out\":[{\"id\":\"019...\",\"target_id\":\"019...\",\"edge_type\":\"depends_on\"}],\"edges_in\":[],\"related_entities\":[{\"id\":\"019...\",\"name\":\"PostgreSQL\",\"entity_type\":\"technology\"}]}"}],
  "isError": false
}
```

**Response field limits:** `edges_out` and `edges_in` capped at 100 each. `related_entities` capped at 50.

---

### Temporal Queries

Bi-temporal queries allow you to see how entities changed over time.

#### get_history

Get the version history for an entity.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `entity_id` | string | yes | Entity UUID. |
| `since` | string | no | ISO 8601 start time filter. |
| `until` | string | no | ISO 8601 end time filter. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 70,
  "method": "tools/call",
  "params": {
    "name": "get_history",
    "arguments": {"entity_id": "019..."}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"versions\":[{\"id\":\"019...\",\"name\":\"Auth Service\",\"created_at\":\"2026-04-01T10:00:00Z\",\"updated_at\":\"2026-04-05T14:30:00Z\"}],\"count\":1}"}],
  "isError": false
}
```

---

#### get_state_at_time

Get an entity's state as it existed at a specific point in time.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `entity_id` | string | yes | Entity UUID. |
| `timestamp` | string | yes | ISO 8601 timestamp. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 71,
  "method": "tools/call",
  "params": {
    "name": "get_state_at_time",
    "arguments": {
      "entity_id": "019...",
      "timestamp": "2026-04-03T00:00:00Z"
    }
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"id\":\"019...\",\"name\":\"Auth Service\",\"entity_type\":\"service\",\"observation_count\":2,\"created_at\":\"2026-04-01T10:00:00Z\"}"}],
  "isError": false
}
```

---

#### get_changes_since

Get a feed of entities that changed since a given time. Useful for incremental sync.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `since` | string | yes | | ISO 8601 start time. |
| `entity_type` | string | no | | Filter by entity type. |
| `top_k` | integer | no | 50 | Max results. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 72,
  "method": "tools/call",
  "params": {
    "name": "get_changes_since",
    "arguments": {"since": "2026-04-04T00:00:00Z", "top_k": 20}
  }
}
```

---

### Temporal Event Tools

Tools for extracting, searching, and computing durations between dated events found in memory content. Unlike `search_temporal` (which filters by storage time), these tools work with **event dates mentioned in the text** (e.g., "the workshop on January 10th").

#### extract_events

Parse natural language dates from text and create event entities in the knowledge graph. Handles formats like "January 10th", "March 15, 2023", "Feb 27".

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `content` | string | yes | | Text to extract events from. |
| `context_year` | integer | no | 2023 | Default year for dates without an explicit year. |
| `create_entities` | boolean | no | `true` | Create event entities in the knowledge graph. |

**Response fields:** `events` (array of `{description, event_date, entity_id}`), `extracted` (count), `entities_created` (count).

#### search_events

Search for event entities by their actual event date (not storage time). Returns results sorted chronologically.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `since` | string | no | | Start date (ISO 8601 or natural language like "January 10"). |
| `until` | string | no | | End date. |
| `name` | string | no | | Filter by event name substring. |
| `top_k` | integer | no | 20 | Max results. |

**Response:** Standard result set with event entities sorted by `event_date` ascending.

#### calculate_duration

Compute the number of days between two dates. Accepts ISO 8601 or natural language dates. Eliminates the need for LLM date arithmetic.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `from` | string | yes | | Start date (ISO 8601 or natural language). |
| `to` | string | yes | | End date. |
| `context_year` | integer | no | 2023 | Default year for dates without an explicit year. |

**Response fields:** `days`, `days_inclusive` (counting both endpoints), `from` (parsed ISO 8601), `to` (parsed ISO 8601), `from_is_earlier` (boolean), `weeks` and `remaining_days` (if >= 7 days).

**Example:**

```json
{
  "jsonrpc": "2.0", "id": 50,
  "method": "tools/call",
  "params": {
    "name": "calculate_duration",
    "arguments": {
      "from": "January 10",
      "to": "January 17",
      "context_year": 2023
    }
  }
}
```

Response: `{"days": 7, "days_inclusive": 8, "from": "2023-01-10T12:00:00.000Z", "to": "2023-01-17T12:00:00.000Z", "from_is_earlier": true, "weeks": 1, "remaining_days": 0}`

---

### Import

Bulk import tools for loading existing data.

#### import_batch

Import an array of memories in a single call.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `memories` | array | yes | Array of memory objects (max 1000). |

Each memory object:

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `content` | string | yes | | Memory text. |
| `source_type` | string | no | `"mcp"` | Origin label. |
| `tags` | array of strings | no | `[]` | Tags. |
| `tier` | string | no | `"episodic"` | Memory tier. |
| `created_at` | string | no | now | ISO 8601 timestamp. For backdating imported data. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 80,
  "method": "tools/call",
  "params": {
    "name": "import_batch",
    "arguments": {
      "memories": [
        {"content": "Redis is used for caching.", "source_type": "documentation", "tags": ["redis"]},
        {"content": "Deploy to staging before production.", "source_type": "runbook", "tier": "procedural"},
        {"content": "The CI pipeline runs on GitHub Actions.", "tags": ["ci", "github"]}
      ]
    }
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"imported\":3,\"skipped\":0,\"errors\":0,\"duration_ms\":12.5}"}],
  "isError": false
}
```

---

#### import_file

Import memories from a local file. Supports JSONL, CSV, mbox, plain text, and markdown. Text is automatically chunked.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `path` | string | yes | | Absolute file path. Must be under the user's home directory. |
| `format` | string | no | `"auto"` | `"auto"`, `"jsonl"`, `"csv"`, `"mbox"`, `"text"`, `"markdown"`. |
| `source_type` | string | no | `"import"` | Origin label for imported memories. |
| `chunking` | string | no | `"paragraph"` | `"paragraph"`, `"line"`, `"page"`, `"none"`. |
| `max_chunk_size` | integer | no | 4096 | Max bytes per chunk. |
| `preserve_timestamps` | boolean | no | `false` | Use source timestamps for `created_at`. Mbox: extracts `Date:` header. JSONL: uses `created_at` field. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 81,
  "method": "tools/call",
  "params": {
    "name": "import_file",
    "arguments": {
      "path": "/home/user/notes/meeting-notes.md",
      "format": "markdown",
      "source_type": "meeting",
      "chunking": "paragraph"
    }
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"imported\":12,\"skipped\":0,\"errors\":0,\"chunks_created\":12,\"duration_ms\":45.2}"}],
  "isError": false
}
```

---

#### import_directory

Import all matching files from a directory. Can run asynchronously for large imports.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `path` | string | yes | | Directory path. Must be under user's home directory. |
| `pattern` | string | no | `"*"` | Glob pattern (e.g., `"*.md"`, `"*.txt"`). |
| `format` | string | no | `"auto"` | File format. |
| `recursive` | boolean | no | `false` | Recurse into subdirectories (max depth: 16). |
| `source_type` | string | no | `"document"` | Origin label. |
| `chunking` | string | no | `"paragraph"` | Chunking strategy. |
| `max_chunk_size` | integer | no | 4096 | Max bytes per chunk. |
| `async` | boolean | no | `false` | Run in background. Returns a job ID for polling. |

**Request (synchronous):**

```json
{
  "jsonrpc": "2.0", "id": 82,
  "method": "tools/call",
  "params": {
    "name": "import_directory",
    "arguments": {
      "path": "/home/user/docs",
      "pattern": "*.md",
      "recursive": true
    }
  }
}
```

**Response (synchronous):**

```json
{
  "content": [{"type": "text", "text": "{\"files_processed\":8,\"files_skipped\":2,\"memories_imported\":47,\"duration_ms\":230.1}"}],
  "isError": false
}
```

**Request (asynchronous):**

```json
{
  "jsonrpc": "2.0", "id": 83,
  "method": "tools/call",
  "params": {
    "name": "import_directory",
    "arguments": {
      "path": "/home/user/docs",
      "pattern": "*.md",
      "recursive": true,
      "async": true
    }
  }
}
```

**Response (asynchronous):**

```json
{
  "content": [{"type": "text", "text": "{\"job_id\":\"019...\",\"status\":\"running\",\"path\":\"/home/user/docs\"}"}],
  "isError": false
}
```

---

#### get_import_status

Check the status of import jobs. With no arguments, returns all active/recent jobs.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `job_id` | string | no | Specific job UUID. Omit to list all. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 84,
  "method": "tools/call",
  "params": {
    "name": "get_import_status",
    "arguments": {"job_id": "019..."}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"jobs\":[{\"job_id\":\"019...\",\"path\":\"/home/user/docs\",\"status\":\"complete\",\"files_processed\":8,\"files_skipped\":2,\"memories_imported\":47,\"duration_ms\":230.1}]}"}],
  "isError": false
}
```

**Status values:** `"running"`, `"complete"`, `"failed"`

---

### Consolidation and Maintenance

#### consolidate_memories

Trigger episodic-to-semantic memory consolidation. Clusters related episodic memories, creates entities and relations from them, and merges duplicate entities.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `topic` | string | no | all | Consolidate only memories matching this topic. |
| `dry_run` | boolean | no | `false` | Preview without making changes. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 90,
  "method": "tools/call",
  "params": {
    "name": "consolidate_memories",
    "arguments": {"dry_run": true}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"consolidated_count\":15,\"new_entities\":3,\"new_relations\":5,\"duration_ms\":120.4}"}],
  "isError": false
}
```

---

#### list_memories

List stored memories with optional filtering and pagination.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `source_type` | string | no | | Filter by source type. |
| `tier` | string | no | | Filter by tier (`"episodic"`, `"semantic"`, `"procedural"`). |
| `offset` | integer | no | 0 | Pagination offset. |
| `limit` | integer | no | 50 | Results per page. Max 200. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 91,
  "method": "tools/call",
  "params": {
    "name": "list_memories",
    "arguments": {"tier": "procedural", "limit": 10}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"memories\":[{\"id\":\"019...\",\"content_preview\":\"Deploy to staging before production. Run the full...\",\"source_type\":\"runbook\",\"tier\":\"procedural\",\"importance\":0.95,\"created_at\":\"2026-04-05T12:00:00Z\"}],\"total_count\":1,\"truncated\":false}"}],
  "isError": false
}
```

Note: `content_preview` is the first 200 characters of the full content.

---

#### prune_stale

Find memories eligible for pruning based on age and importance decay. By default operates in dry-run mode (reports candidates without deleting).

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `min_age_days` | integer | no | 90 | Minimum age in days. |
| `min_importance` | number | no | 0.1 | Importance threshold (memories below this score are candidates). |
| `dry_run` | boolean | no | `true` | Set to `false` to actually delete. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 92,
  "method": "tools/call",
  "params": {
    "name": "prune_stale",
    "arguments": {"min_age_days": 60, "dry_run": true}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"candidates\":[{\"id\":\"019...\",\"content_preview\":\"Old meeting notes from...\",\"importance\":0.05,\"age_days\":95.2,\"last_accessed\":\"2026-01-05T08:00:00Z\"}],\"count\":1,\"dry_run\":true}"}],
  "isError": false
}
```

**Candidate limit:** Max 200 candidates returned per call.

---

### Statistics and Monitoring

#### health_check

Check that the daemon is running and storage is accessible.

**Arguments:** None (pass empty object `{}`).

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 100,
  "method": "tools/call",
  "params": {"name": "health_check", "arguments": {}}
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"status\":\"ok\",\"version\":\"v0.4.0\",\"storage_ok\":true,\"embedding_model_loaded\":true}"}],
  "isError": false
}
```

---

#### get_memory_stats

Get aggregate counts of memories, entities, edges, and index sizes.

**Arguments:** None.

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 101,
  "method": "tools/call",
  "params": {"name": "get_memory_stats", "arguments": {}}
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"total_memories\":247,\"total_entities\":38,\"total_edges\":52,\"memory_vectors\":247,\"entity_vectors\":38,\"fts_indexed\":285}"}],
  "isError": false
}
```

---

#### get_hardware_info

Get detected hardware: CPU model, GPU, SIMD capabilities, RAM, NUMA topology.

**Arguments:** None.

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 102,
  "method": "tools/call",
  "params": {"name": "get_hardware_info", "arguments": {}}
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"cpu\":{\"model\":\"AMD RYZEN AI MAX+ 395 w/ Radeon 8060S\",\"cores\":32,\"simd_caps\":[\"avx2\",\"avx512f\",\"avx512bw\"]},\"gpu\":{\"model\":\"AMD Radeon 8060S\",\"vendor\":\"AMD\",\"vram_mb\":65536,\"gtt_mb\":65536,\"rocm\":true},\"ram_mb\":65536,\"numa_nodes\":1,\"has_nvme\":true,\"simd_dispatch\":\"avx512\"}"}],
  "isError": false
}
```

---

#### get_index_stats

Get per-engine statistics for LMDB, FTS5, and the vector index.

**Arguments:** None.

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 103,
  "method": "tools/call",
  "params": {"name": "get_index_stats", "arguments": {}}
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"lmdb\":{\"entity_count\":38,\"edge_count\":52,\"memory_count\":247},\"fts5\":{\"indexed_docs\":285},\"vector\":{\"memory_vectors\":247,\"entity_vectors\":38,\"dimensions\":768}}"}],
  "isError": false
}
```

---

### Index Management

#### rebuild_indexes

Rebuild derived indexes (FTS5 and/or vector) from the LMDB source of truth. Use after data corruption or to force re-indexing.

**Arguments:**

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `target` | string | no | `"all"` | `"fts"`, `"vector"`, or `"all"`. |

**Request:**

```json
{
  "jsonrpc": "2.0", "id": 110,
  "method": "tools/call",
  "params": {
    "name": "rebuild_indexes",
    "arguments": {"target": "all"}
  }
}
```

**Response:**

```json
{
  "content": [{"type": "text", "text": "{\"rebuilt\":[\"fts\",\"vector\"],\"duration_ms\":345.7}"}],
  "isError": false
}
```

---

### Admin Tools

These tools require elevated privileges and are not available to standard MCP clients.

#### admin_reset_auth

Reset authentication tokens for all sessions. Requires admin privileges.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `admin_key` | string | yes | Admin authentication key. |

#### export_all_memories

Export the complete memory database as JSONL. Requires admin privileges.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `format` | string | no | Export format. |
| `include_embeddings` | boolean | no | Include embedding vectors in export. |

#### debug_raw_query

Execute a raw database query. Only available when debug mode is enabled in the server configuration.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | yes | Raw query string. |
| `database` | string | no | Target database. |

#### set_system_config

Update a runtime configuration value. Requires admin privileges.

**Arguments:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `key` | string | yes | Configuration key. |
| `value` | string | yes | New value. |

---

## Error Handling

### JSON-RPC Errors

Protocol-level errors use standard JSON-RPC error codes:

| Code | Meaning | When |
|------|---------|------|
| `-32600` | Invalid Request | Malformed JSON-RPC object. |
| `-32601` | Method not found | Unknown method (not `initialize`, `tools/list`, `tools/call`). |
| `-32602` | Invalid params | Unknown tool name, or missing required arguments. |

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -32602,
    "message": "unknown tool: nonexistent_tool"
  }
}
```

### Tool-Level Errors

Tool-level errors are returned inside the MCP content envelope with `isError: true`:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [{"type": "text", "text": "{\"error\":\"memory not found\"}"}],
    "isError": true
  }
}
```

Always check `isError` before parsing the `text` field as a successful result.

---

## Limits and Constraints

| Constraint | Limit | Notes |
|------------|-------|-------|
| Content size per memory | 64 KB | Enforced at store/update. |
| FTS5 query length | 10 KB | Prevents query abuse. |
| Tags per memory | 1,000 | |
| `top_k` (search results) | 50 max | Default: 10. |
| `list_memories` limit | 200 max | Default: 50. |
| `import_batch` size | 1,000 memories | |
| Concurrent import jobs | 16 | Async imports via `import_directory`. |
| Recursive import depth | 16 levels | |
| `prune_stale` candidates | 200 max per call | |
| `get_entity_graph` depth | 5 max | Default: 2. |
| Edges per entity (displayed) | 100 in/out | |
| Related entities (displayed) | 50 | |
| Stdio line buffer | 1 MB | |
| HTTP request body | 2 MB | |
| Concurrent HTTP sessions | 256 | |
| Embedding dimensions | 768 | Fixed by nomic-embed-text-v1.5. |

---

## Best Practices for AI Agents

### Choosing the Right Search Tool

- **`search_hybrid`** -- Use this by default. It combines semantic, keyword, and graph results with rank fusion. Best overall quality.
- **`search_keyword`** -- Use when searching for exact terms: error codes, function names, specific identifiers, quoted strings.
- **`search_semantic`** -- Use when the user's question uses different words than what's stored. Finds conceptually similar content. Requires an embedding model.
- **`search_temporal`** -- Use when the user asks about "recent" changes or events within a time window.

### Storing Memories Effectively

- **Be specific.** `"PostgreSQL 16 on port 5432, auth via scram-sha-256"` is more useful than `"we use a database"`.
- **Use tags.** Tags enable filtering in `list_memories` and improve keyword search. Use consistent naming: `["database", "postgresql", "infrastructure"]`.
- **Choose the right tier.**
  - `episodic` -- Events, conversations, observations (default). Subject to importance decay.
  - `semantic` -- Consolidated facts, reference knowledge. Created by consolidation or manually.
  - `procedural` -- How-to instructions, runbooks, processes.
- **Set source_type.** Helps filter and attribute memories later: `"conversation"`, `"document"`, `"code"`, `"observation"`.

### Building a Knowledge Graph

- Create entities for recurring concepts: people, services, projects, technologies.
- Add observations as you learn new facts -- they're searchable.
- Create relations to capture dependencies, ownership, and associations.
- Use `get_entity_graph` to explore context around a known entity before answering questions about it.

### Efficient Usage

- Use `search_hybrid` with a reasonable `top_k` (5-10) rather than fetching everything.
- Use `list_memories` with filters (`source_type`, `tier`) and pagination rather than listing all.
- Use `import_batch` for bulk loading instead of calling `store_memory` in a loop.
- For large directory imports, use `async: true` and poll with `get_import_status`.
- Run `consolidate_memories` periodically to promote episodic memories to structured knowledge.

### What Not To Do

- Do not store secrets (API keys, passwords, tokens). The secret scanner will reject them.
- Do not store prompt injection payloads. The injection scanner blocks content scoring >= 7.0.
- The admin tools (`admin_reset_auth`, `export_all_memories`, `debug_raw_query`, `set_system_config`) require elevated privileges and will return errors for standard clients.
- Do not paginate through all memories sequentially without a specific need.
- Be mindful of search frequency -- high-volume automated searching may trigger rate limiting.

---

## All 35 Tools at a Glance

| # | Tool | Category | Description |
|---|------|----------|-------------|
| 1 | `store_memory` | Memory CRUD | Store a new memory |
| 2 | `retrieve_memory` | Memory CRUD | Retrieve by UUID |
| 3 | `update_memory` | Memory CRUD | Update content |
| 4 | `delete_memory` | Memory CRUD | Soft-delete |
| 5 | `search_hybrid` | Search | Vector + keyword + graph with RRF fusion |
| 6 | `search_semantic` | Search | Vector similarity only |
| 7 | `search_keyword` | Search | BM25 keyword only |
| 8 | `search_temporal` | Search | Time-range filter |
| 9 | `create_entity` | Entity Graph | Create entity node |
| 10 | `add_observation` | Entity Graph | Add observation to entity |
| 11 | `create_relation` | Entity Graph | Create directed edge |
| 12 | `search_entities` | Entity Graph | Search entities |
| 13 | `get_entity_graph` | Entity Graph | Traverse entity with edges |
| 14 | `get_history` | Temporal | Entity version history |
| 15 | `get_state_at_time` | Temporal | Entity snapshot at timestamp |
| 16 | `get_changes_since` | Temporal | Change feed since timestamp |
| 17 | `import_batch` | Import | Bulk import array |
| 18 | `import_file` | Import | Import from file |
| 19 | `import_directory` | Import | Import directory (sync or async) |
| 20 | `get_import_status` | Import | Poll async import progress |
| 21 | `consolidate_memories` | Maintenance | Episodic-to-semantic consolidation |
| 22 | `list_memories` | Maintenance | List with filters and pagination |
| 23 | `prune_stale` | Maintenance | Find/remove decayed memories |
| 24 | `health_check` | Monitoring | Daemon health status |
| 25 | `get_memory_stats` | Monitoring | Aggregate counts |
| 26 | `get_hardware_info` | Monitoring | CPU/GPU/SIMD/RAM detection |
| 27 | `get_index_stats` | Monitoring | Per-engine statistics |
| 28 | `rebuild_indexes` | Index Mgmt | Rebuild FTS5 and/or vector index |
| 29 | `admin_reset_auth` | Admin | Reset authentication tokens (requires admin) |
| 30 | `export_all_memories` | Admin | Export memory database (requires admin) |
| 31 | `debug_raw_query` | Admin | Raw database query (requires debug mode) |
| 32 | `set_system_config` | Admin | Update runtime config (requires admin) |
| 33 | `extract_events` | Temporal Events | Parse dates from text, create event entities |
| 34 | `search_events` | Temporal Events | Search entities by event date range |
| 35 | `calculate_duration` | Temporal Events | Compute days between two dates |
