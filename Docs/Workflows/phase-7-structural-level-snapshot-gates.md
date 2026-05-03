# Phase 7 Structural Level Snapshot Gates

Phase 7 makes level snapshot sentinels structural without making UEMCP project-specific.

## Goal

Project packs can prove that a loaded map contains the right kind of placed actor and representative component anchors while UEMCP stays limited to generic read-only observations.

## Contract

`sentinel_level_snapshots` supports:

- `class_name` and `name_contains` filters for narrowing the actor set.
- `min_total_actor_count` for broad map population checks.
- `min_matched_actor_count` for filtered actor-set checks.
- `min_component_count` for returned component-set checks.
- `expected_actor_names` and `expected_actor_classes` for returned actor identity checks.
- `expected_component_names` and `expected_component_classes` with `include_components: true` for representative component-anchor checks.

Avoid exact generated actor instance names unless the consuming project has declared them stable. Prefer generated class names and component names for authored gameplay coordinators.

## Done

- Generic gate tests cover matched-count, component-count, missing-component-name, and missing-component-class failures.
- Public docs describe the neutral profile schema.
- Consuming projects can tighten their own packs without adding project names, paths, maps, or asset assumptions to UEMCP core.
