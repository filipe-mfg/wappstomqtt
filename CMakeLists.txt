cmake_minimum_required(VERSION 3.16)
project(tb_wappsto_bridge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# POSIX compliance
add_compile_definitions(_POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)

# -----------------------------------------------------------
# Dependencies
# -----------------------------------------------------------

# nlohmann/json (header-only, fetched at configure time)
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# OpenSSL (for TLS connection to Wappsto)
find_package(OpenSSL REQUIRED)

# libmosquitto (for MQTT connection to ThingsBoard)
find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h REQUIRED)

# -----------------------------------------------------------
# Sources
# -----------------------------------------------------------

set(SOURCES
    src/main.cpp
    src/Config.cpp
    src/ThingsBoardClient.cpp
    src/WappstoClient.cpp
    src/Bridge.cpp
)

add_executable(tb_wappsto_bridge ${SOURCES})

target_include_directories(tb_wappsto_bridge PRIVATE
    src
    ${MOSQUITTO_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(tb_wappsto_bridge PRIVATE
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    ${MOSQUITTO_LIB}
    pthread
)

# -----------------------------------------------------------
# Install
# -----------------------------------------------------------

install(TARGETS tb_wappsto_bridge DESTINATION bin)
install(FILES config.example.json DESTINATION etc/tb_wappsto_bridge RENAME config.json)

# -----------------------------------------------------------
# Summary
# -----------------------------------------------------------

message(STATUS "Build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
message(STATUS "Mosquitto lib  : ${MOSQUITTO_LIB}")
