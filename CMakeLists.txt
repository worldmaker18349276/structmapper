cmake_minimum_required(VERSION 3.0.2)
project(structmapper)

find_package(catkin REQUIRED COMPONENTS
  rospy
)
find_package(nlohmann_json REQUIRED)

catkin_python_setup()

catkin_package(
  INCLUDE_DIRS include
  DEPENDS nlohmann_json
)

include_directories(
  include
  ${nlohmann_json_INCLUDE_DIRS}
)
