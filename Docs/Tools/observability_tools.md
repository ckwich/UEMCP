# Observability Tools

These tools are the first UEMCP surface. They are read-mostly and designed to prove the live Unreal Editor state before any mutating automation runs.

For the canonical Phase 1 agent workflow and local profile boundary, see [Phase 1 Observability Workflow](../Workflows/phase-1-observability.md).

All observability tools return the same envelope shape:

- `ok`: true when the tool completed.
- `tool`: MCP tool name.
- `request_id`: unique call identifier.
- `started_at`, `finished_at`, `duration_ms`: timing evidence.
- `editor`: editor identity fields when available.
- `data`: typed payload for the tool.
- `warnings`: bounded non-fatal warnings.
- `error`: structured error with `category`, `message`, and `raw` when `ok` is false.

## uemcp_ping

Checks whether the Python MCP server can reach the Unreal Editor bridge. On success, returns server metadata and bridge identity.

## get_editor_status

Returns Unreal Editor identity and state:

- Plugin version.
- Engine version.
- Project name and path.
- Current map.
- PIE running state.
- Editor slow-task state.
- Selected actor count.
- Dirty package count.

## get_editor_readiness

Returns whether the editor is ready for automation, optionally waiting for consecutive ready samples. This is a read-only Python composition over `get_editor_status`; it does not add a new Unreal command.

Parameters:

- `timeout_seconds`: optional wait budget, clamped from 0 to 300 seconds. `0` takes a single snapshot.
- `stable_samples`: number of consecutive ready samples required, clamped from 1 to 10.
- `poll_interval_seconds`: delay between status samples when waiting, clamped from 0 to 10 seconds.
- `settle_seconds`: optional duration the editor must remain ready after the first ready sample, clamped from 0 to 120 seconds.

The response includes:

- `ready`: true when the latest samples show no blocking editor state.
- `state`: `ready`, `blocked`, or `timeout`.
- `blocking_reasons`: currently `editor_slow_task_active`, `play_in_editor_running`, or `editor_status_unavailable`.
- `latest_status`: the latest `get_editor_status` payload.
- `samples`: the most recent bounded readiness samples, each including status request id, current map, PIE state, slow-task state, and blocking reasons.

## diagnose_editor_automation_readiness

Runs a read-only diagnostic pass before any automation test is started. This is a Python composition over `uemcp_ping`, `get_editor_status`, `get_editor_readiness`, and optionally `get_output_log`.

Parameters:

- `readiness_timeout_seconds`: optional readiness wait budget, clamped from 0 to 300 seconds. `0` takes a single snapshot.
- `readiness_stable_samples`: consecutive ready samples required by the readiness gate, clamped from 1 to 10.
- `readiness_poll_interval_seconds`: delay between readiness samples when waiting, clamped from 0 to 10 seconds.
- `readiness_settle_seconds`: optional duration the editor must remain ready after the first ready sample, clamped from 0 to 120 seconds.
- `output_log_limit`: output log sample size, clamped from 0 to 1000. `0` skips the output-log gate.

The response includes:

- `ready_for_automation`: true only when the bridge, status, readiness, and output-log gates all pass.
- `summary`: compact `state`, `first_blocking_category`, and `first_blocking_message`.
- `gates`: per-gate status for `bridge`, `status`, `readiness`, and `output_log`.
- `readiness`: compact readiness evidence using the same shape as profile automation readiness.
- `observability_events`: normalized events when the diagnostic finds a blocking condition. Successful diagnostics return an empty list.
- `evidence_refs`: request ids for the ping, status, readiness, and output-log checks.

This tool does not list or run automation tests. Use it as the cheapest first check when an agent needs to decide whether automation is safe to start.

## get_observability_recent_events

Returns the bounded in-process history of high-level observability results without querying Unreal Editor. This is a Python-only read of the current MCP server process history; it is not persisted across server restarts.

The history records:

