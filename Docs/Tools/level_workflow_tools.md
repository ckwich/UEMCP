# Level Workflow Tools

Level workflow tools create, open, save, construct, and validate Unreal maps
through editor-owned APIs. They are the preferred surface for multi-step map
work because they pair mutation with exact package paths, dirty-map preflights,
and structured evidence.

UEMCP never shell-edits `.umap`, `.uasset`, redirectors, external actor
packages, or generated Unreal metadata. Map content changes must go through the
Unreal Editor bridge or visible editor workflows.

## level_list_maps

Read-only Asset Registry query for map packages under exact roots.

**Parameters:**
- `roots` (array, required) - Exact `/Game` roots such as `/Game/Maps`.
- `limit` (integer, optional) - Maximum maps returned, clamped to 10000.

**Returns:**
- `roots`, `map_count`, `matched_map_count`, `truncated`,
  `asset_registry_loading`, and `maps`.

## level_create

Create a new map and optionally save it to an exact package path.

**Parameters:**
- `package_path` (string, required) - Exact `/Game/...` map package path.
- `template_path` (string, optional) - Exact `/Game/...` template map package.
- `save_existing` (boolean, optional) - Save dirty current-map packages before
  replacing the editor world, defaults to false.
- `save_new_level` (boolean, optional) - Save the new map, defaults to true.
- `fail_if_exists` (boolean, optional) - Refuse existing packages, defaults to
  true.
- `dry_run` (boolean, optional) - Report the plan without mutation, defaults to
  true.

`level_create` refuses dirty current-map packages unless `save_existing` is
true.

## level_open

Open one exact map package through Unreal Editor loading APIs.

**Parameters:**
- `package_path` (string, required) - Exact `/Game/...` map package path.
- `save_existing` (boolean, optional) - Save dirty current-map packages first.
- `require_exists` (boolean, optional) - Refuse missing maps, defaults to true.

## level_save

Save the current editor map and map-related packages.

**Parameters:**
- `package_path` (string, optional) - If provided, must match the current map.
- `only_if_dirty` (boolean, optional) - Save only dirty packages, defaults to
  true.
- `include_external_actor_packages` (boolean, optional) - Include actor and
  external actor packages, defaults to true.

## level_construction_plan

Python-only dry-run validator for declarative map construction.

Allowed operations:
- `ensure_actor`
- `set_actor_transform`
- `set_actor_folder`
- `set_actor_label`
- `set_actor_tags`
- `delete_actor`

Every operation requires an exact `actor_name`. Wildcards are refused.
`ensure_actor` requires either `actor_class` or `asset_path`. `delete_actor`
requires `confirm_delete: true`.

## level_apply_construction_plan

Apply a validated construction plan through editor-owned APIs.

**Parameters:**
- `operations` (array, required) - Normalized construction operations.
- `target_map` (string, optional) - Exact map package expected for the plan.
- `open_level` (boolean, optional) - Open `target_map` before applying.
- `create_if_missing` (boolean, optional) - Create a missing target map.
- `save_level` (boolean, optional) - Save after applying.
- `dry_run` (boolean, optional) - Report would-create/update/delete evidence.

## level_validate_construction

Read-only validation for expected actors in the current editor map.

**Parameters:**
- `expected_actors` (array, required) - Objects with exact `actor_name` plus
  optional `class`, `label`, `folder_path`, `tags`, and `location`.
- `target_map` (string, optional) - If provided, must match the current map.
- `location_tolerance` (number, optional) - Distance tolerance, defaults to
  `1.0`.

## When To Use These Instead Of Actor Tools

Use actor tools for narrow one-off edits in an already confirmed map. Use level
workflow tools for map lifecycle, batch construction, smoke fixtures, or any
workflow that needs exact map-package evidence.
