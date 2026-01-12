cmake_minimum_required(VERSION 3.8)
project(tak_chat)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Require C++17 for std::optional, std::filesystem, etc.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# Find Dependencies
# =============================================================================
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(west_point_comms_sim REQUIRED)

# =============================================================================
# Generate Custom Messages
# =============================================================================
rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/TakChat.msg"
  DEPENDENCIES std_msgs
)

# =============================================================================
# Build Executable
# =============================================================================
add_executable(tak_chat_node
  src/tak_chat_node.cpp
)

# Include directories
target_include_directories(tak_chat_node PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# Link dependencies
ament_target_dependencies(tak_chat_node
  rclcpp
  std_msgs
  sensor_msgs
  west_point_comms_sim
)

# Link to generated messages from this package
rosidl_get_typesupport_target(cpp_typesupport_target ${PROJECT_NAME} "rosidl_typesupport_cpp")
target_link_libraries(tak_chat_node "${cpp_typesupport_target}")

# =============================================================================
# Install
# =============================================================================
# Install executable
install(TARGETS
  tak_chat_node
  DESTINATION lib/${PROJECT_NAME}
)

# Install headers
install(DIRECTORY include/
  DESTINATION include
)

# Install launch files
install(DIRECTORY launch/
  DESTINATION share/${PROJECT_NAME}/launch
)

# Install scripts
install(PROGRAMS
  scripts/tak_chat_test.py
  scripts/cot_monitor.py
  scripts/tak_chat_test_until_send_to_tak.py
  scripts/tak_chat_console.py
  DESTINATION lib/${PROJECT_NAME}
)

# =============================================================================
# Export for other packages
# =============================================================================
ament_export_include_directories(include)
ament_export_dependencies(rosidl_default_runtime rclcpp std_msgs sensor_msgs west_point_comms_sim)

# =============================================================================
# Testing (optional)
# =============================================================================
if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
