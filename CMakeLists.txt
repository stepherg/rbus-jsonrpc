cmake_minimum_required(VERSION 3.10)
project(rbus_jsonrpc C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find required libraries
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)

find_library(WEBSOCKETS_LIBRARIES NAMES websockets libwebsockets)
find_path(WEBSOCKETS_INCLUDE_DIRS libwebsockets.h)

find_library(JANSSON_LIBS NAMES jansson libjansson)
find_path(JANSSON_INCLUDE_DIRS jansson.h)

find_library(RBUS_LIBRARY NAMES rbus)
find_path(RBUS_INCLUDE_DIR NAMES rbus.h PATH_SUFFIXES rbus)

# Include directories
include_directories(
    ${OPENSSL_INCLUDE_DIR}
    ${WEBSOCKETS_INCLUDE_DIRS}
    ${JANSSON_INCLUDE_DIRS}
    ${RBUS_INCLUDE_DIR}
)

# Add executable
add_executable(rbus_jsonrpc rbus_jsonrpc.c)

# Link libraries
target_link_libraries(rbus_jsonrpc
    ${OPENSSL_LIBRARIES}
    ${WEBSOCKETS_LIBRARIES}
    ${JANSSON_LIBS}
    ${RBUS_LIBRARY}
)

# Add compiler flags
target_compile_options(rbus_jsonrpc PRIVATE
    ${WEBSOCKETS_CFLAGS_OTHER}
    ${JANSSON_CFLAGS_OTHER}
)

# Installation rules
install(TARGETS rbus_jsonrpc
    RUNTIME DESTINATION bin
)