- `get_editor_readiness`
- `diagnose_editor_automation_readiness`
- `run_profile_automation_tests`
- `run_project_compatibility_gates`

Parameters:

- `tool`: optional tool-name filter.
- `limit`: maximum entries, clamped from 1 to 100.
- `include_success`: when false, returns only entries whose compact result was not successful.
- `newest_first`: when true, default, returns newest entries first.

Each entry includes `sequence`, `recorded_at`, `tool`, `request_id`, `ok`, compact `successful`, optional `failure_category`, readable `message`, compact `summary`, `observability_events`, `evidence_refs`, `editor`, and `warnings`.

This tool intentionally does not record its own reads. Use it when an agent needs to inspect the last readiness, diagnostic, or profile-run outcome without rerunning checks or starting automation.

## summarize_observability_state

Returns the latest actionable state derived from the same in-process history used by `get_observability_recent_events`. It does not query Unreal Editor and does not record its own reads.

Parameters:

- `tool`: optional tool-name filter before summarizing, such as `run_profile_automation_tests`.
- `limit`: maximum recent history entries to inspect, clamped from 1 to 100.

The response includes:

- `state`: `unknown` when no matching history exists, `ready` when the newest matching entry is successful, `blocked` when the newest matching entry is a readiness/diagnostic/preflight blocker, or `failing` when the newest matching entry is an automation result failure.
- `message`: readable latest state or blocker message.
- `latest_entry`: newest matching history entry.
- `latest_blocker`: newest matching entry when it is currently unsuccessful; stale failures are ignored after a newer success.
- `latest_success`: newest successful matching entry.
- `recommended_next_step`: a compact suggested tool/reason for `unknown`, `blocked`, or `failing` states.
- `counts`, `history_capacity`, and `filters`.

Use this as the shortest agent-facing answer to "what is blocking automation right now?" after at least one readiness diagnostic or profile run has populated history.

## get_output_log

Requests bounded output log data from the editor bridge. The Unreal plugin registers a thread-safe in-memory `GLog` output device during module startup and keeps the newest 2048 entries available for MCP queries.

Parameters:

- `limit`: maximum entries, clamped from 1 to 1000.
- `category`: optional Unreal log category filter.
- `verbosity`: optional verbosity filter.
- `contains`: optional substring filter.

The response includes:

- `entries`: matching log entries in chronological order, bounded by `limit`.
- `truncated`: true when more matching entries exist than were returned.
- `buffer_capacity`: current Unreal-side ring buffer capacity.
- `captured_entry_count`: total entries currently retained in the ring buffer.
- `matched_entry_count`: number of retained entries matching the requested filters before `limit` was applied.
- `filters`: the normalized filters used by the bridge.
- `warnings`: bounded non-fatal warnings.

Each entry includes `sequence`, `time_seconds`, `timestamp_utc`, `category`, `verbosity`, and `message`.

## get_level_snapshot

Returns a bounded read-only snapshot of actors in the current editor world. Use it after `get_editor_status` has proven the expected map is loaded and before project-specific automation needs evidence that level content is present.

Parameters:

- `limit`: maximum actors returned, clamped from 1 to 1000.
- `class_name`: optional actor class or class-path substring filter.
- `name_contains`: optional actor name or editor-label substring filter.
- `include_components`: when true, include bounded component identity for each returned actor.
- `component_limit`: maximum components per actor when components are included, clamped from 1 to 1000.

The response includes:

- `current_map`, `is_pie_running`, and `selected_actor_count`.
- `world`: editor-world name, current map, world type, and path.
- `total_actor_count`: valid actors in the editor world before filters.
- `matched_actor_count`: actors matching the class/name filters before `limit`.
- `returned_actor_count`: actors included in the returned array.
- `truncated`: true when more matching actors exist than were returned.
- `filters`: normalized filters used by the bridge.
- `actors`: bounded actor entries.

