# Asset Workflow Tools

These tools support the interactive asset lane. They are designed for Fab UI
and editor-backed import workflows where the agent needs before/after evidence
without making headless smoke depend on marketplace, auth, browser, or network
state.

Asset acquisition remains interactive. These tools do not bypass login,
entitlement, purchase, license acceptance, or payment prompts.

## asset_intake_snapshot

Captures read-only Asset Registry inventory for one or more package roots.

Parameters:

- `roots`: required package/content roots such as `/Game/Environment`.
- `classes`: optional class filters such as `StaticMesh`, `Texture2D`, or full class paths.
- `include_dependencies`: include package dependencies for each returned asset, defaults to true.
- `include_referencers`: include package referencers for each returned asset, defaults to false.
- `include_tags`: include Asset Registry tags for each returned asset, defaults to true.
- `limit`: maximum returned assets, clamped from 1 to 10000.

The Unreal bridge implementation uses Asset Registry metadata and does not load,
save, or mutate asset packages.

## asset_intake_diff

Compares two snapshot payloads by package/object identity.

Parameters:

- `before`: required snapshot object.
- `after`: required snapshot object.
- `include_unchanged`: include unchanged asset entries, defaults to false.

The response reports `added`, `removed`, `changed`, and a summary. Changed assets
include the changed field names plus before/after entries.

## asset_intake_write_manifest

Writes a reviewed asset intake diff to a project-owned JSON manifest.

Parameters:

- `diff`: required diff payload from `asset_intake_diff`.
- `output_path`: required manifest path.
- `notes`: optional operator notes, including source URL/name/license context.
- `project_root`: optional consuming project root used to resolve relative paths.
- `active_profile`: optional UEMCP profile name.
- `project_path`: optional consuming `.uproject` path.
- `allow_custom_output_path`: defaults to false.

By default, manifests must be written under `Tools/UEMCP/asset-intake`. This
keeps asset provenance in the consuming project pack instead of public UEMCP
defaults.

## asset_import_from_disk

Imports source files already present on disk through Unreal Editor import tasks.

Parameters:

- `source_files`: required absolute or relative local file paths.
- `destination_path`: required `/Game` content folder such as `/Game/Imported`.
- `replace_existing`: replace existing assets at the planned package names, defaults to false.
- `save_imported_assets`: save imported packages during the import task, defaults to false.
- `dry_run`: report planned packages, missing files, and conflicts without importing, defaults to true.

Use `dry_run: true` first. The mutating run returns imported object paths,
failed files, dirty packages, and save intent.

## Asset Organization

Use `asset_organize_plan` to validate exact-path operations before calling a
mutating organization tool.

Tools:

- `asset_organize_plan`: read-only validation for operation lists.
- `asset_rename`: rename one exact package path with a leaf `new_name`.
- `asset_move`: move one exact package path to another exact package path.
- `asset_duplicate`: duplicate one exact package path to another exact package path.
- `asset_delete`: delete one exact package path; wildcards are refused.
- `asset_save_packages`: save explicit package paths through the editor asset subsystem.
- `asset_fixup_redirectors`: list or fix redirectors under explicit `/Game` roots.

Every mutating organization tool uses editor-backed APIs and returns changed
package evidence. Broad deletes, shell-side asset edits, and wildcard package
operations are intentionally outside the public surface.

## Placement Preparation

`asset_prepare_for_level` loads one exact asset and reports placement readiness.
For Static Mesh assets it includes bounds, collision presence, material slot
count, LOD0 section count, Nanite state, scale hints, and warnings. For
Blueprint assets it reports whether the generated class is actor-placeable.

`asset_create_blueprint_wrapper` creates or dry-runs a placement-ready Actor
Blueprint wrapper for a Static Mesh at an explicit target package path.

## Map Placement

Use `asset_place_in_level_plan` before `asset_place_in_level`.

`asset_place_in_level` places Static Mesh or Actor Blueprint assets into the
current editor level with explicit actor names and transforms. It can save the
level when `save_level` is true and reports created actors plus changed
packages.

`asset_validate_level_placements` checks expected actor names or labels in the
current editor level and returns found/missing evidence.
