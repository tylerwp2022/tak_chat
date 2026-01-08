//------------------------------------------------------------------------------
// tak_chat_node main.cpp
//
// Standalone ROS2 node for sending TAK/ATAK chat messages.
// Run with: ros2 run <package> tak_chat_node
//
// Parameters:
//   callsign                 - Your TAK callsign (default: warthog1)
//   android_id               - Device ID for GeoChat UID (default: 07ac9cbb59cdc215)
//   tak_server_flow_tag_key  - Flow tag attribute name
//   outgoing_cot_topic       - Topic to publish CoT messages (default: send_to_tak)
//   navsat_topic             - NavSatFix subscription topic (default: navsat)
//   chat_request_topic       - Topic for incoming chat requests (default: chat_request)
//
// Example:
//   ros2 run your_package tak_chat_node --ros-args
//     -p callsign:=warthog1
//     -r navsat:=/warthog1/sensors/ublox/fix
//     -r send_to_tak:=/warthog1/send_to_tak
//     -r chat_request:=/warthog1/chat_request
//------------------------------------------------------------------------------

#include <tak_chat/tak_chat_node.hpp>

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  
  auto node = std::make_shared<TakChatNode>();
  
  RCLCPP_INFO(node->get_logger(), "TakChatNode starting...");
  
  rclcpp::spin(node);
  
  rclcpp::shutdown();
  return 0;
}