Each actor includes `name`, editor `label`, `class`, `class_path`, object `path`, `level_name`, `hidden`, `location`, `rotation`, and `scale`. When component inclusion is enabled, the actor also reports component counts and bounded component identity.

## asset_search

Searches Unreal Asset Registry metadata on the game thread without loading, saving, or mutating assets. Use this as the first Phase 2 content-observation gate before dependency, referencer, Blueprint, or level-inspection tools.

Parameters:

- `root`: optional content root. Defaults to `/Game`. Accepts Unreal package roots such as `/Game/Failstate`, repo-style content paths such as `Content/Failstate/Blueprints/Blockout`, and absolute paths containing `/Content/`.
- `class_name`: optional asset class filter. Matching accepts either the short class name such as `Blueprint` or the full class path such as `/Script/Engine.Blueprint`.
- `name_contains`: optional asset-name substring filter.
- `path_contains`: optional object-path or package-name substring filter.
- `limit`: maximum assets, clamped from 1 to 1000.

The response includes:

- `assets`: matching assets, sorted by object path and bounded by `limit`.
- `total_asset_count`: assets returned by the Asset Registry root filter before text/class filtering.
- `matched_asset_count`: assets matching all filters before `limit`.
- `returned_asset_count`: assets returned.
- `truncated`: true when more matching assets exist than were returned.
- `asset_registry_loading`: true when the Asset Registry reports it is still loading and results may be incomplete.
- `filters`: normalized query, including the Unreal package path actually used by the bridge.
- `warnings`: bounded non-fatal warnings.

Each asset includes `asset_name`, `object_path`, `package_name`, `package_path`, `asset_class`, and `asset_class_path`.

For Failstate Blockout validation:

```python
build_asset_search(
    root="Content/Failstate/Blueprints/Blockout",
    name_contains="BP_FSBlockout",
    limit=50,
)
```

## asset_dependencies

Returns package dependency data for one asset from Unreal's Asset Registry without loading, saving, or mutating the asset.

Parameters:

- `asset_path`: required asset object path, package path, repo-style `Content/...` path, or absolute content path. Object paths such as `/Game/Foo/Bar.Bar` normalize to package name `/Game/Foo/Bar`.
- `include_hard`: include hard package dependencies. Defaults to true.
- `include_soft`: include soft package dependencies. Defaults to true.
- `limit`: maximum relationships, clamped from 1 to 1000.

The response includes:

- `asset_path`: original request value.
- `package_name`: normalized package name used for the Asset Registry query.
- `asset_found` and `source_asset`: whether registry metadata was found for the requested package.
- `dependencies`: dependency entries, sorted and bounded by `limit`.
- `matched_dependency_count`, `returned_dependency_count`, and `truncated`.
- `query_succeeded`, `asset_registry_loading`, `filters`, and `warnings`.

Each dependency includes `identifier`, optional `package_name`, `package_path`, `object_name`, `value_name`, `primary_asset_id`, `category`, `properties`, `hard`, `soft`, `game`, `editor_only`, and related asset metadata when the target package resolves to Asset Registry asset data.

For Failstate Blockout validation:

```python
build_asset_dependencies(
    asset_path="/Game/Failstate/Blueprints/Blockout/BP_FSBlockoutCover",
    include_hard=True,
    include_soft=True,
    limit=50,
)
```

## asset_referencers

Returns package referencer data for one asset from Unreal's Asset Registry without loading, saving, or mutating the asset. It accepts the same parameters and returns the same normalized source fields as `asset_dependencies`, but the relationship array is `referencers` with `matched_referencer_count` and `returned_referencer_count`.

For Failstate Blockout validation:

```python
build_asset_referencers(
    asset_path="/Game/Failstate/Blueprints/Blockout/BP_FSBlockoutCover",
    include_hard=True,
    include_soft=True,
    limit=50,
)
```

## blueprint_query

