#!/bin/bash

# Function to detect the operating system
detect_os() {
    case "$(uname -s)" in
        Linux*)     OS=Linux;;
        Darwin*)    OS=Mac;;
        CYGWIN*)    OS=Cygwin;;
        MINGW*)     OS=MinGw;;
        *)          OS="UNKNOWN";;
    esac
    echo $OS
}

# Detect the operating system
OS=$(detect_os)
echo "Detected OS: $OS"

# Set the build directory
BUILD_DIR="build"

# Clean the build directory
echo "Cleaning the build directory..."
if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi
mkdir "$BUILD_DIR"
cd "$BUILD_DIR"

# Run CMake to configure and generate build files
echo "Configuring project with CMake..."
cmake -DCMAKE_BUILD_TYPE=Release ..
if [ $? -ne 0 ]; then
    echo "CMake configuration failed. Exiting."
    exit 1
fi

# Build the executable
echo "Building the project with $(nproc) threads..."
cmake --build . -- -j$(nproc)
if [ $? -ne 0 ]; then
    echo "Build failed. Exiting."
    exit 1
fi

# Set the executable name based on the operating system
if [ "$OS" == "Linux" ] || [ "$OS" == "Mac" ]; then
    EXECUTABLE="./bin/example"
elif [ "$OS" == "MinGw" ] || [ "$OS" == "Cygwin" ]; then
    EXECUTABLE="./bin/example.exe"
else
    echo "Unsupported OS. Exiting."
    exit 1
fi

# Check if the executable exists
if [ ! -x "$EXECUTABLE" ]; then
    echo "Executable not found or is not executable. Exiting."
    exit 1
fi

cd ..

# Run the executable
echo "Running the executable..."
$BUILD_DIR/bin/example

# Check if the executable ran successfully
if [ $? -ne 0 ]; then
    echo "Executable failed to run. Exiting."
    exit 1
fi

echo "Execution completed successfully."
