"""UEMCP server for Unreal Engine editor integration."""

import logging
import socket
import sys
import json
from contextlib import asynccontextmanager
from typing import AsyncIterator, Dict, Any, Optional
from mcp.server.fastmcp import FastMCP

# Configure logging with more detailed format
logging.basicConfig(
    level=logging.DEBUG,  # Change to DEBUG level for more details
    format='%(asctime)s - %(name)s - %(levelname)s - [%(filename)s:%(lineno)d] - %(message)s',
    handlers=[
        logging.FileHandler('unreal_mcp.log'),
        # logging.StreamHandler(sys.stdout) # Remove this handler to unexpected non-whitespace characters in JSON
    ]
)
logger = logging.getLogger("UnrealMCP")

# Configuration
UNREAL_HOST = "127.0.0.1"
UNREAL_PORT = 55557
CONNECT_TIMEOUT_SECONDS = 5.0
RECEIVE_TIMEOUT_SECONDS = 30.0
AUTOMATION_COMMAND_GRACE_SECONDS = 15.0
MAX_RESPONSE_BYTES = 8 * 1024 * 1024


def summarize_json_for_log(value: Any, max_chars: int = 800) -> str:
    """Return a bounded JSON-ish representation for logs."""
    try:
        text = json.dumps(value, sort_keys=True, default=str)
    except TypeError:
        text = str(value)
    if len(text) <= max_chars:
        return text
    return f"{text[:max_chars]}...<truncated {len(text) - max_chars} chars>"

class UnrealConnection:
    """Connection to an Unreal Engine instance."""
    
    def __init__(
        self,
        connect_timeout_seconds: float = CONNECT_TIMEOUT_SECONDS,
        receive_timeout_seconds: float = RECEIVE_TIMEOUT_SECONDS,
    ):
        """Initialize the connection."""
        self.socket = None
        self.connected = False
        self.connect_timeout_seconds = float(connect_timeout_seconds)
        self.receive_timeout_seconds = float(receive_timeout_seconds)
    
    def connect(self) -> bool:
        """Connect to the Unreal Engine instance."""
        try:
            # Close any existing socket
            if self.socket:
                try:
                    self.socket.close()
                except:
                    pass
                self.socket = None
            
            logger.info(f"Connecting to Unreal at {UNREAL_HOST}:{UNREAL_PORT}...")
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.settimeout(self.connect_timeout_seconds)
            
            # Set socket options for better stability
            self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            
            # Set larger buffer sizes
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
            
            self.socket.connect((UNREAL_HOST, UNREAL_PORT))
            self.connected = True
            logger.info("Connected to Unreal Engine")
            return True
            
        except Exception as e:
            logger.error(f"Failed to connect to Unreal: {e}")
            self.connected = False
            return False
    
    def disconnect(self):
        """Disconnect from the Unreal Engine instance."""
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
        self.socket = None
        self.connected = False

    def receive_timeout_for_command(self, command: str, params: Dict[str, Any] = None) -> float:
        """Return the socket receive budget for a command response."""
        if command == "run_automation_test":
            requested_timeout = RECEIVE_TIMEOUT_SECONDS
            try:
                requested_timeout = float((params or {}).get("timeout_seconds", requested_timeout))
            except (TypeError, ValueError):
                logger.warning("Invalid automation timeout parameter: %s", params)

            return max(
                self.receive_timeout_seconds,
                requested_timeout + AUTOMATION_COMMAND_GRACE_SECONDS,
            )

        return self.receive_timeout_seconds

    def receive_full_response(
        self,
        sock,
        buffer_size=4096,
        timeout_seconds: Optional[float] = None,
    ) -> bytes:
        """Receive a complete response from Unreal, handling chunked data."""
        chunks = []
        sock.settimeout(timeout_seconds or self.receive_timeout_seconds)
        try:
            while True:
                chunk = sock.recv(buffer_size)
                if not chunk:
                    if not chunks:
                        raise Exception("Connection closed before receiving data")
                    break
                chunks.append(chunk)
                
                # Process the data received so far
                data = b''.join(chunks)
                if len(data) > MAX_RESPONSE_BYTES:
                    raise Exception(f"Unreal response exceeded {MAX_RESPONSE_BYTES} bytes")

                decoded_data = data.decode('utf-8')
                
                # Try to parse as JSON to check if complete
                try:
                    json.loads(decoded_data)
                    logger.info(f"Received complete response ({len(data)} bytes)")
                    return data
                except json.JSONDecodeError:
                    # Not complete JSON yet, continue reading
                    logger.debug(f"Received partial response, waiting for more data...")
                    continue
                except Exception as e:
                    logger.warning(f"Error processing response chunk: {str(e)}")
                    continue
        except socket.timeout:
            logger.warning("Socket timeout during receive")
            if chunks:
                # If we have some data already, try to use it
                data = b''.join(chunks)
                try:
                    json.loads(data.decode('utf-8'))
                    logger.info(f"Using partial response after timeout ({len(data)} bytes)")
                    return data
                except:
                    pass
            raise Exception("Timeout receiving Unreal response")
        except Exception as e:
            logger.error(f"Error during receive: {str(e)}")
            raise
    
    def send_command(self, command: str, params: Dict[str, Any] = None) -> Optional[Dict[str, Any]]:
        """Send a command to Unreal Engine and get the response."""
        # Always reconnect for each command, since Unreal closes the connection after each command
        # This is different from Unity which keeps connections alive
        if self.socket:
            try:
                self.socket.close()
            except:
                pass
            self.socket = None
            self.connected = False
        
        if not self.connect():
            logger.error("Failed to connect to Unreal Engine for command")
            return None
        
        try:
            # Match Unity's command format exactly
            command_obj = {
                "type": command,  # Use "type" instead of "command"
                "params": params or {}  # Use Unity's params or {} pattern
            }
            
            # Send without newline, exactly like Unity
            command_json = json.dumps(command_obj)
            logger.info(
                "Sending command '%s' (%d bytes, params=%s)",
                command,
                len(command_json.encode('utf-8')),
                summarize_json_for_log(params or {}),
            )
            self.socket.sendall(command_json.encode('utf-8'))
            
            # Read response using improved handler
            response_data = self.receive_full_response(
                self.socket,
                timeout_seconds=self.receive_timeout_for_command(command, params),
            )
            response = json.loads(response_data.decode('utf-8'))
            
            logger.info(
                "Received response for '%s': status=%s keys=%s",
                command,
                response.get("status"),
                sorted(response.keys()),
            )
            
            # Check for both error formats: {"status": "error", ...} and {"success": false, ...}
            if response.get("status") == "error":
                error_message = response.get("error") or response.get("message", "Unknown Unreal error")
                logger.error(f"Unreal error (status=error): {error_message}")
                # We want to preserve the original error structure but ensure error is accessible
                if "error" not in response:
                    response["error"] = error_message
            elif response.get("success") is False:
                # This format uses {"success": false, "error": "message"} or {"success": false, "message": "message"}
                error_message = response.get("error") or response.get("message", "Unknown Unreal error")
                logger.error(f"Unreal error (success=false): {error_message}")
                # Convert to the standard format expected by higher layers
                response = {
                    "status": "error",
                    "error": error_message
                }
            
            # Always close the connection after command is complete
            # since Unreal will close it on its side anyway
            try:
                self.socket.close()
            except:
                pass
            self.socket = None
            self.connected = False
            
            return response
            
        except Exception as e:
            logger.error(f"Error sending command: {e}")
            # Always reset connection state on any error
            self.connected = False
            try:
                self.socket.close()
            except:
                pass
            self.socket = None
            return {
                "status": "error",
                "error": str(e)
            }

