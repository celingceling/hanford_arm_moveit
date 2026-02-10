cmake_minimum_required(VERSION 3.8)
project(isaac_joint_command_relay)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(control_msgs REQUIRED)

add_executable(isaac_joint_command_relay_node src/isaac_joint_command_relay.cpp)
ament_target_dependencies(isaac_joint_command_relay_node
  rclcpp sensor_msgs control_msgs)

install(TARGETS isaac_joint_command_relay_node
  DESTINATION lib/${PROJECT_NAME})

ament_package()
