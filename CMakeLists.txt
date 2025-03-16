# Specify the minimum version for CMake
cmake_minimum_required(VERSION 3.15)

# Project's name
project(ExampleProject VERSION 1.0)

# Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Enable all compiler warnings except for unused variables
add_compile_options(-Wall -Wextra -Wno-unused-variable -Wno-unused-parameter)

# Include directories
include_directories(include)

# Set the source directory
set(SRCDIR ${CMAKE_SOURCE_DIR}/src)

file(GLOB SOURCES "${SRCDIR}/*.cpp")

# Add the executable
add_executable(example ${SOURCES})

# Specify the output directory for the executable
set_target_properties(example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Link required libraries
target_link_libraries(example PRIVATE
    pthread
    cpprest
    ssl
    crypto
    boost_system
    boost_thread
)

# Set optimization level for Release builds
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
