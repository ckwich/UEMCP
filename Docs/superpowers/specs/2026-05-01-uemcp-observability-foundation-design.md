# UEMCP Observability Foundation Design

Date: 2026-05-01
Status: Approved direction, initial design
Repo: C:\Dev\UEMCP
Branch: codex/observability-foundation

## Purpose

Fork `ckwich/UEMCP` into a Failstate-focused Unreal Engine MCP where observability is the first product surface, not an afterthought. The server should help an assistant inspect the live Unreal Editor state, prove failing gates, and validate changes before it is trusted to mutate maps, Blueprints, assets, or gameplay data.

The current upstream bridge is a useful prototype: Python MCP over stdio, a C++ Unreal Editor plugin, and a localhost TCP command bridge. This fork should keep that basic shape while replacing "send editor commands and hope" with explicit runtime evidence, bounded observations, and safe validation loops.

## Binding Context

- UEMCP is forked from `https://github.com/ckwich/UEMCP.git`, based on the upstream Unreal MCP prototype.
- Failstate target project: `C:\Dev\Failstate`.
- Active Failstate worktree observed during this design pass: `C:\Dev\Failstate\.worktrees\phase1-combat-shell`.
- Failstate currently targets Unreal Engine 5.7 in `Failstate.uproject`; older planning notes that mention UE 5.5 are stale for this integration.
- The current Failstate slice is Phase 1 Combat Shell, a PvE-first validation layer.
- Editor-owned content stays editor-owned. UEMCP must not perform shell-side `.uasset` or `.umap` surgery.
- The known Blockout path is `Content/Failstate/Blueprints/Blockout`.

## Decision

Build the fork around an observability-first tool contract before adding or re-enabling mutating tools.

Recommended approach: preserve the two-process bridge, but make the initial MCP surface read-mostly, typed, traceable, and validation-oriented. Mutating Unreal operations come later, behind explicit capability flags and after the observation path can prove what changed.

## Alternatives Considered

### Thin Upstream Wrapper

This is the fastest path, but it leaves the risky parts unchanged: broad mutation commands, weak compatibility boundaries, inconsistent tool exposure, little structured error reporting, and too much dependence on reading side effects after the fact.

Rejected for Failstate because it would make debugging slower precisely when the project needs trustworthy editor evidence.

### Full Rewrite

A clean service/plugin rewrite could produce a better architecture, but it would delay usable Unreal integration and throw away the useful prototype shape that already exists.

Rejected for now because the first win should be a reliable observation bridge, not a broad platform rewrite.

### Forked Observability-First Bridge

Keep the Python MCP service, the Unreal Editor plugin, and localhost editor bridge. Replace the first tool surface with structured observations, bounded output, explicit request IDs, and Failstate-aware validation commands.

Accepted because it gives us a practical path to power without trusting unsafe automation too early.

## Architecture

### Python MCP Service

The Python side is the MCP contract layer. It owns tool names, input schemas, response normalization, timeouts, capability checks, and Failstate profile loading. It should not hide Unreal failures behind generic strings.

Each tool response should include:

- `ok`: boolean success marker.
- `request_id`: stable per-call identifier.
- `tool`: MCP tool name.
- `started_at` and `finished_at`: ISO timestamps.
- `duration_ms`: elapsed time.
- `editor`: engine version, project path, plugin version when available.
- `data`: typed result payload.
- `warnings`: bounded list of non-fatal concerns.
- `error`: structured error object when `ok` is false.

### Unreal Editor Plugin

The plugin is the live observation agent inside Unreal Editor. It should expose focused commands over the local bridge, using stable Unreal APIs where possible:

- Output log capture.
- Asset registry search and relationship queries.
- Current editor world, PIE world, map, actor, component, and selection snapshots.
- Blueprint class/default/component inspection.
- Automation test listing and execution.
- Viewport capture.

The plugin should start read-mostly. Editor mutation commands should either be absent or disabled until the observability surface is validated.

### Local Bridge Protocol

The bridge remains localhost-only, but it needs a stricter envelope:

- One command request includes `request_id`, `command`, `params`, `capabilities`, and optional `profile`.
- One command response includes `request_id`, `ok`, `data`, `warnings`, and `error`.
- Errors are categorized, for example `connection_failed`, `timeout`, `editor_not_ready`, `invalid_params`, `asset_not_found`, `automation_failed`, and `internal_error`.
- Large outputs are bounded by explicit `limit`, `since`, `category`, or `path` parameters.

Authentication can stay out of the first implementation slice if the socket is strictly bound to `127.0.0.1`, but the protocol should leave room for a future token or capability handshake.

