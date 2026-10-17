# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
# This board's component include paths exceed Windows CreateProcess limits.
# Ninja response files also make a clean Windows build independent of cwd length.
set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "Use response files" FORCE)

# Based on xiaozhi-esp32 v2.0.5. PROJECT_VER stays 2.1.0 to match the firmware
# version reported by this board, so OTA version checks remain consistent.
set(PROJECT_VER "2.1.0")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

