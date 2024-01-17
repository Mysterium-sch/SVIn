cmake_minimum_required(VERSION 2.8.3)
project(pose_graph)

if(CMAKE_BUILD_TYPE)
    message(STATUS "Build type specified as ${CMAKE_BUILD_TYPE}")
else()
    message(STATUS "Build type not specified, using Release")
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 14)

# changed to -march=native. Working in Ubuntu 18.04 and 20.04
set(CMAKE_CXX_FLAGS "-march=native -fPIC -Wall")

find_package(catkin REQUIRED COMPONENTS
    roscpp
    rospy
    std_msgs
    nav_msgs
    cv_bridge
    roslib
    pcl_ros
    pcl_conversions
    image_transport
    message_generation
    sensor_msgs
    tf2
    tf2_ros
    tf2_sensor_msgs
    tf2_geometry_msgs
    message_generation
    std_srvs
    nav_msgs
)

add_message_files(
    FILES
    SvinHealth.msg
)

generate_messages(
    DEPENDENCIES
    std_msgs
    geometry_msgs
    nav_msgs
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

find_package(OpenCV)
find_package(PCL REQUIRED)
find_package(Boost COMPONENTS filesystem system REQUIRED)
find_package(Glog REQUIRED)

# brisk
find_package(brisk 2 REQUIRED)
include_directories(BEFORE ${BRISK_INCLUDE_DIRS})
message(STATUS "Using system brisk. Found at ${BRISK_INCLUDE_DIRS}.")

find_package(Ceres REQUIRED)

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
find_package(Eigen3)

include_directories(BEFORE ${GLOG_INCLUDE_DIRS})
include_directories(include
                    ThirdParty
                    ${catkin_INCLUDE_DIRS}
                    ${CERES_INCLUDE_DIRS}
                    ${EIGEN3_INCLUDE_DIR}
                    ${BRISK_INCLUDE_DIRS}
                    ${PCL_INCLUDE_DIRS}
                    ${Boost_INCLUDE_DIRS})

catkin_package(CATKIN_DEPENDS pcl_ros pcl_conversions)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/okvis/cmake")
find_package(VISensor)

if(VISENSORDRIVER_FOUND)
    add_definitions(-DHAVE_LIBVISENSOR)
    message(STATUS "Found libvisensor in ROS interface.")
    include_directories(${VISensorDriver_INCLUDE_DIR})
else()
    message(WARNING "libvisensor not found")
endif()

include_directories(include)
include_directories(
    ${catkin_INCLUDE_DIRS}
    ${CMAKE_BINARY_DIR}/include
)

add_definitions(-fPIC -march=native -Wextra -Winit-self -Woverloaded-virtual
                -Wnon-virtual-dtor -Wsign-promo -Wno-long-long -std=c++14)

# Add Utils.h to the list of header files
set(HEADER_FILES include/utils/Utils.h)

add_library(${PROJECT_NAME} 
    src/pose_graph/GlobalMapping.cpp
    src/pose_graph/Keyframe.cpp
    src/pose_graph/LoopClosure.cpp
    src/pose_graph/Parameters.cpp
    src/pose_graph/PoseGraph.cpp
    src/pose_graph/Publisher.cpp
    src/pose_graph/Subscriber.cpp
    src/pose_graph/SwitchingEstimator.cpp
    src/utils/CameraPoseVisualization.cpp
    src/utils/UtilsOpenCV.cpp
    src/utils/Utils.cpp
    src/utils/Statistics.cpp
    ${HEADER_FILES}  # Add Utils.h here
    ThirdParty/DBoW/BowVector.cpp
    ThirdParty/DBoW/FBrief.cpp
    ThirdParty/DBoW/FeatureVector.cpp
    ThirdParty/DBoW/QueryResults.cpp
    ThirdParty/DBoW/ScoringObject.cpp
    ThirdParty/DUtils/Random.cpp
    ThirdParty/DUtils/Timestamp.cpp
    ThirdParty/DVision/BRIEF.cpp
    ThirdParty/VocabularyBinary.cpp
)

# Link Utils.h with the library
target_include_directories(${PROJECT_NAME} PUBLIC include/utils)

target_link_libraries(${PROJECT_NAME}
    opengv
    ${catkin_LIBRARIES}
    ${OpenCV_LIBS}
    ${CERES_LIBRARIES} 
    ${BRISK_LIBRARIES}
    ${PCL_LIBRARIES}
    ${Boost_LIBRARIES}
) 

# Build Utils.h target
add_executable(${PROJECT_NAME}_utils include/utils/Utils.h)
target_link_libraries(${PROJECT_NAME}_utils ${PROJECT_NAME})

add_executable(${PROJECT_NAME}_node src/pose_graph_node.cpp)
target_link_libraries(${PROJECT_NAME}_node ${PROJECT_NAME})
