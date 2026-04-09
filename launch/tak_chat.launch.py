"""
================================================================================
Launch file for tak_chat_node — single and multi-robot deployments
================================================================================

PETAAR26 INTEGRATION:
    All parameter defaults are sourced from the petaar26 package config files:
      config/tak_params.yaml  → tak_server_flow_tag_key, known_device_uids
      config/paths.yaml       → allowed_callsigns_file (cot_runner_yaml)
      profiles.json → gps_topic_suffix drives this value

BUG FIX (vs prior version):
    The navsat remapping was previously hardcoded to "sensors/geofog/gps/fix"
    in both single and multi-robot modes. NAI_3 and NAI_4 use u-blox GPS
    (/sensors/ublox/fix), so tak_chat was receiving no GPS position data on
    those conditions — TAK position CoTs were either stale or not sent.

    FIX: Added navsat_topic launch argument (default = geofog for backward
    compatibility). sim_control.py now passes the active profile's
    gps_topic_suffix as navsat_topic when launching tak_chat.

SINGLE ROBOT USAGE:
    # Default robot (warthog1) with default GPS (geofog):
    ros2 launch tak_chat tak_chat.launch.py

    # Specify different robot and GPS hardware:
    ros2 launch tak_chat tak_chat.launch.py \
        robot_name:=warthog2 \
        navsat_topic:=sensors/ublox/fix

MULTI-ROBOT USAGE:
    ros2 launch tak_chat tak_chat.launch.py \
        robot_names:="['warthog1', 'warthog2', 'warthog3']" \
        navsat_topic:=sensors/ublox/fix

PARAMETERS:
    robot_name (string, default: "warthog1")
        Single robot name. Ignored if robot_names is provided.

    robot_names (string, default: "[]")
        Python list of robot names for multi-robot mode.
        Example: "['warthog1', 'warthog2', 'warthog3']"

    navsat_topic (string, default: "sensors/geofog/gps/fix")
        GPS topic to remap navsat to within each robot namespace.
        ROBOT-RELATIVE (no leading slash, no robot_name prefix).
        The ROS2 namespace mechanism prepends the robot name automatically.
          geofog hardware (NAI_2, testing): sensors/geofog/gps/fix
          u-blox  hardware (NAI_3, NAI_4): sensors/ublox/fix
        Driven by gps_topic_suffix in the active profile (profiles.json).

    tak_server_flow_tag_key (string, default: from petaar26 tak_params.yaml)
        Flow tag key for TAK server. Default from config/tak_params.yaml.

    known_device_uids (string, default: from petaar26 tak_params.yaml)
        Pre-populated callsign→device UID map.
        Format: "['CALLSIGN:ANDROID-hex', ...]"
        WHY: The UID map is normally learned from incoming messages, but BT
        nodes may send unicasts before any message has been received from the
        operator. Pre-populating ensures correct chatgrp uid1 on first send.
        Default from config/tak_params.yaml → tak.known_device_uids.

================================================================================
"""

import ast
import os
import yaml

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


# =============================================================================
# PETAAR26 CONFIG LOADER
# =============================================================================

def _petaar26(filename: str) -> dict:
    """Load a petaar26 config YAML from the installed share directory."""
    path = os.path.join(
        get_package_share_directory('petaar26'),
        'config', filename
    )
    with open(path) as f:
        return yaml.safe_load(f)


_tak    = _petaar26('tak_params.yaml')['tak']
_topics = _petaar26('topics.yaml')['topics']
_paths  = _petaar26('paths.yaml')['paths']
_hw     = _petaar26('hardware.yaml')['hardware']


# =============================================================================
# DYNAMIC NODE CREATION
# =============================================================================

