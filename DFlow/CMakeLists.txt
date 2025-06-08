cmake_minimum_required(VERSION 3.10)
project(DistributedCache CXX)

set(CMAKE_CXX_STANDARD 17) # Or higher if you prefer

# Path to the httplib and nlohmann/json headers
# Assuming they are in the same directory as your source files
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Add controller executable
add_executable(controller controller.cpp)
# Link any necessary system libraries (e.g., for networking on Linux)
# target_link_libraries(controller pthread) # Example for Linux threading

# Add worker_node executable
add_executable(worker_node worker_node.cpp)
# target_link_libraries(worker_node pthread) # Example for Linux threading