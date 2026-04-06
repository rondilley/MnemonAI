# How-To: Configuring Gemini with MnemonAI (mnemond)

If you're tired of your AI having the short-term memory of a goldfish on espresso, you need a local memory service. This guide covers how to wire up **MnemonAI** (mnemond) — a high-performance, local-only C11 memory server — to the Gemini CLI.

## Prerequisites

1.  **MnemonAI Binary**: You should have `mnemond.exe` built and available (typically in `C:\Users\YOUR_USER\git\MnemonAI`).
2.  **Gemini CLI**: Installed and working on Windows.

## 1. Configure the MCP Server

Gemini CLI needs to know how to talk to the mnemond daemon. On Windows, your global configuration lives at `C:\Users\YOUR_USER\.gemini\settings.json`.

Add an entry to the `mcpServers` block. We use the alias `remote-service` on this system, which maps to the `mcp_remote-service_*` tool prefix.

### Example: settings.json
```json
{
  "mcpServers": {
    "remote-service": {
      "url": "http://your-mnemond-host:3847/mcp",
      "trust": true
    }
  }
}
```

*Note: The `trust: true` flag is essential if you don't want to click a "Yes" button every time the AI tries to remember something.*

## 2. Automate the Research Phase

To make the memory useful, you shouldn't have to manually tell Gemini to search it. Use a global context hook in `C:\Users\YOUR_USER\.gemini\gemini.md` to force a memory lookup at the start of every session.

### Example: gemini.md
```markdown
# Persistent Memory & Knowledge Graph (mnemond)

Use the remote memory service to maintain context across sessions and build a knowledge graph of projects, decisions, and conventions.

## Mandatory Workflow
Always do at session start:
- During the Research phase, run mcp_remote-service_search_hybrid for context relevant to the current task or project.
- Review results before proceeding to avoid re-discovering known information or duplicating effort.

## What to Store
- Architectural decisions: Document the "why" behind changes.
- Significant errors: Log root causes and resolutions.
- Project-specific terminology and acronyms.
```

## 3. Verify the Integration

Once configured, restart your Gemini CLI session. You can verify the server is active by running:

```powershell
# List active MCP servers
gemini mcp list

# Check if the tools are available in a session
/tools
```

You should see tools prefixed with `mcp_remote-service_`, such as `mcp_remote-service_search_hybrid` and `mcp_remote-service_store_memory`.

## Troubleshooting

- **Pathing**: Ensure you use double backslashes (`\\`) in the JSON config.
- **Port Conflicts**: If running multiple instances, ensure you specify unique ports or use the default stdio transport.
- **Model Loading**: mnemond requires a GGUF embedding model. If it fails to start, check the `mnemond` logs or run the command manually in a shell to see the stderr output.

---
*Editorial note: Cloud-based "memory" is just someone else's database. mnemond keeps your thoughts on your own NVMe where they belong.*
