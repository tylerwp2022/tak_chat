"""
================================================================================
Launch file for tak_chat_node - supports single and multi-robot deployments
================================================================================

SINGLE ROBOT USAGE:
-------------------
# Default robot (warthog1)
ros2 launch tak_chat tak_chat.launch.py

# Specify different robot
ros2 launch tak_chat tak_chat.launch.py robot_name:=warthog2

MULTI-ROBOT USAGE:
------------------
# Launch multiple robots (overrides robot_name parameter)
ros2 launch tak_chat tak_chat.launch.py \
    robot_names:="['warthog1', 'warthog2', 'warthog3']"

# Multi-robot with custom TAK server
ros2 launch tak_chat tak_chat.launch.py \
    robot_names:="['warthog1', 'warthog2']" \
    tak_server_flow_tag_key:=TAK-Server-custom-uuid

PARAMETERS:
-----------
robot_name (string, default: "warthog1")
    Single robot's name. Ignored if robot_names is specified.

robot_names (string, default: "[]")
    List of robot names for multi-robot deployment (Python list format).
    If provided, overrides robot_name parameter.
    Example: "['warthog1', 'warthog2', 'warthog3']"

android_id (string, default: "dfac01d76beec661")
    Android device ID for GeoChat UID generation.

tak_server_flow_tag_key (string, default: "TAK-Server-...")
    Flow tag key for TAK server.

================================================================================
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
import ast


def launch_tak_chat_nodes(context, *args, **kwargs):
    """
    Dynamically create tak_chat nodes based on configuration.
    
    This function handles both single-robot and multi-robot scenarios:
      - If robot_names is provided (non-empty list): Launch multiple robots
      - Otherwise: Launch single robot using robot_name parameter
    
    Args:
        context: Launch context containing parameter values
    
    Returns:
        List of Node objects to launch
    """
    
    # Get launch configuration values
    robot_names_str = LaunchConfiguration('robot_names').perform(context)
    robot_name_single = LaunchConfiguration('robot_name').perform(context)
    android_id = LaunchConfiguration('android_id').perform(context)
    tak_server_flow_tag_key = LaunchConfiguration('tak_server_flow_tag_key').perform(context)
    
    # Determine if we're in multi-robot mode
    robot_names = []
    try:
        # Try to parse robot_names as a Python list
        parsed = ast.literal_eval(robot_names_str)
        if isinstance(parsed, list) and len(parsed) > 0:
            robot_names = parsed
    except:
        # If parsing fails or result is not a list, fall back to single robot
        pass
    
    # Create nodes based on mode
    nodes = []
    
    if robot_names:
        # =====================================================================
        # MULTI-ROBOT MODE
        # =====================================================================
        # Launch one tak_chat_node for each robot in the list
        
        print(f"\n{'='*80}")
        print(f"TAK CHAT MULTI-ROBOT MODE: Launching {len(robot_names)} robots")
        print(f"{'='*80}")
        
        for idx, robot_name in enumerate(robot_names, 1):
            print(f"  [{idx}/{len(robot_names)}] Configuring TAK chat for: {robot_name}")
            
            node = Node(
                package='tak_chat',
                executable='tak_chat_node',
                # Give each node a unique name to avoid conflicts
                name=f'tak_chat_node_{robot_name}',
                # Use namespace to isolate each robot's topics
                namespace=robot_name,
                parameters=[{
                    # Use robot_name as callsign (matches namespace)
                    'callsign': robot_name,
                    'android_id': android_id,
                    'tak_server_flow_tag_key': tak_server_flow_tag_key,
                    
                    # Topic configuration (relative to namespace)
                    'outgoing_cot_topic': 'send_to_tak',
                    'incoming_cot_topic': 'incoming_cot',
                    'navsat_topic': 'navsat',
                    'tak_chat_out_topic': 'tak_chat/out',
                    'tak_chat_in_topic': 'tak_chat/in',
                    'comms_topic': 'comms',
                    
                    # Allowed callsigns configuration
                    'allowed_callsigns_file': '/phoenix/src/phoenix-tak/src/tak_bridge/config/cot_runner.yaml',
                    
                    # Timing/retry parameters
                    'send_delay_s': 1.0,
                    'reply_delay_s': 1.0,
                    'retry_timeout_s': 10.0,
                    'retry_interval_s': 1.0,
                    'min_retry_count': 1,
                }],
                remappings=[
                    # Remap navsat to actual GPS topic within namespace
                    ('navsat', 'sensors/ublox/fix'),
                ],
                output='screen',
                emulate_tty=True,
                # Optional: Auto-restart if node crashes
                # respawn=True,
                # respawn_delay=2.0,
            )
            nodes.append(node)
        
        print(f"{'='*80}\n")
    
    else:
        # =====================================================================
        # SINGLE-ROBOT MODE (DEFAULT)
        # =====================================================================
        # Launch one node with the specified robot_name
        
        print(f"\n{'='*80}")
        print(f"TAK CHAT SINGLE-ROBOT MODE: Launching {robot_name_single}")
        print(f"{'='*80}\n")
        
        node = Node(
            package='tak_chat',
            executable='tak_chat_node',
            name='tak_chat_node',
            namespace=robot_name_single,
            parameters=[{
                'callsign': robot_name_single,
                'android_id': android_id,
                'tak_server_flow_tag_key': tak_server_flow_tag_key,
                
                # Topic configuration (relative to namespace)
                'outgoing_cot_topic': 'send_to_tak',
                'incoming_cot_topic': 'incoming_cot',
                'navsat_topic': 'navsat',
                'tak_chat_out_topic': 'tak_chat/out',
                'tak_chat_in_topic': 'tak_chat/in',
                'comms_topic': 'comms',
                
                # Allowed callsigns configuration
                'allowed_callsigns_file': '/phoenix/src/phoenix-tak/src/tak_bridge/config/cot_runner.yaml',
                
                # Timing/retry parameters
                'send_delay_s': 1.0,
                'reply_delay_s': 1.0,
                'retry_timeout_s': 10.0,
                'retry_interval_s': 1.0,
                'min_retry_count': 1,
            }],
            remappings=[
                ('navsat', 'sensors/ublox/fix'),
            ],
            output='screen',
            emulate_tty=True,
            # Optional: Auto-restart if node crashes
            # respawn=True,
            # respawn_delay=2.0,
        )
        nodes.append(node)
    
    return nodes


def generate_launch_description():
    """
    Generate the launch description with configurable parameters.
    
    This function is called by the ROS2 launch system to construct the
    launch description. It:
      1. Declares launch arguments (command-line parameters)
      2. Uses OpaqueFunction to dynamically create nodes based on arguments
      3. Returns the complete launch description
    """
    
    # =========================================================================
    # DECLARE LAUNCH ARGUMENTS
    # =========================================================================
    
    robot_name_arg = DeclareLaunchArgument(
        'robot_name',
        default_value='warthog1',
        description='Single robot name (ignored if robot_names is provided)'
    )
    
    robot_names_arg = DeclareLaunchArgument(
        'robot_names',
        default_value='[]',
        description='List of robot names for multi-robot mode: "[\'warthog1\', \'warthog2\']"'
    )
    
    android_id_arg = DeclareLaunchArgument(
        'android_id',
        default_value='dfac01d76beec661',
        description='Android device ID for GeoChat UID generation'
    )
    
    tak_server_flow_tag_key_arg = DeclareLaunchArgument(
        'tak_server_flow_tag_key',
        default_value='TAK-Server-d520578543014e9cba1916fad77b9917',
        description='Flow tag key for TAK server'
    )
    
    # =========================================================================
    # DYNAMIC NODE CREATION
    # =========================================================================
    # Use OpaqueFunction to create nodes at runtime based on parameters
    
    launch_nodes = OpaqueFunction(function=launch_tak_chat_nodes)
    
    # =========================================================================
    # RETURN LAUNCH DESCRIPTION
    # =========================================================================
    
    return LaunchDescription([
        # Declare launch arguments
        robot_name_arg,
        robot_names_arg,
        android_id_arg,
        tak_server_flow_tag_key_arg,
        
        # Dynamically create nodes
        launch_nodes,
    ])