Loads one Blueprint through Unreal's asset system and reports stored Blueprint metadata without compiling, saving, or mutating the asset. Use this after `asset_search` and relationship checks when an agent needs class identity, generated-class status, exposed variables, or simple component template evidence.

Parameters:

- `asset_path`: required Blueprint object path, package path, repo-style `Content/...` path, or absolute content path. Object paths such as `/Game/Foo/Bar.Bar` normalize to package name `/Game/Foo/Bar`.
- `include_variables`: include `NewVariables` metadata. Defaults to true.
- `include_components`: include Simple Construction Script component node metadata. Defaults to true.
- `variable_limit`: maximum variables returned, clamped from 1 to 1000.
- `component_limit`: maximum component nodes returned, clamped from 1 to 1000.

The response includes:

- `asset_path`, `package_name`, `object_path`, `asset_found`, `source_asset_count`, and `source_asset`.
- `blueprint_name`, `blueprint_type`, `status`, `is_up_to_date`, `blueprint_category`, and `blueprint_description`.
- `parent_class`, `generated_class`, and `skeleton_class`, each with existence, name, path, class path, native flag, and super-class metadata where available.
- `variables`, `variable_count`, `returned_variable_count`, and `variables_truncated`.
- `components`, `component_count`, `returned_component_count`, and `components_truncated`.
- `asset_registry_loading`, `filters`, and `warnings`.

Variable entries include name, guid, pin type, friendly name, category, property flags, replication metadata, default value, common boolean flag projections, and metadata key/value pairs. Component entries include SCS variable name, attach/parent metadata, child count, component class, and component template identity when Unreal reports one.

For Failstate Blockout validation:

```python
build_blueprint_query(
    asset_path="/Game/Failstate/Blueprints/Blockout/BP_FSBlockoutCover",
    include_variables=True,
    include_components=True,
    variable_limit=50,
    component_limit=50,
)
```

## list_automation_tests

Lists Unreal automation tests currently visible to the editor automation framework. The bridge calls `FAutomationTestFramework::LoadTestModules()` and `GetValidTestNames()` on the game thread, then returns a bounded, prefix-filtered result.

Parameters:

- `prefix`: optional dot-path prefix filter. Matching checks both `full_test_path` and Unreal's runnable `test_name`.
- `limit`: maximum tests, clamped from 1 to 1000.

The response includes:

- `tests`: matching tests, bounded by `limit`.
- `total_valid_test_count`: all valid tests reported by Unreal before filtering.
- `matched_test_count`: number of tests matching `prefix` before `limit`.
- `returned_test_count`: number of tests returned.
- `truncated`: true when more matching tests exist than were returned.
- `filters`: the normalized query.

Each test includes `display_name`, `full_test_path`, `test_name`, `tags`, `test_parameter`, `source_file`, `source_line`, `asset_path`, `open_command`, `flags`, and `participants_required`.

For Failstate, the current profile prefix is `Failstate.Phase1`.

## run_automation_test

Runs one exact Unreal automation test and returns structured execution evidence. The command accepts either the listed `full_test_path` or `test_name`; internally the bridge resolves the test through `GetValidTestNames()` and runs the framework command name.

Parameters:

- `test_name`: exact `full_test_path` or `test_name` from `list_automation_tests`.
- `timeout_seconds`: maximum latent-command drain time, clamped from 1 to 120 seconds.

The response includes:

- `test`: the resolved automation test metadata.
- `status`: `passed`, `failed`, or `timed_out`.
- `successful`: true only when the framework reports success and the timeout was not hit.
- `timed_out`: true when the latent-command drain exceeded `timeout_seconds`.
- `duration_seconds` and `reported_duration_seconds`.
- `error_count`, `warning_count`, `event_count`, `events_truncated`, and bounded `events`.

The bridge refuses to start automation tests while an editor slow task or Play In Editor session is active. This avoids Unreal's `StopTest()` assertion path and makes the failed gate explicit. `get_editor_status` exposes those readiness gates as `is_slow_task_active` and `is_pie_running`.

