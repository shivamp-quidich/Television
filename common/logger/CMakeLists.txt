# ==============================================================================
# Logger Library
# ==============================================================================

cmake_minimum_required(VERSION 3.10)
project(logger LANGUAGES CXX)

# ==============================================================================
# Find Required Packages
# ==============================================================================

find_package(spdlog REQUIRED)

# ==============================================================================
# Source Files
# ==============================================================================

set(LOGGER_SOURCES
    src/logger.cpp
)

set(LOGGER_HEADERS
    include/logger.h
)

# ==============================================================================
# Logger Static Library
# ==============================================================================

add_library(logger STATIC
    ${LOGGER_SOURCES}
    ${LOGGER_HEADERS}
)

# ==============================================================================
# Include Directories
# ==============================================================================

target_include_directories(logger 
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# ==============================================================================
# Link Libraries
# ==============================================================================

target_link_libraries(logger 
    PUBLIC
        spdlog::spdlog              # Exposed in logger.h
)

# ==============================================================================
# Compiler Features
# ==============================================================================

target_compile_features(logger PUBLIC cxx_std_17)

# ==============================================================================
# Status Messages
# ==============================================================================

message(STATUS "Logger library configuration:")
message(STATUS "  spdlog version: ${spdlog_VERSION}")
message(STATUS "  Sources: ${LOGGER_SOURCES}")

