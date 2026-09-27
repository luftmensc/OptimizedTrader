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

# Original example: rolling optimization without trading costs
add_executable(example
    ${SRCDIR}/example.cpp
    ${SRCDIR}/FibAlgoTrader.cpp
    ${SRCDIR}/HelperFunctions.cpp
)

# Walk forward backtest with trading costs and performance report
add_executable(backtest
    ${SRCDIR}/backtest.cpp
    ${SRCDIR}/BreakoutStrategy.cpp
    ${SRCDIR}/MarketData.cpp
    ${SRCDIR}/Performance.cpp
    ${SRCDIR}/WalkForward.cpp
)

# Specify the output directory for the executables
set_target_properties(example backtest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Link required libraries
target_link_libraries(example PRIVATE pthread)
target_link_libraries(backtest PRIVATE pthread)

# Set optimization level for Release builds
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