The plugin ships a deterministic smoke test named `UEMCP.Observability.Smoke`; the live smoke script lists and runs it before reporting success.

## run_profile_automation_tests

Runs either one exact automation test or a bounded batch discovered from a profile automation prefix, then returns a compact summary with readiness and output-log tail evidence.

Parameters:

- `profile_name`: profile used for default automation prefixes; defaults to `failstate`.
- `test_name`: optional exact `full_test_path` or `test_name`. When present, the tool runs single-test mode and skips discovery.
- `prefix`: optional automation prefix for batch mode. When omitted, the first profile `automation_test_prefixes` entry is used.
- `limit`: maximum discovered tests to run in batch mode, clamped from 1 to 50.
- `timeout_seconds`: per-test timeout, clamped from 1 to 120 seconds.
- `output_log_limit`: number of output log entries to include after the run, clamped from 0 to 1000.
- `require_ready`: when true, default, run `get_editor_readiness` before discovery or execution and fail with `editor_busy` if the editor is not ready.
- `readiness_timeout_seconds`: optional readiness wait budget, clamped from 0 to 300 seconds. `0` takes a single snapshot.
- `readiness_stable_samples`: consecutive ready samples required by the readiness gate, clamped from 1 to 10.
- `readiness_poll_interval_seconds`: delay between readiness samples when waiting, clamped from 0 to 10 seconds.
- `readiness_settle_seconds`: optional duration the editor must remain ready after the first ready sample, clamped from 0 to 120 seconds.

The response includes:

- `mode`: `single` or `prefix`.
- `readiness`: compact readiness gate evidence, including readiness request id, ready state, blocking reasons, latest status, and bounded samples.
- `observability_events`: normalized high-level events emitted by the runner. Successful runs currently return an empty list.
- `summary`: total, passed, failed, errors, timed_out, and successful counts.
- `tests`: compact per-test results with runnable names, status, duration, counts, bounded event snippets, and request ids.
- `discovery`: batch-mode discovery evidence, including matched/returned counts and prefix filters.
- `output_log_tail`: bounded Unreal output-log entries captured after the run.
- `evidence_refs`: request ids linking the readiness, discovery, run, and output-log envelopes.

If `require_ready` is true and the readiness gate reports `editor_slow_task_active`, `play_in_editor_running`, or `editor_status_unavailable`, the tool returns `ok: false` before discovery or automation execution. The error raw payload keeps the compact readiness evidence and adds:

- `failure_category`: normalized category for routing, such as `editor_busy`, `connection_failed`, `unsupported_command`, `timeout`, or `internal_error`.
- `observability_events`: one `readiness_gate_failed` event with `phase`, `severity`, `failure_category`, `state`, `blocking_reasons`, readable `message`, readiness request id, latest status request id, status error category, and evidence refs.

This keeps the failed gate attached to the high-level command instead of relying on every caller to run a manual preflight or infer the root cause from nested readiness samples.

This tool is intentionally a Python composition of `get_editor_readiness`, `list_automation_tests`, `run_automation_test`, and `get_output_log`; it does not add a new Unreal mutation path.

## run_project_compatibility_gates

Runs the active profile's project-owned compatibility gates against the live editor. This is the generic Phase 5 project-pack gate: UEMCP supplies the runner, while the consuming project supplies sentinel paths, assets, Blueprint identities, and automation prefixes in its profile.

Parameters:

- `profile_name`: profile to load. Defaults to `failstate` for compatibility.
- `include_automation`: when true, default, run automation prefix gates from `compatibility_gates.automation_prefixes` or top-level `automation_test_prefixes`.
- `limit`: default maximum automation tests per prefix, clamped from 1 to 50.
- `timeout_seconds`: per-test automation timeout, clamped from 1 to 120 seconds.
- `output_log_limit`: output log entries attached to each automation prefix run, clamped from 0 to 1000.
- `require_ready`: when true, default, run one readiness preflight before all project compatibility gates.
- `readiness_timeout_seconds`, `readiness_stable_samples`, `readiness_poll_interval_seconds`, `readiness_settle_seconds`: same readiness semantics as `run_profile_automation_tests`.

