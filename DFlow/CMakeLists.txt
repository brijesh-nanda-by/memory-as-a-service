cmake_minimum_required(VERSION 3.10)
# project(DistributedCache CXX)
project(worker_node)

set(CMAKE_CXX_STANDARD 17) # Or higher if you prefer
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pthread")

# Path to the httplib and nlohmann/json headers
# Assuming they are in the same directory as your source files
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/include)
# Add controller executable
add_executable(controller controller.cpp)
# Link any necessary system libraries (e.g., for networking on Linux)
# target_link_libraries(controller pthread) # Example for Linux threading

# Add worker_node executable
add_executable(worker_node worker_node.cpp)
target_link_libraries(worker_node pthread) # Example for Linux threading