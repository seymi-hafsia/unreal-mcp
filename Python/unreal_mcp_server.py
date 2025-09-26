"""
Unreal Engine MCP Server

A simple MCP server for interacting with Unreal Engine.
"""

import logging
import socket
import sys
import time
import uuid
from contextlib import asynccontextmanager
from typing import AsyncIterator, Dict, Any, Optional
from mcp.server.fastmcp import FastMCP

from protocol import (
    ProtocolError,
    current_timestamp_ms,
    read_frame,
    write_frame,
)

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

class UnrealConnection:
    """Connection to an Unreal Engine instance using Protocol v1."""

    PROTOCOL_VERSION = 1
    HANDSHAKE_TIMEOUT = 10.0
    WRITE_TIMEOUT = 5.0
    IDLE_TIMEOUT = 60.0
    ENGINE_VERSION = "5.6.x"
    CLIENT_VERSION = "python-mcp/1.0.0"

    def __init__(self) -> None:
        self.socket: Optional[socket.socket] = None
        self.connected = False
        self.session_id = str(uuid.uuid4())
        self.capabilities: list[str] = []
        now = time.monotonic()
        self._last_receive = now
        self._last_send = now

    def connect(self) -> bool:
        """Connect to the Unreal Engine instance and perform handshake."""

        self.disconnect()

        try:
            logger.info("Connecting to Unreal at %s:%s...", UNREAL_HOST, UNREAL_PORT)
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
            sock.settimeout(None)
            sock.connect((UNREAL_HOST, UNREAL_PORT))

            self.socket = sock
            self.connected = True
            self._perform_handshake()
            logger.info("Connected to Unreal Engine (capabilities=%s)", self.capabilities)
            return True

        except ProtocolError as exc:
            logger.error("Protocol handshake failed: %s (%s)", exc.code, exc)
            self.disconnect()
            return False
        except Exception as exc:  # pragma: no cover - defensive logging
            logger.error("Failed to connect to Unreal: %s", exc)
            self.disconnect()
            return False

    def disconnect(self) -> None:
        """Disconnect from the Unreal Engine instance."""

        if self.socket:
            try:
                self.socket.close()
            except OSError:
                pass
        self.socket = None
        self.connected = False

    def _perform_handshake(self) -> None:
        if not self.socket:
            raise ProtocolError("INTERNAL_ERROR", "Socket not initialized.")

        handshake = {
            "type": "handshake",
            "protocolVersion": self.PROTOCOL_VERSION,
            "engineVersion": self.ENGINE_VERSION,
            "pluginVersion": self.CLIENT_VERSION,
            "sessionId": self.session_id,
        }

        write_frame(self.socket, handshake, timeout=self.WRITE_TIMEOUT)
        ack = read_frame(self.socket, timeout=self.HANDSHAKE_TIMEOUT)

        if ack.get("type") != "handshake/ack" or not ack.get("ok", False):
            raise ProtocolError("PROTOCOL_VERSION_MISMATCH", "Protocol handshake rejected.", {"response": ack})

        self.capabilities = list(ack.get("capabilities", []))
        now = time.monotonic()
        self._last_send = now
        self._last_receive = now

    def _handle_control_message(self, message: Dict[str, Any]) -> bool:
        if not self.socket:
            return False

        message_type = message.get("type")
        if message_type == "ping":
            timestamp = int(message.get("ts", current_timestamp_ms()))
            try:
                write_frame(self.socket, {"type": "pong", "ts": timestamp}, timeout=self.WRITE_TIMEOUT)
                self._last_send = time.monotonic()
                logger.debug("Responded to ping (%s)", timestamp)
            except ProtocolError as exc:
                logger.error("Failed to respond to ping: %s", exc)
                raise
            return True

        if message_type == "pong":
            logger.debug("Received pong (%s)", message.get("ts"))
            return True

        return False

    def _wait_for_message(self) -> Dict[str, Any]:
        if not self.socket:
            raise ProtocolError("READ_TIMEOUT", "Socket not connected.")

        deadline = time.monotonic() + self.IDLE_TIMEOUT
        while True:
            remaining = max(0.0, deadline - time.monotonic())
            message = read_frame(self.socket, timeout=remaining)
            self._last_receive = time.monotonic()
            if self._handle_control_message(message):
                continue
            return message

    def send_command(self, command: str, params: Optional[Dict[str, Any]] = None) -> Optional[Dict[str, Any]]:
        """Send a command to Unreal Engine and wait for a framed response."""

        if not self.connected or not self.socket:
            if not self.connect():
                logger.error("Failed to connect to Unreal Engine for command")
                return None

        payload = {
            "type": command,
            "params": params or {},
        }

        try:
            write_frame(self.socket, payload, timeout=self.WRITE_TIMEOUT)
            self._last_send = time.monotonic()
            response = self._wait_for_message()
            logger.debug("Received response payload: %s", response)
            return response

        except ProtocolError as exc:
            logger.error("Protocol error while communicating with Unreal: %s (%s)", exc.code, exc)
            error_payload = exc.to_dict()
            self.disconnect()
            return error_payload
        except Exception as exc:  # pragma: no cover - defensive logging
            logger.error("Unexpected error while sending command: %s", exc)
            self.disconnect()
            return {
                "ok": False,
                "error": {
                    "code": "INTERNAL_ERROR",
                    "message": str(exc),
                    "details": {},
                },
            }

# Global connection state
_unreal_connection: UnrealConnection = None

def get_unreal_connection() -> Optional[UnrealConnection]:
    """Get the connection to Unreal Engine."""
    global _unreal_connection
    try:
        if _unreal_connection is None:
            _unreal_connection = UnrealConnection()
            if not _unreal_connection.connect():
                logger.warning("Could not connect to Unreal Engine")
                _unreal_connection = None

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
            logger.info("Connected to Unreal Engine on startup")
        else:
            logger.warning("Could not connect to Unreal Engine on startup")
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

# Register tools
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
    ### Viewport and Screenshots
    - `focus_viewport(target, location, distance, orientation)` - Focus viewport
    - `take_screenshot(filename, show_ui, resolution)` - Capture screenshots

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