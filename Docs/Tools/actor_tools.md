# Unreal MCP Editor Tools

This document provides detailed information about the actor tools available in the Unreal MCP integration.

## Overview

Actor tools allow you to manipulate actors in the Unreal Engine scene.

## Actor Tools

### get_actors_in_level

Get a list of all actors in the current level.

**Parameters:**
- None

**Returns:**
- List of all actors with their properties

**Example:**
```json
{
  "command": "get_actors_in_level",
  "params": {}
}
```

### find_actors_by_name

Find actors in the current level by name pattern.

**Parameters:**
- `pattern` (string) - The name or partial name pattern to search for

**Returns:**
- List of matching actor names

**Example:**
```json
{
  "command": "find_actors_by_name",
  "params": {
    "pattern": "Cube"
  }
}
```

### create_actor

Create a new actor in the current level.

**Parameters:**
- `name` (string) - The name for the new actor (must be unique)
- `type` (string) - The type of actor to create (must be uppercase)
- `location` (array, optional) - [X, Y, Z] coordinates for the actor's position, defaults to [0, 0, 0]
- `rotation` (array, optional) - [Pitch, Yaw, Roll] values for the actor's rotation, defaults to [0, 0, 0]
- `scale` (array, optional) - [X, Y, Z] values for the actor's scale, defaults to [1, 1, 1]

**Returns:**
- Information about the created actor

**Example:**
```json
{
  "command": "create_actor",
  "params": {
    "name": "MyCube",
    "type": "CUBE",
    "location": [0, 0, 100],
    "rotation": [0, 45, 0],
    "scale": [2, 2, 2]
  }
}
```

### delete_actor

Delete an actor by name.

**Parameters:**
- `name` (string) - The name of the actor to delete

**Returns:**
- Result of the delete operation

**Example:**
```json
{
  "command": "delete_actor",
  "params": {
    "name": "MyCube"
  }
}
```

### set_actor_transform

Set the transform (location, rotation, scale) of an actor.

**Parameters:**
- `name` (string) - The name of the actor to modify
- `location` (array, optional) - [X, Y, Z] coordinates for the actor's position
- `rotation` (array, optional) - [Pitch, Yaw, Roll] values for the actor's rotation
- `scale` (array, optional) - [X, Y, Z] values for the actor's scale

**Returns:**
- Result of the transform operation

**Example:**
```json
{
  "command": "set_actor_transform",
  "params": {
    "name": "MyCube",
    "location": [100, 200, 300],
    "rotation": [0, 90, 0]
  }
}
```

### Asset-driven placement

For placing imported Static Mesh or Actor Blueprint assets, prefer
`asset_place_in_level_plan`, `asset_place_in_level`, and
`asset_validate_level_placements` from [Asset Workflow Tools](asset_workflow_tools.md).
Those tools pair placement mutations with asset readiness evidence, exact
source package paths, current-map checks, and optional level save reporting.

### get_actor_properties

Get read-only reflected properties of an actor.

**Parameters:**
- `name` (string) - The name of the actor
- `include_private` (boolean, optional) - Include private/protected reflected fields, defaults to false
- `include_transient` (boolean, optional) - Include transient reflected fields, defaults to false
- `include_config` (boolean, optional) - Include config reflected fields, defaults to false
- `include_non_editable` (boolean, optional) - Include fields that are not editable or Blueprint-visible, defaults to false
- `include_object_paths` (boolean, optional) - Include full object paths for object references, defaults to false
- `property_limit` (integer, optional) - Maximum top-level reflected properties to return, defaults to 64
- `max_struct_depth` (integer, optional) - Maximum nested struct depth, defaults to 3
- `max_collection_items` (integer, optional) - Maximum array entries returned per property, defaults to 16
- `name_contains` (string, optional) - Filter reflected property names by substring

**Returns:**
- Object containing actor identity, transform, and a `properties` object
- Each reflected property includes type information, access/replication/editing flags, a typed `value` when supported, and `value_text` from Unreal's property exporter
- Struct properties include `struct_type` and nested reflected `fields`
- `properties_meta` reports matching count, returned count, limits, filters, and truncation state
- Private, transient, config, object-path, and non-editable state is excluded by default and must be explicitly requested

**Example:**
```json
{
  "command": "get_actor_properties",
  "params": {
    "name": "MyCube",
    "property_limit": 32
  }
}
```

## Error Handling

All command responses include a "success" field indicating whether the operation succeeded, and an optional "message" field with details in case of failure.

```json
{
  "success": false,
  "message": "Actor 'MyCube' not found in the current level"
}
```

## Implementation Notes

- All numeric parameters for transforms (location, rotation, scale) must be provided as lists of 3 float values
- Actor types should be provided in uppercase
- The server maintains logging of all operations with detailed information and error messages
- All commands are executed through a connection to the Unreal Engine editor

## Type Reference

### Actor Types

Supported actor types for the `create_actor` command:

- `CUBE` - Static mesh cube
- `SPHERE` - Static mesh sphere
- `CYLINDER` - Static mesh cylinder
- `PLANE` - Static mesh plane
- `POINT_LIGHT` - Point light source
- `SPOT_LIGHT` - Spot light source
- `DIRECTIONAL_LIGHT` - Directional light source
- `CAMERA` - Camera actor
- `EMPTY` - Empty actor (container)

## Future Extensions

The following tool categories are planned for future releases:

- **Level Tools**: Managing Unreal Engine levels
- **Material Tools**: Creating and editing materials
- **Blueprint Tools**: Manipulating Blueprints
- **Asset Tools**: Managing project assets
- **Editor Tools**: Controlling the Unreal Editor
