# AGENTS.md - UEMCP Project Instructions

## Core Rule
- Build and debug the project the right way, every time.
- Do not rely on flimsy shortcuts, lazy workarounds, or speculative fixes when a correct path exists.

## Debugging Discipline
When investigating a bug, follow this process:
1. Lock the symptom.
2. Inspect live runtime state.
3. Prove the failing gate.
4. Change exactly one thing.
5. Re-verify.

## Project Pack Boundary
- UEMCP must remain project-neutral and shareable.
- Generic inspection, automation, readiness, asset, Blueprint, map, profile, and capability tools belong in this repo.
- Concrete game paths, sentinel assets, maps, workflow gates, local worktrees, and compatibility expectations belong in the consuming project's UEMCP pack or ignored local profile files.
- When Failstate needs new integration, prefer adding a generic UEMCP capability plus a Failstate-owned pack update.
- Do not add Failstate-specific tool names, paths, maps, asset names, or assumptions to the public MCP surface.

## Profile And Pack Lookup
- Project profiles can be loaded from `UEMCP_PROFILE_DIR/<profile>.json`.
- Local machine profiles can live in ignored `.uemcp.local/profiles/<profile>.json`.
- Packaged profiles under `Python/profiles` are shareable examples and defaults, not machine-specific contracts.
- A project-owned pack should use a path like `Tools/UEMCP/profiles/<profile>.json` and can be passed to scripts through `UEMCP_PROFILE_DIR`.

## Unreal Asset Safety
- Read-only observations are the default.
- Do not perform shell-side edits to `.uasset`, `.umap`, redirectors, or generated Unreal asset metadata.
- If a fix depends on editor-owned asset state, use Unreal Editor workflows or editor-backed UEMCP tools that report structured evidence.

## Session Workflow
- Start each work session with a deliberate planning pass before implementation.
- Use available Codex superpowers proactively for planning, debugging, validation, and verification.
- Define the validation path before implementation.
- End completed work sessions with a scoped git commit unless unrelated user changes make a clean commit boundary impossible.