# Global connection state
_unreal_connection: UnrealConnection = None

def get_unreal_connection() -> Optional[UnrealConnection]:
    """Get the connection to Unreal Engine."""
    global _unreal_connection
    try:
        if _unreal_connection is None:
            _unreal_connection = UnrealConnection()
        
        return _unreal_connection
    except Exception as e:
        logger.error(f"Error getting Unreal connection: {e}")
        return None

@asynccontextmanager
async def server_lifespan(server: FastMCP) -> AsyncIterator[Dict[str, Any]]:
    """Handle server startup and shutdown."""
    global _unreal_connection
    logger.info("UnrealMCP server starting up")
    try:
        _unreal_connection = get_unreal_connection()
        if _unreal_connection:
            logger.info("Initialized Unreal connection manager; commands reconnect per request")
        else:
            logger.warning("Could not initialize Unreal connection manager")
    except Exception as e:
        logger.error(f"Error connecting to Unreal Engine on startup: {e}")
        _unreal_connection = None
    
    try:
        yield {}
    finally:
        if _unreal_connection:
            _unreal_connection.disconnect()
            _unreal_connection = None
        logger.info("Unreal MCP server shut down")

# Initialize server
mcp = FastMCP(
    "UnrealMCP",
    description="Unreal Engine integration via Model Context Protocol",
    lifespan=server_lifespan
)

# Import and register tools
from tools.editor_tools import register_editor_tools
from tools.blueprint_tools import register_blueprint_tools
from tools.node_tools import register_blueprint_node_tools
from tools.project_tools import register_project_tools
from tools.umg_tools import register_umg_tools
from tools.observability_tools import register_observability_tools
from tools.asset_workflow_tools import register_asset_workflow_tools