def launch_tak_chat_nodes(context, *args, **kwargs):
    """
    Dynamically create tak_chat nodes based on configuration.

    Handles both single-robot and multi-robot scenarios:
      - If robot_names is a non-empty list: launch one node per robot.
      - Otherwise: launch a single node using robot_name.

    The navsat remapping is applied per-node using the navsat_topic argument,
    which resolves to the active GPS hardware topic for the current profile.
    """

    # -------------------------------------------------------------------------
    # Resolve launch configuration values
    # -------------------------------------------------------------------------
    robot_names_str         = LaunchConfiguration('robot_names').perform(context)
    robot_name_single       = LaunchConfiguration('robot_name').perform(context)
    tak_server_flow_tag_key = LaunchConfiguration('tak_server_flow_tag_key').perform(context)
    known_device_uids_str   = LaunchConfiguration('known_device_uids').perform(context)

    # navsat_topic is ROBOT-RELATIVE (no leading slash, no namespace prefix).
    # The ROS2 namespace + remapping system resolves it to the full topic path.
    # Example: "sensors/ublox/fix" → /warthog1/sensors/ublox/fix
    navsat_topic = LaunchConfiguration('navsat_topic').perform(context)

    # -------------------------------------------------------------------------
    # Parse known_device_uids  (Python list string → Python list)
    # -------------------------------------------------------------------------
    known_device_uids = []
    try:
        parsed_uids = ast.literal_eval(known_device_uids_str)
        if isinstance(parsed_uids, list):
            known_device_uids = parsed_uids
    except Exception:
        pass  # Malformed string — fall back to empty list; node will log a warning.

    # -------------------------------------------------------------------------
    # Determine single vs multi-robot mode
    # -------------------------------------------------------------------------
    robot_names = []
    try:
        parsed = ast.literal_eval(robot_names_str)
        if isinstance(parsed, list) and len(parsed) > 0:
            robot_names = parsed
    except Exception:
        pass  # Fall back to single-robot mode.

    # -------------------------------------------------------------------------
    # Common node parameters (identical for single and multi-robot modes)
    # -------------------------------------------------------------------------
    def _node_params(robot_name: str) -> dict:
        return {
            'callsign':                robot_name,
            'tak_server_flow_tag_key': tak_server_flow_tag_key,
            # Topic names — robot-relative, resolved within the node namespace.
            'outgoing_cot_topic':      'send_to_tak',
            'incoming_cot_topic':      'incoming_cot',
            'navsat_topic':            'navsat',          # remapped below to active GPS
            'tak_chat_out_topic':      'tak_chat/out',
            'tak_chat_in_topic':       'tak_chat/in',
            'comms_topic':             'comms',
            # Allowed callsigns config — sourced from config/paths.yaml.
            'allowed_callsigns_file':  _paths['cot_runner_yaml'],
            # Timing and retry parameters.
            'send_delay_s':            1.0,
            'reply_delay_s':           1.0,
            'retry_timeout_s':         10.0,
            'retry_interval_s':        1.0,
            'min_retry_count':         1,
            # Pre-seeded UID map — ensures correct chatgrp routing before first
            # inbound message. Sourced from config/tak_params.yaml.
            'known_device_uids':       known_device_uids,
        }

    def _node_remappings(navsat_topic: str) -> list:
        # Remap tak_chat_node's internal 'navsat' topic to the active GPS topic.
        # navsat_topic is robot-relative, e.g. "sensors/ublox/fix".
        # Within the robot namespace this resolves to /warthog1/sensors/ublox/fix.
        return [('navsat', navsat_topic)]

    # =========================================================================
    # MULTI-ROBOT MODE
    # =========================================================================
    nodes = []

    if robot_names:
        print(f"\n{'='*80}")
        print(f"TAK CHAT MULTI-ROBOT MODE: Launching {len(robot_names)} robots")
        print(f"  navsat_topic → {navsat_topic}")
        print(f"{'='*80}")

        for idx, robot_name in enumerate(robot_names, 1):
            print(f"  [{idx}/{len(robot_names)}] Configuring TAK chat for: {robot_name}")
            nodes.append(
                Node(
                    package='tak_chat',
                    executable='tak_chat_node',
                    name=f'tak_chat_node_{robot_name}',
                    namespace=robot_name,
                    parameters=[_node_params(robot_name)],
                    remappings=_node_remappings(navsat_topic),
                    output='screen',
                    emulate_tty=True,
                )
            )

        print(f"{'='*80}\n")

    # =========================================================================
    # SINGLE-ROBOT MODE (DEFAULT)
    # =========================================================================
    else:
        print(f"\n{'='*80}")
        print(f"TAK CHAT SINGLE-ROBOT MODE: Launching {robot_name_single}")
        print(f"  navsat_topic → {navsat_topic}")
        print(f"{'='*80}\n")

        nodes.append(
            Node(
                package='tak_chat',
                executable='tak_chat_node',
                name='tak_chat_node',
                namespace=robot_name_single,
                parameters=[_node_params(robot_name_single)],
                remappings=_node_remappings(navsat_topic),
                output='screen',
                emulate_tty=True,
            )
        )

    return nodes


