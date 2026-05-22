# Level Map Construction Workflow

This workflow is the generic UEMCP lane for editor-owned map construction.
Concrete game maps, room recipes, sentinel actors, and compatibility
expectations belong in the consuming project's `Tools/UEMCP` pack.

## Boundary

- UEMCP core owns generic `level_*` tools, evidence contracts, docs, and smoke.
- Project packs own concrete recipes under `Tools/UEMCP/recipes/maps/*.json`.
- Ignored local profile folders may hold machine-specific test recipes.
- UEMCP never shell-edits `.umap`, `.uasset`, redirectors, external actor
  packages, or generated Unreal asset metadata.
- `level_create` and `level_open` refuse dirty current-map packages unless the
  caller explicitly requests `save_existing=true`.

## Ladder

1. Run `get_editor_status` and confirm the expected project.
2. Run `level_list_maps` for the relevant `/Game` roots.
3. Use `level_create` or `level_open` with exact map package paths.
4. Validate a batch recipe with `level_construction_plan`.
5. Apply it with `level_apply_construction_plan`.
6. Verify with `level_validate_construction` and `get_level_snapshot`.
7. Persist with `level_save`.
8. Run project-pack compatibility gates.

## Recipe Shape

```json
{
  "name": "example_map_recipe",
  "target_map": "/Game/Project/Maps/L_Example",
  "operations": [
    {
      "op": "ensure_actor",
      "actor_name": "Example_PointLight",
      "actor_class": "/Script/Engine.PointLight",
      "label": "Example Point Light",
      "folder_path": "Example/Lighting",
      "location": [0.0, 0.0, 180.0],
      "rotation": [0.0, 0.0, 0.0],
      "scale": [1.0, 1.0, 1.0],
      "tags": ["Example"]
    }
  ],
  "expected_actors": [
    {
      "actor_name": "Example_PointLight",
      "class": "PointLight",
      "folder_path": "Example/Lighting",
      "tags": ["Example"],
      "location": [0.0, 0.0, 180.0]
    }
  ],
  "post_construction_gates": [
    {
      "kind": "map_recipe_current_map"
    },
    {
      "kind": "map_recipe_expected_actors"
    },
    {
      "kind": "map_recipe_min_actor_count",
      "min_total_actor_count": 1
    }
  ]
}
```

## Project-Pack Gates

Project profiles may include `map_recipes` with `post_construction_gates`.
`run_project_compatibility_gates` evaluates these after a project pack has
created or updated the map:

- `map_recipe_current_map` checks `get_editor_status.current_map` against the
  gate `expected_map` or the recipe `target_map`.
- `map_recipe_expected_actors` checks `get_level_snapshot` for expected actor
  names and classes from the gate or the recipe `expected_actors`.
- `map_recipe_min_actor_count` checks `get_level_snapshot.total_actor_count`
  against `min_total_actor_count`, `min_actor_count`, or the number of expected
  actors in the recipe.

## Asset Placement

For simple asset placement into a confirmed current map,
`asset_place_in_level_plan`, `asset_place_in_level`, and
`asset_validate_level_placements` remain useful. Use level workflow tools when
the work includes map lifecycle, batch construction, deletes, saves, or
project-pack map recipes.
