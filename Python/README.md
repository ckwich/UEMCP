# UEMCP

Observability-first Python MCP bridge for interacting with Unreal Engine through the UEMCP editor plugin.

## Setup

1. Make sure Python 3.10+ is installed
2. Install `uv` if you haven't already:
   ```bash
   curl -LsSf https://astral.sh/uv/install.sh | sh
   ```
3. Create and activate a virtual environment:
   ```bash
   uv venv
   source .venv/bin/activate  # On Unix/macOS
   # or
   .venv\Scripts\activate     # On Windows
   ```
4. Install dependencies:
   ```bash
   uv sync --extra dev
   ```

At this point, configure your MCP client to run `uv --directory C:/Dev/UEMCP/Python run unreal_mcp_server.py`.

## Observability Tools

The first supported UEMCP tools are read-mostly:

- `uemcp_ping`
- `get_editor_status`
- `get_output_log`
- `get_failstate_context`

These tools return structured envelopes with request IDs, timing, editor identity, warnings, and categorized errors.

## Testing Scripts

There are several scripts in the [scripts](./scripts) folder. They are useful for testing the tools and the Unreal Bridge via a direct connection. This means that you do not need to have an MCP Server running.

You should make sure you have installed dependencies and/or are running in the `uv` virtual environment in order for the scripts to work.


## Validation

```bash
uv lock --check
uv run --extra dev pytest -q
uv run python -c "from unreal_mcp_server import mcp; print(type(mcp).__name__)"
```

## Troubleshooting

- Make sure Unreal Engine editor is loaded loaded and running before running the server.
- Check logs in `unreal_mcp.log` for detailed error information

## Development

Add Python MCP tools under `tools/`, keep read-only observability separate from mutating editor commands, and add pytest coverage before implementation.
