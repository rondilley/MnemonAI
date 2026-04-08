# How To: Configure Codex to Use the Memory MCP (mnemond)

This guide shows the correct way to wire `mnemond` into Codex so it is available in every session and used with a consistent memory policy.

For Codex, the setup has two parts:

1. Register `mnemond` as an MCP server in `~/.codex/config.toml`
2. Add always-on memory usage instructions in `developer_instructions` in the same file

That is the right global setup for Codex. A skill is not the right mechanism here because skills are trigger-based, while `developer_instructions` are global and always loaded.

---

## What to Configure

Codex needs two separate pieces of configuration:

- **Connectivity**: tell Codex where the `mnemond` MCP server lives
- **Behavior**: tell Codex when and how to use the memory tools well

Both are configured in:

`C:\Users\YOUR_USER\.codex\config.toml`

---

## Step 1: Register the MCP Server

Add an `mcp_servers.mnemond` entry to `~/.codex/config.toml`:

```toml
[mcp_servers.mnemond]
url = "http://your-mnemond-host:3847/mcp"
```

Notes:

- `mnemond` is the server name Codex uses internally
- The endpoint is HTTP on port `3847`
- The MCP path is `/mcp`

---

## Step 2: Add Global Memory Instructions

The MCP registration only gives Codex access to the tools. It does not tell Codex when to search memory, when to store memory, or which search mode to prefer.

For always-on behavior, add a `developer_instructions` block to `~/.codex/config.toml`.

This is the correct place for a global memory policy because it augments Codex's built-in instructions without replacing them.

### Recommended `developer_instructions` block

```toml
developer_instructions = """
## Memory (mnemond)

Use the `mnemond` MCP tools to preserve durable context across sessions and avoid re-discovering project knowledge.

### Session-start behavior
- Before substantial exploration or implementation, run `mcp__mnemond__search_hybrid` with a query relevant to the current task, repo, feature, bug, or project.
- Review the results before proceeding.
- If the task is clearly project-specific, include the project name in the query.
- If the user is continuing prior work, retrieving context from memory is mandatory before making changes.

### When to retrieve memories
- Before making architectural, design, or workflow decisions
- When the user references prior work or asks to continue something
- When unfamiliar project conventions, terminology, acronyms, or standards appear
- When prior debugging history or known failures could affect the current task
- When memory context would materially improve correctness or speed

### When to store memories
- Architectural decisions and their rationale
- User preferences, coding conventions, workflow rules, and project standards
- Significant bugs, failures, or debugging outcomes, including root cause and fix
- Project-specific terminology, acronyms, and definitions
- Important relationships between systems, components, people, or entities
- Meaningful task outcomes and lessons learned that will help future sessions

### Tool selection
- `mcp__mnemond__search_hybrid`: default retrieval mode
- `mcp__mnemond__search_keyword`: use for exact identifiers
- `mcp__mnemond__search_semantic`: use when searching by meaning
- `mcp__mnemond__search_entities`: use when looking up entities
- `mcp__mnemond__get_entity_graph`: use for relationships
- `mcp__mnemond__retrieve_memory`: use for a specific memory ID
- `mcp__mnemond__store_memory`: use to persist new knowledge
- `mcp__mnemond__search_events`: search by event dates in content (not storage time)
- `mcp__mnemond__calculate_duration`: compute days between two dates (always use this instead of calculating yourself)
- `mcp__mnemond__extract_events`: parse dates from text and create event entities

### Memory tiers
- `episodic`: events, debugging sessions, task progress
- `semantic`: durable facts, conventions, preferences, definitions
- `procedural`: repeatable workflows and SOPs

### Tagging conventions
- Always include 2-5 descriptive tags
- Use consistent tags for project, technology, and category
- Include the project name as a tag for project-specific memories

### What not to store
- Temporary paths, scratch values, or one-off calculations
- Secrets, credentials, tokens, or API keys
- Large verbatim file contents; store summaries instead
"""
```

---

## Why `developer_instructions` Is the Right Place

Use `developer_instructions` for this policy because:

- it applies globally to all Codex sessions
- it is additive and preserves the built-in Codex prompt
- it is the right level for durable behavioral guidance such as memory usage rules

Do not use a skill for this. Skills are not reliably always-on.

Do not use `model_instructions_file` unless you intentionally want to replace Codex's built-in instruction file with your own full prompt stack.

---

## Example Complete Config

A complete working Codex configuration looks like this:

