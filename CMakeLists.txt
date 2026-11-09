cmake_minimum_required(VERSION 3.15)

project(PeacockLauncher CXX)

# Find latest version at https://github.com/OrfeasZ/ZHMModSDK/releases
# Set ZHMMODSDK_DIR variable to a local directory to use a local copy of the ZHMModSDK.
set(ZHMMODSDK_VER "v4.2.1")
include(cmake/setup-zhmmodsdk.cmake)

# Set C++ standard to C++23.
set(CMAKE_CXX_STANDARD 23)

# Set version numbers
set(VERSION_MAJOR 1)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)
set(VERSION_BUILD 0)

# Configure the version.rc file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    @ONLY
)

# Create the PeacockLauncher mod library.
add_library(PeacockLauncher SHARED
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    src/PeacockLauncher.cpp
    src/PeacockLauncher.h
    src/PeacockLogViewer.cpp
    src/PeacockLogViewer.h
    src/PeacockUpdater.cpp
    src/PeacockUpdater.h
)

# Set UTF-8 flag.
set_target_properties(PeacockLauncher PROPERTIES COMPILE_FLAGS "/utf-8")

# Add SDK and DX headers dependencies.
find_package(directx-headers CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(PeacockLauncher
    ZHMModSDK
    Microsoft::DirectX-Guids
    Microsoft::DirectX-Headers
    implot
    miniz::miniz
    nlohmann_json::nlohmann_json
    winhttp
    ws2_32
    version
)

install(TARGETS PeacockLauncher
    RUNTIME DESTINATION bin
)

# Install the mod to the game folder when the `GAME_INSTALL_PATH` variable is set.
zhmmodsdk_install(PeacockLauncher)