# Register tools
register_observability_tools(mcp)
register_asset_workflow_tools(mcp)
register_editor_tools(mcp)
register_blueprint_tools(mcp)
register_blueprint_node_tools(mcp)
register_project_tools(mcp)
register_umg_tools(mcp)  

@mcp.prompt()
def info():
    """Information about available Unreal MCP tools and best practices."""
    return """
    # Unreal MCP Server Tools and Best Practices
    
    ## UMG (Widget Blueprint) Tools
    - `create_umg_widget_blueprint(widget_name, parent_class="UserWidget", path="/Game/UI")` 
      Create a new UMG Widget Blueprint
    - `add_text_block_to_widget(widget_name, text_block_name, text="", position=[0,0], size=[200,50], font_size=12, color=[1,1,1,1])`
      Add a Text Block widget with customizable properties
    - `add_button_to_widget(widget_name, button_name, text="", position=[0,0], size=[200,50], font_size=12, color=[1,1,1,1], background_color=[0.1,0.1,0.1,1])`
      Add a Button widget with text and styling
    - `bind_widget_event(widget_name, widget_component_name, event_name, function_name="")`
      Bind events like OnClicked to functions
    - `add_widget_to_viewport(widget_name, z_order=0)`
      Add widget instance to game viewport
    - `set_text_block_binding(widget_name, text_block_name, binding_property, binding_type="Text")`
      Set up dynamic property binding for text blocks

    ## Editor Tools
    ### Actor Management
    - `get_actors_in_level()` - List all actors in current level
    - `find_actors_by_name(pattern)` - Find actors by name pattern
    - `spawn_actor(name, type, location=[0,0,0], rotation=[0,0,0], scale=[1,1,1])` - Create actors
    - `delete_actor(name)` - Remove actors
    - `set_actor_transform(name, location, rotation, scale)` - Modify actor transform
    - `get_actor_properties(name)` - Get actor properties
    
    ## Blueprint Management
    - `create_blueprint(name, parent_class)` - Create new Blueprint classes
    - `add_component_to_blueprint(blueprint_name, component_type, component_name)` - Add components
    - `set_static_mesh_properties(blueprint_name, component_name, static_mesh)` - Configure meshes
    - `set_physics_properties(blueprint_name, component_name)` - Configure physics
    - `compile_blueprint(blueprint_name)` - Compile Blueprint changes
    - `set_blueprint_property(blueprint_name, property_name, property_value)` - Set properties
    - `set_pawn_properties(blueprint_name)` - Configure Pawn settings
    - `spawn_blueprint_actor(blueprint_name, actor_name)` - Spawn Blueprint actors
    
    ## Blueprint Node Management
    - `add_blueprint_event_node(blueprint_name, event_type)` - Add event nodes
    - `add_blueprint_input_action_node(blueprint_name, action_name)` - Add input nodes
    - `add_blueprint_function_node(blueprint_name, target, function_name)` - Add function nodes
    - `connect_blueprint_nodes(blueprint_name, source_node_id, source_pin, target_node_id, target_pin)` - Connect nodes
    - `add_blueprint_variable(blueprint_name, variable_name, variable_type)` - Add variables
    - `add_blueprint_get_self_component_reference(blueprint_name, component_name)` - Add component refs
    - `add_blueprint_self_reference(blueprint_name)` - Add self references
    - `find_blueprint_nodes(blueprint_name, node_type, event_type)` - Find nodes
    
    ## Project Tools
    - `create_input_mapping(action_name, key, input_type)` - Create input mappings
    
    ## Best Practices
    
    ### UMG Widget Development
    - Create widgets with descriptive names that reflect their purpose
    - Use consistent naming conventions for widget components
    - Organize widget hierarchy logically
    - Set appropriate anchors and alignment for responsive layouts
    - Use property bindings for dynamic updates instead of direct setting
    - Handle widget events appropriately with meaningful function names
    - Clean up widgets when no longer needed
    - Test widget layouts at different resolutions
    
    ### Editor and Actor Management
    - Use unique names for actors to avoid conflicts
    - Clean up temporary actors
    - Validate transforms before applying
    - Check actor existence before modifications
    - Take regular viewport screenshots during development
    - Keep the viewport focused on relevant actors during operations
    
    ### Blueprint Development
    - Compile Blueprints after changes
    - Use meaningful names for variables and functions
    - Organize nodes logically
    - Test functionality in isolation
    - Consider performance implications
    - Document complex setups
    
    ### Error Handling
    - Check command responses for success
    - Handle errors gracefully
    - Log important operations
    - Validate parameters
    - Clean up resources on errors
    """

# Run the server
if __name__ == "__main__":
    logger.info("Starting MCP server with stdio transport")
    mcp.run(transport='stdio')
