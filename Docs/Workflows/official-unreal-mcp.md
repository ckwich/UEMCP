# UEMCP and Epic's Official Unreal MCP

Unreal Engine 5.8 ships an experimental, first-party MCP server ("Unreal MCP"),
built on the Toolset Registry plugin. This page explains how UEMCP relates to it,
what UEMCP adopted from its design, and how to run both servers side by side.

Status as of 2026-10: Epic's server is marked Experimental, and its APIs and data
formats may change. Re-check this page against the engine release you are using.

## What Epic's server is

- **Plugins:** `Unreal MCP` plus `All Toolsets`. Toolset Registry comes with them as a dependency.
- **Transport:** Streamable HTTP and SSE only, at `http://127.0.0.1:8000/mcp` by
  default. There is no stdio and no authentication, and the server binds to loopback only.
- **Start:** use Editor Preferences > General > Model Context Protocol > Auto Start
  Server, or run `ModelContextProtocol.StartServer [port]` in the console.
- **Other console commands:** `ModelContextProtocol.StopServer`,
  `ModelContextProtocol.RefreshTools`, and `ModelContextProtocol.GenerateClientConfig <client>`.
- **Tool Search mode** (on by default): exposes `list_toolsets`, `describe_toolset`,
  and `call_tool` instead of the full tool list.
- **Built-in coverage:** spawning and editing actors, lighting, material instances,
  Slate inspection, and automation tests.
- **Extension:** Python classes derived from `unreal.ToolsetDefinition` with
  `@toolset_registry.tool_call` static methods, registered at startup through
  `toolset_registry.registration.Registration([...]).register()` in `init_unreal.py`.
  In C++, use `UToolsetDefinition` subclasses with AI-callable static `UFUNCTION`s.
  Type hints and docstrings become the schema.
- **Execution:** calls run serially on the game thread. MCP calls are not wrapped in
  Unreal transactions automatically. A new C++ `UFUNCTION` needs an editor restart.

## Where each one is stronger

| Area | Epic Unreal MCP | UEMCP |
|---|---|---|
| Maintenance | Epic ships it, and it tracks engine releases | Maintained by us |
| Engine versions | 5.8+ | 5.5+ |
| Transport | In-editor HTTP, no Python process | Python stdio server plus a C++ TCP bridge |
| Readiness and observability | Not a focus | `get_editor_readiness`, output log capture, level and PIE snapshots, event history |
| Safety classification | One `call_tool` covers every operation | Per-tool safety classes, MCP annotations, split dispatchers |
| Asset intake and level construction | Not covered | Snapshot/diff/manifest intake, construction plans, validation gates |
| Project packs | Not covered | Profile and pack boundary (`UEMCP_PROFILE_DIR`) |
| Breadth of basic editor control | Growing every release | Fixed; we should stop expanding it |

The conclusion: **do not compete on basic editor control.** UEMCP's value lies in
the workflow, safety, and observability layer.

## What UEMCP adopted (this change)

1. **MCP tool annotations from the safety manifest.** Every tool now advertises
   `readOnlyHint` and `destructiveHint` (along with `idempotentHint` and
   `openWorldHint`), derived from `Scripts/UEMCPToolSurface.Audit.json`. A tool
   with no manifest entry fails closed and is marked destructive. Clients that
   honour annotations can auto-approve the 29 read-only tools and keep prompting
   for the 39 destructive ones.
2. **Catalog mode** (`UEMCP_TOOL_MODE=catalog`). This mirrors Epic's Tool Search to
   cut context from 73 tool schemas down to 4 meta-tools, with one deliberate
   difference: dispatch is split into `call_read_tool` and `call_mutating_tool`.
   Epic's single `call_tool` means an allowlist entry approves everything. With
   the split, "auto-approve reads, prompt for writes" still works. Each dispatcher
   refuses tools from the other class.

The default mode stays `full`, so existing setups behave the same, apart from
gaining annotations.

```json
{
  "mcpServers": {
    "unrealMCP": {
      "command": "uv",
      "args": ["--directory", "Python", "run", "unreal_mcp_server.py"],
      "env": { "UEMCP_TOOL_MODE": "catalog" }
    }
  }
}
```

## Running both servers side by side

The two servers do not conflict. Epic listens on HTTP port 8000 and the UEMCP
bridge on TCP port 55557. In Claude Code:

```bash
claude mcp add --transport http unreal-official http://127.0.0.1:8000/mcp
claude mcp add unreal-uemcp -- uv --directory /path/to/UEMCP/Python run unreal_mcp_server.py
```

Suggested division of labour:

- Use UEMCP for the read-only ladder first: `uemcp_ping`, `get_editor_readiness`,
  then snapshots.
- Use Epic's toolsets for basic edits they cover, such as actors, lights, and
  material instances.
- Use UEMCP workflow tools for asset intake, level construction plans, and
  validation gates.
- Re-verify with UEMCP read-only tools after any edit, whichever server made it.

Both servers execute on the game thread. Do not issue overlapping writes from two
clients or subagents.

## Roadmap: porting UEMCP onto Epic's Toolset Registry

These steps need a UE 5.8 editor to validate. None of them have been verified yet.

1. **Spike (smallest proof):** register one UEMCP read-only capability, such as an
   output-log tail, as a Python `ToolsetDefinition` in a sandbox project. Confirm it
   appears in `list_toolsets` on Epic's server.
2. **Port the observability toolset:** readiness, log capture, and snapshots. These
   are UEMCP's most distinctive tools and are read-only, so they carry the least risk.
3. **Port workflow toolsets:** asset intake and level construction, each with
   `dry_run`, explicit overwrite policy, and `FScopedTransaction` wrapping, so that
   one workflow produces one undo entry.
4. **Decide on the TCP bridge.** If steps 1 to 3 hold up on 5.8, the Python stdio
   server and C++ bridge become a compatibility path for 5.5 to 5.7, and new
   capability lands as toolsets first.
5. **Retire overlap.** Stop adding generic actor and Blueprint editing tools that
   Epic's toolsets cover.

## Sources

- Epic: Unreal MCP in Unreal Editor (UE 5.8 documentation),
  https://dev.epicgames.com/documentation/unreal-engine/unreal-mcp-in-unreal-editor
- Forum: registering Python functions as Unreal MCP tools (UE 5.8),
  https://forums.unrealengine.com/t/how-to-register-python-functions-as-ai-callable-tools-through-unreal-mcp-ue-5-8/2741072
- Community setup notes: Using Unreal MCP in UE 5.8 from Codex,
  https://dev.to/gamedevtoollab/using-unreal-mcp-in-ue-58-from-codex-setup-toolsets-and-safe-editor-automation-4pon
