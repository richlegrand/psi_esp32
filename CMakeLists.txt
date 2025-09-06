cmake_minimum_required(VERSION 3.16)

# ESP32-P4 libdatachannel port configuration
set(IDF_TARGET "esp32p4" CACHE STRING "ESP32 target")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project configuration
project(esp32_libdatachannel)

# libdatachannel configuration flags for minimal build
set(NO_MEDIA ON CACHE BOOL "Disable media support")
set(NO_WEBSOCKET ON CACHE BOOL "Disable WebSocket support") 
set(USE_MBEDTLS ON CACHE BOOL "Use MbedTLS instead of OpenSSL")
set(NO_EXAMPLES ON CACHE BOOL "Skip building examples")
set(NO_TESTS ON CACHE BOOL "Skip building tests")

# Component paths
set(LIBDATACHANNEL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../libdatachannel)
set(LIBDATACHANNEL_DEPS ${LIBDATACHANNEL_ROOT}/deps)

# Add components
set(EXTRA_COMPONENT_DIRS 
    ${CMAKE_CURRENT_SOURCE_DIR}/components
)

# Memory configuration for ESP32-P4
set(CONFIG_SPIRAM_USE_MALLOC 1)
set(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 16384)

# C++ standard required by libdatachannel
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)