### Failstate Profile

Failstate-specific knowledge should live in a profile/config layer rather than being hard-coded through the core bridge.

Initial profile fields:

- Project path.
- Preferred worktree path.
- Engine major/minor expectation.
- Content roots to search first.
- Known validation test prefixes, such as `Failstate.Phase1`.
- Known map or asset paths for the current slice.
- Output log categories worth surfacing first.

## Initial Tool Surface

These tools are the first useful contract. They are intentionally observational.

- `uemcp_ping`: prove the MCP server, bridge, plugin, editor, and project identity.
- `get_editor_status`: report engine version, project path, current map, PIE/editor mode, selected actor count, dirty package count when available, and plugin version.
- `get_output_log`: return bounded log lines with optional category, severity, text filter, and time/window limits.
- `asset_search`: query assets by path, class, name, tag, or content root.
- `asset_dependencies`: return read-only asset dependency data from Unreal's asset registry.
- `asset_referencers`: return read-only asset referencer data from Unreal's asset registry.
- `get_level_snapshot`: summarize the current editor or PIE world, including map, actor counts, classes, labels, transforms, and relevant gameplay tags where available.
- `get_selected_actors`: inspect the current editor selection with actor class, path, transform, component summary, and owning level.
- `blueprint_query`: inspect Blueprint identity, parent class, generated class availability, component defaults, exposed variables, and compile status where available.
- `run_automation_tests`: list or run Unreal Automation tests by prefix and return a bounded summary plus log pointers.
- `capture_viewport`: save an explicit screenshot artifact for visual validation.
- `get_failstate_context`: report the active Failstate profile, target project/worktree, known content roots, validation commands, and any profile warnings.

## Safety Rules

- Read-only observations are the default.
- Any tool that changes Unreal state must be absent or disabled until the observability contract is working.
- No arbitrary Python execution, Unreal console command execution, or generic "run script in editor" tool in the first slice.
- No shell-side edits to `.uasset`, `.umap`, or generated Unreal asset metadata.
- Any future mutating tool must declare the asset/map/package it intends to change before it runs and report what Unreal says changed after it runs.
- Large data surfaces must be bounded, filtered, and resumable.
- Errors must be structured enough for an assistant to stop at the failing gate instead of guessing.

## Observability Gates

The foundation is only useful if these gates can be proven directly:

1. MCP server imports and lists tools in the pinned Python environment.
2. Unreal plugin compiles against Failstate's installed UE 5.7 toolchain.
3. Editor launches the Failstate worktree with the plugin enabled.
4. `uemcp_ping` returns plugin version, engine version, project path, current map, and bridge latency.
5. `get_output_log` returns bounded output without freezing or dumping the whole editor log.
6. `asset_search` can find the Blockout content root in `Content/Failstate/Blueprints/Blockout`.
7. `asset_dependencies` and `asset_referencers` return read-only registry data for a known Blockout asset.
8. `get_level_snapshot` distinguishes editor world from PIE world when PIE is running.
9. `blueprint_query` reports parent class and generated class status for a known Failstate Blueprint.
10. `run_automation_tests` can list or invoke `Failstate.Phase1` tests and return a structured pass/fail summary.
11. `capture_viewport` writes a screenshot to an explicit artifact path and reports that path.

## First Implementation Slice

The next commit after this design should stay narrow:

1. Rename/rebrand enough package metadata to establish UEMCP ownership without broad churn.
2. Pin or constrain Python dependencies so the MCP server imports predictably.
3. Add shared response envelope helpers on the Python side.
4. Add `uemcp_ping` and `get_editor_status`.
5. Add bounded `get_output_log`.
6. Add a Failstate profile file with project/worktree/content-root/test-prefix defaults.
7. Add smoke validation for Python import/tool listing.

Unreal-side implementation should favor stable UE 5.7 editor APIs and compile proof over speculative compatibility claims.

## Non-Goals For The First Slice

- Blueprint graph creation or mutation.
- Actor spawning, movement, deletion, or map construction.
- Data Asset editing.
- Gameplay Ability System authoring.
- Multiplayer authority tooling.
- Packaging or build farm integration.
- Runtime game server control.
- General-purpose editor scripting.

## Closeout Standard

Every completed slice should end with:

- Cleanly scoped git commit.
- Exact validation commands and results.
- If Engram MCP write tools are available, a concise memory entry with repo path, branch, commit, files changed, validation, and next step.
- If Engram write tools are unavailable, a copy/paste-ready Engram entry in the final response.