# =============================================================================
# LAUNCH DESCRIPTION
# =============================================================================

def generate_launch_description():

    # =========================================================================
    # LAUNCH ARGUMENTS
    # =========================================================================

    robot_name_arg = DeclareLaunchArgument(
        'robot_name',
        default_value='warthog1',
        description='Single robot name (ignored if robot_names is provided).',
    )

    robot_names_arg = DeclareLaunchArgument(
        'robot_names',
        default_value='[]',
        description=(
            "Python list of robot names for multi-robot mode. "
            "Overrides robot_name when non-empty. "
            "Example: \"['warthog1', 'warthog2', 'warthog3']\""
        ),
    )

    navsat_topic_arg = DeclareLaunchArgument(
        'navsat_topic',
        # Default is the geofog topic for backward compatibility.
        # sim_control.py passes the active profile's gps_topic_suffix here,
        # selecting between geofog (NAI_2, testing) and ublox (NAI_3, NAI_4).
        #
        # IMPORTANT: This must be ROBOT-RELATIVE (no leading slash, no robot_name).
        # The ROS2 namespace mechanism prepends the robot name automatically.
        # "sensors/ublox/fix" → /warthog1/sensors/ublox/fix  (warthog1 namespace)
        #
        # WHY THIS FIX: The previous version had this hardcoded to
        # "sensors/geofog/gps/fix" in both single and multi-robot modes.
        # That meant NAI_3 and NAI_4 runs used the wrong GPS topic — tak_chat
        # received no position data and TAK CoTs had stale or missing coordinates.
        default_value=_hw['gps_topic_suffix'],
        description=(
            "GPS topic to remap 'navsat' to within each robot namespace. "
            "Must be ROBOT-RELATIVE (no leading slash). "
            "Options: sensors/geofog/gps/fix (GeoFog, NAI_2) "
            "or sensors/ublox/fix (u-blox, NAI_3/NAI_4). "
            "Driven by gps_topic_suffix in the active profile (profiles.json)."
        ),
    )

    tak_server_flow_tag_key_arg = DeclareLaunchArgument(
        'tak_server_flow_tag_key',
        default_value=_tak['flow_tag'],      # from config/tak_params.yaml
        description=(
            "Flow tag key identifying the TAK server connection. "
            "Default from config/tak_params.yaml → tak.flow_tag."
        ),
    )

    known_device_uids_arg = DeclareLaunchArgument(
        'known_device_uids',
        default_value=str(_tak['known_device_uids']),  # from config/tak_params.yaml
        description=(
            "Pre-seeded callsign→device UID map for correct unicast routing "
            "before the first inbound message is received. "
            "Format: \"['CALLSIGN:ANDROID-hex', ...]\" "
            "Default from config/tak_params.yaml → tak.known_device_uids."
        ),
    )

    return LaunchDescription([
        robot_name_arg,
        robot_names_arg,
        navsat_topic_arg,
        tak_server_flow_tag_key_arg,
        known_device_uids_arg,
        OpaqueFunction(function=launch_tak_chat_nodes),
    ])
