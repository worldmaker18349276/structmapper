cmake_minimum_required(VERSION 3.0.2)
project(structmapper)

find_package(catkin REQUIRED COMPONENTS
  rospy
  roscpp
  rostest
)
find_package(nlohmann_json REQUIRED)

catkin_python_setup()

catkin_package(
  INCLUDE_DIRS include
  CATKIN_DEPENDS roscpp
  DEPENDS nlohmann_json
  CFG_EXTRAS structmapper-extras.cmake
)

include_directories(
  include
  ${nlohmann_json_INCLUDE_DIRS}
  ${catkin_INCLUDE_DIRS}
)

# files needed by structmapper_generate_schemas()/structmapper_generate_python_schemas() (see cmake/structmapper-extras.cmake)
install(DIRECTORY cmake/
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}/cmake
  FILES_MATCHING PATTERN "*.in" PATTERN "*.py"
)

install(DIRECTORY include/${PROJECT_NAME}/
  DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION}
  FILES_MATCHING PATTERN "*.hpp"
  PATTERN ".svn" EXCLUDE
)

if(CATKIN_ENABLE_TESTING)
  catkin_add_gtest(test_structmapper test/test_structmapper.cpp)
  if(TARGET test_structmapper)
    target_link_libraries(test_structmapper
      ${catkin_LIBRARIES}
    )
  endif()

  catkin_add_nosetests(test)
endif()
