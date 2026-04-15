# Example CMake command line to create project build files:
#
# *** Windows ***
# cmake -G "Visual Studio 17 2022" -A Win32 -B Build -S .
#
# *** Linux ***
# cmake -G "Unix Makefiles" -B Build -S .

# Specify the minimum CMake version required
cmake_minimum_required(VERSION 3.10)

# Project name and language (C or C++)
project(StdWorkerThread VERSION 1.0 LANGUAGES CXX)

# Require C++17 for std::optional and std::chrono features
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Collect all .cpp source files in the current directory
file(GLOB SOURCES "${CMAKE_SOURCE_DIR}/*.cpp" "${CMAKE_SOURCE_DIR}/*.h")

# Add an executable target
add_executable(StdWorkerThreadApp ${SOURCES})

# Set StdWorkerThreadApp as the default startup project in Visual Studio
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT StdWorkerThreadApp)

# Link pthreads on Linux for thread naming support
if(UNIX)
    target_link_libraries(StdWorkerThreadApp PRIVATE pthread)
endif()




