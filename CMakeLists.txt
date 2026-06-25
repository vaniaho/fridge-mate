# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart_fridge)

# esp_websocket_client 1.7.0 leaves optional event diagnostics uninitialized
# and consumes a va_list twice. Apply the small compatibility fix after the
# component manager has materialized managed_components.
include(${CMAKE_SOURCE_DIR}/cmake/patch_esp_websocket_client.cmake)