The response includes:

- `profile_name` and `profile_source`, so callers can prove whether the profile came from `UEMCP_PROFILE_DIR`, `.uemcp.local`, or packaged defaults.
- `readiness`: one compact preflight gate for the whole project pack when `require_ready` is true.
- `summary`: total, passed, failed, and successful counts across all compatibility gates.
- `gates`: compact per-gate evidence. Current generic gate kinds are `sentinel_map`, `sentinel_level_snapshot`, `sentinel_asset_search`, `sentinel_blueprint`, and `automation_prefix`.
- `observability_events`: one `compatibility_gate_failed` event per failed gate. Successful runs return an empty list.
- `evidence_refs`: request ids for readiness and each live gate.

Supported profile schema under `compatibility_gates`:

```json
{
  "sentinel_maps": [
    {
      "name": "current_map_ready",
      "expected_maps": ["/Game/Project/Maps/L_Test"]
    }
  ],
  "sentinel_level_snapshots": [
    {
      "name": "level_anchor_present",
      "class_name": "BP_LevelAnchor_C",
      "min_total_actor_count": 1,
      "min_matched_actor_count": 1,
      "min_component_count": 1,
      "expected_actor_names": ["LevelAnchor"],
      "expected_actor_classes": ["BP_LevelAnchor_C"],
      "expected_component_names": ["SceneRoot"],
      "expected_component_classes": ["SceneComponent"],
      "include_components": true,
      "component_limit": 20,
      "limit": 100
    }
  ],
  "sentinel_asset_searches": [
    {
      "name": "core_blueprints",
      "root": "Content/Project/Blueprints",
      "name_contains": "BP_",
      "expected_assets": ["BP_Player", "BP_GameMode"],
      "limit": 50
    }
  ],
  "sentinel_blueprints": [
    {
      "name": "player_blueprint",
      "asset_path": "/Game/Project/Blueprints/BP_Player",
      "expected_blueprint_name": "BP_Player",
      "expected_parent_class": "Character",
      "expected_generated_class": "BP_Player_C"
    }
  ],
  "automation_prefixes": [
    {
      "name": "phase1_tests",
      "prefix": "Project.Phase1",
      "min_tests": 1,
      "limit": 10
    }
  ]
}
```

`sentinel_maps` reads `get_editor_status.current_map` and passes when the current map is one of `expected_maps`. A singular `expected_map` is also accepted. If neither field is present, the gate falls back to the profile's top-level `known_maps`.

`sentinel_level_snapshots` calls `get_level_snapshot` with optional `class_name`, `name_contains`, `include_components`, and `component_limit` filters. It can assert `min_total_actor_count`, `min_matched_actor_count`, `min_component_count`, `expected_actor_names`, `expected_actor_classes`, `expected_component_names`, and `expected_component_classes` across the returned actors. Use component assertions with `include_components: true`.

When `automation_prefixes` is omitted, the runner uses top-level `automation_test_prefixes` as compatibility gates. Empty project packs are treated as unsuccessful because they do not prove compatibility.

## get_project_context

Returns the active project profile without touching Unreal state. The default profile name remains `failstate` for compatibility, but the tool is project-neutral. Profile lookup order is:

1. `UEMCP_PROFILE_DIR/<profile>.json`
2. `.uemcp.local/profiles/<profile>.json`
3. `Python/profiles/<profile>.json`

The response includes `active_profile`, `profile`, `profile_source`, `read_only`, and profile warnings. Project-specific paths, maps, sentinel assets, and workflow gates should live in the consuming project's UEMCP pack, not in UEMCP core.

## get_failstate_context