```toml
model = "gpt-5.4"
model_reasoning_effort = "high"
personality = "pragmatic"

developer_instructions = """
## Memory (mnemond)

Use the `mnemond` MCP tools to preserve durable context across sessions and avoid re-discovering project knowledge.

### Session-start behavior
- Before substantial exploration or implementation, run `mcp__mnemond__search_hybrid` with a query relevant to the current task, repo, feature, bug, or project.
- Review the results before proceeding.
- If the task is clearly project-specific, include the project name in the query.
- If the user is continuing prior work, retrieving context from memory is mandatory before making changes.

### When to retrieve memories
- Before making architectural, design, or workflow decisions
- When the user references prior work or asks to continue something
- When unfamiliar project conventions, terminology, acronyms, or standards appear
- When prior debugging history or known failures could affect the current task
- When memory context would materially improve correctness or speed

### When to store memories
- Architectural decisions and their rationale
- User preferences, coding conventions, workflow rules, and project standards
- Significant bugs, failures, or debugging outcomes, including root cause and fix
- Project-specific terminology, acronyms, and definitions
- Important relationships between systems, components, people, or entities
- Meaningful task outcomes and lessons learned that will help future sessions

### Tool selection
- `mcp__mnemond__search_hybrid`: default retrieval mode
- `mcp__mnemond__search_keyword`: use for exact identifiers
- `mcp__mnemond__search_semantic`: use when searching by meaning
- `mcp__mnemond__search_entities`: use when looking up entities
- `mcp__mnemond__get_entity_graph`: use for relationships
- `mcp__mnemond__retrieve_memory`: use for a specific memory ID
- `mcp__mnemond__store_memory`: use to persist new knowledge
- `mcp__mnemond__search_events`: search by event dates in content (not storage time)
- `mcp__mnemond__calculate_duration`: compute days between two dates (always use this instead of calculating yourself)
- `mcp__mnemond__extract_events`: parse dates from text and create event entities

### Memory tiers
- `episodic`: events, debugging sessions, task progress
- `semantic`: durable facts, conventions, preferences, definitions
- `procedural`: repeatable workflows and SOPs

### Tagging conventions
- Always include 2-5 descriptive tags
- Use consistent tags for project, technology, and category
- Include the project name as a tag for project-specific memories

### What not to store
- Temporary paths, scratch values, or one-off calculations
- Secrets, credentials, tokens, or API keys
- Large verbatim file contents; store summaries instead
"""

[windows]
sandbox = "elevated"

[mcp_servers.mnemond]
url = "http://your-mnemond-host:3847/mcp"
```

Your config may also contain trusted-project entries and other settings, but the above is all that is needed for `mnemond`.

---

## Step 3: Restart Codex

After editing `~/.codex/config.toml`, start a new Codex session.

The new session will pick up:

- the `mnemond` MCP registration
- the global memory behavior in `developer_instructions`

---

## Step 4: Verify the Setup

There are two things to verify:

1. Codex can see the `mnemond` server
2. Codex actually uses it during work

### Basic verification prompts

Start a new Codex session and ask:

```text
list tools available from mnemond
```

or:

```text
run health_check on mnemond
```

or:

```text
show me mnemond memory stats
```

If the wiring is correct, Codex should call the corresponding `mnemond` MCP tools rather than answer from guesswork.

### Behavior verification prompts

To confirm the global instructions are working, ask Codex to continue work on a known repo or feature:

```text
continue work on podcastorum
```

If the policy is loaded, Codex should search memory first before diving into implementation.

---

## Codex-Specific Tool Naming

In Codex, MCP tools are namespaced. For `mnemond`, that means the tools appear as:

- `mcp__mnemond__search_hybrid`
- `mcp__mnemond__search_keyword`
- `mcp__mnemond__search_semantic`
- `mcp__mnemond__search_entities`
- `mcp__mnemond__get_entity_graph`
- `mcp__mnemond__retrieve_memory`
- `mcp__mnemond__store_memory`

That is why the `developer_instructions` block above uses the fully namespaced Codex tool names instead of the shorter Claude-style names.

---

## Optional CLI Commands

Codex CLI exposes MCP management commands:

```bash
codex mcp --help
codex mcp list
codex mcp get mnemond
```

These are useful for inspection and troubleshooting, but the most important configuration still lives in `~/.codex/config.toml`.

---

## Troubleshooting

### Codex does not use memory even though `mnemond` is connected

Usually this means the MCP server is registered but there is no strong instruction telling Codex when to use it. Add or fix the `developer_instructions` block.

### Codex can use `mnemond`, but not globally

This usually means the policy was placed in a skill or a project-only instruction file. Move it to `developer_instructions` in `~/.codex/config.toml`.

### `mnemond` is registered, but tool calls fail

Check:

- the `url` value
- that the `mnemond` server is running
- that the endpoint is reachable from the machine running Codex

### The current session does not seem to pick up new config

Restart Codex. Config changes are safest to verify in a fresh session.

---

## Summary

For Codex, the correct global setup is:

1. register `mnemond` under `[mcp_servers.mnemond]` in `~/.codex/config.toml`
2. place your memory policy in `developer_instructions` in the same file
3. restart Codex and verify with a health or stats request

That is the cleanest and most correct way to make `mnemond` available everywhere and actually used.
