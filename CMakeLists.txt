cmake_minimum_required(VERSION 3.20)
project(niri-island CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-O3 -Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED
    wayland-client
    wayland-scanner
    cairo
    pangocairo
    gdk-pixbuf-2.0
    libsystemd
    libpulse
    libudev
)

pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)

# Wayland protocol code generation
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocols)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

macro(generate_wayland_proto PROTO_NAME XML_PATH)
    add_custom_command(
        OUTPUT ${GEN_DIR}/${PROTO_NAME}-protocol.c
        COMMAND ${WAYLAND_SCANNER} private-code ${XML_PATH} ${GEN_DIR}/${PROTO_NAME}-protocol.c
        DEPENDS ${XML_PATH}
    )
    add_custom_command(
        OUTPUT ${GEN_DIR}/${PROTO_NAME}-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} client-header ${XML_PATH} ${GEN_DIR}/${PROTO_NAME}-client-protocol.h
        DEPENDS ${XML_PATH}
    )
endmacro()

generate_wayland_proto(xdg-shell ${PROTO_DIR}/xdg-shell.xml)
generate_wayland_proto(wlr-layer-shell-unstable-v1 ${PROTO_DIR}/wlr-layer-shell-unstable-v1.xml)

set(PROTO_SRCS
    ${GEN_DIR}/xdg-shell-protocol.c
    ${GEN_DIR}/xdg-shell-client-protocol.h
    ${GEN_DIR}/wlr-layer-shell-unstable-v1-protocol.c
    ${GEN_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h
)

add_executable(niri-island
    src/main.cpp
    src/wayland.cpp
    src/renderer.cpp
    src/mpris.cpp
    src/osd.cpp
    src/notifications.cpp
    src/island.cpp
    ${PROTO_SRCS}
)

target_include_directories(niri-island PRIVATE
    ${GEN_DIR}
    ${DEPS_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(niri-island PRIVATE -Wall -Wextra -O2)
target_link_libraries(niri-island PRIVATE ${DEPS_LIBRARIES} m rt pthread)