Compatibility alias for `get_project_context`. New workflows should call `get_project_context`.

The packaged shareable Failstate example still describes:

- `C:/Dev/Failstate`
- `C:/Dev/Failstate/.worktrees/phase1-combat-shell`
- UE 5.7
- `Content/Failstate/Blueprints/Blockout`
- `Failstate.Phase1` automation tests

## Live Smoke Gate

Use the repo smoke script to prove the editor-target build, bridge listener, and read-only observability tools against a running Unreal Editor:

```powershell
powershell -ExecutionPolicy Bypass -File .\Scripts\Smoke-UEMCPObservability.ps1
```

The script defaults to `D:\Epic\UE_5.7`, builds `MCPGameProjectEditor`, launches `MCPGameProject.uproject` when the bridge is not already listening, waits for `127.0.0.1:55557`, and fails if any observability envelope is not `ok: true`, if `get_editor_status.project_path` does not match the sample project, if `diagnose_editor_automation_readiness` does not report `ready_for_automation: true`, if `get_output_log` returns no live entries, if `get_level_snapshot(limit=25)` violates its actor count bounds, if category/substring filtering returns entries outside the requested filter, or if `asset_search(root="/Game", limit=20)` returns assets outside `/Game` or outside the requested limit.

When `-PluginPath` points at a plugin under a different Unreal project, the script builds that plugin owner project first so attached-plugin smoke runs exercise the current plugin source, not a stale DLL.

The script also requires `get_editor_readiness(timeout_seconds=90, stable_samples=2, settle_seconds=20)` to report ready before the first automation gate, then requires a shorter readiness check before the direct `run_automation_test` call. After readiness, `list_automation_tests(prefix="UEMCP.")` must return `UEMCP.Observability.Smoke`, `run_automation_test("UEMCP.Observability.Smoke")` must pass with zero errors, and `run_profile_automation_tests(test_name="UEMCP.Observability.Smoke", require_ready=true, readiness_timeout_seconds=60, readiness_stable_samples=2)` must return a successful single-test summary, readiness evidence, an empty `observability_events` list, and output-log tail evidence.

The script also calls `get_observability_recent_events(limit=50)` at the end and requires recent `get_editor_readiness`, `diagnose_editor_automation_readiness`, and `run_profile_automation_tests` entries. When a project-owned profile directory is mounted, it also requires recent `run_project_compatibility_gates` history. The history read must not record itself and must not contain unexpected failed entries during a passing smoke. Finally, `summarize_observability_state(limit=50)` must report `state: ready`, no latest blocker, no recommended next step, a successful latest entry, and zero unsuccessful entries.

To prove the same read-only gate against Failstate without copying plugin files into the Failstate repo, attach the repo plugin through Unreal's supported `-PLUGIN=` switch:

```powershell
powershell -ExecutionPolicy Bypass -File .\Scripts\Smoke-UEMCPObservability.ps1 `
  -ProjectPath 'C:\Dev\Failstate\.worktrees\phase1-combat-shell\Failstate.uproject' `
  -PluginPath 'C:\Dev\UEMCP\MCPGameProject\Plugins\UnrealMCP\UnrealMCP.uplugin' `
  -ProfileDir 'C:\Dev\Failstate\.worktrees\phase1-combat-shell\Tools\UEMCP\profiles' `
  -CloseLaunchedEditor
```

When `-ProfileDir` is supplied or auto-detected from `<ProjectRoot>\Tools\UEMCP\profiles`, the script requires `get_project_context.profile_source.kind` to be `environment` and runs `run_project_compatibility_gates` against that mounted pack. The smoke fails if the pack has no gates, if any project compatibility gate fails, if readiness evidence is missing, if the profile source is not the mounted environment profile, or if successful gates emit observability events.

If an external project has no `Plugins\UnrealMCP\UnrealMCP.uplugin` and no `-PluginPath`, the script fails before launching the editor instead of waiting for a bridge that cannot start.
