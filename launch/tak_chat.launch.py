"""
Launch file for tak_chat_node.

This launches the TAK chat node which handles CoT message formatting
and publishing for behavior tree nodes and other ROS2 components.

Usage:
  ros2 launch tak_chat tak_chat.launch.py

With arguments:
  ros2 launch tak_chat tak_chat.launch.py \
    namespace:=warthog1 \
    callsign:=warthog1

With config file:
  ros2 launch tak_chat tak_chat.launch.py \
    config_file:=/path/to/config.yaml
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    # Declare launch arguments
    namespace_arg = DeclareLaunchArgument(
        'namespace',
        default_value='warthog1',
        description='Robot namespace'
    )

    callsign_arg = DeclareLaunchArgument(
        'callsign',
        default_value='warthog1',
        description='TAK callsign for this robot'
    )

    android_id_arg = DeclareLaunchArgument(
        'android_id',
        default_value='dfac01d76beec661',
        description='Android device ID for GeoChat UID'
    )

    tak_server_flow_tag_key_arg = DeclareLaunchArgument(
        'tak_server_flow_tag_key',
        default_value='TAK-Server-d520578543014e9cba1916fad77b9917',
        description='Flow tag key for TAK server'
    )

    # Get launch configurations
    namespace = LaunchConfiguration('namespace')
    callsign = LaunchConfiguration('callsign')
    android_id = LaunchConfiguration('android_id')
    tak_server_flow_tag_key = LaunchConfiguration('tak_server_flow_tag_key')

    # Create the node
    tak_chat_node = Node(
        package='tak_chat',
        executable='tak_chat_node',
        name='tak_chat_node',
        namespace=namespace,
        parameters=[{
        'callsign': callsign,
        'android_id': android_id,
        'tak_server_flow_tag_key': tak_server_flow_tag_key,
        'outgoing_cot_topic': 'send_to_tak',
        'incoming_cot_topic': 'incoming_cot',
        'navsat_topic': 'navsat',
        'tak_chat_out_topic': 'tak_chat/out',
        'tak_chat_in_topic':  'tak_chat/in',
        }],

        remappings=[
            # Remap to full topic paths within namespace
            # These can be overridden via command line if needed
            ('navsat', 'sensors/ublox/fix'),
        ],
        output='screen',
        emulate_tty=True,
    )

    return LaunchDescription([
        namespace_arg,
        callsign_arg,
        android_id_arg,
        tak_server_flow_tag_key_arg,
        tak_chat_node,
    ])
