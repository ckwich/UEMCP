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
