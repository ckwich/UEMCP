# Unreal MCP Editor Tools

This document provides detailed information about the editor tools available in the Unreal MCP integration.

## Overview

Editor tools provide explicit editor-level commands through the Unreal bridge. MCP-registered tools are listed in the Python tool surface; bridge-only commands remain internal until they have a validated public contract.

## Editor Tools

### save_current_level

Save the current editor level through Unreal's editor save API.

**Parameters:**
- `only_if_dirty` (boolean, optional) - Skip the save call when the current level package is not dirty, defaults to true

**Returns:**
- Current map, package name, previous dirty state, whether a save occurred, and dirty state after the save

**Example:**
```json
{
  "command": "save_current_level",
  "params": {
    "only_if_dirty": true
  }
}
```

### Bridge-only viewport commands

`focus_viewport` and `take_screenshot` remain lower-level bridge commands and are not currently registered as Python MCP tools. Use the tool-surface audit manifest as the source of truth before exposing either command to agents.

### Asset package operations

Use [Asset Workflow Tools](asset_workflow_tools.md) for editor-backed asset
imports, moves, renames, duplication, deletes, package saves, redirector
fix-up, and Blueprint wrapper creation. Those tools keep exact package-path
validation and dry-run evidence beside the asset workflow contract instead of
mixing asset policy into generic editor commands.

## Error Handling

All command responses include a "status" field indicating whether the operation succeeded, and an optional "message" field with details in case of failure.

```json
{
  "status": "error",
  "message": "Failed to get active viewport"
}
```

## Usage Examples

### Python Example

```python
from unreal_mcp_server import get_unreal_connection

# Get connection to Unreal Engine
unreal = get_unreal_connection()

# Save current level after explicit editor-backed mutations
save_response = unreal.send_command("save_current_level", {"only_if_dirty": true})
print(save_response)
```

## Troubleshooting

- **Command fails with "Failed to get active viewport"**: Make sure Unreal Editor is running and has an active viewport.
- **Actor not found**: Verify that the actor name is correct and the actor exists in the current level.
- **Invalid parameters**: Ensure that location and orientation arrays contain exactly 3 values (X, Y, Z for location; Pitch, Yaw, Roll for orientation).

## Future Enhancements

- Support for setting viewport display mode (wireframe, lit, etc.)
- Camera animation paths for cinematic viewport control
- Support for multiple viewports
