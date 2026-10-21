cmake_minimum_required(VERSION 3.20)
project(niri-island VERSION 2.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(DEPS REQUIRED
    wayland-client
    wayland-scanner
    wayland-protocols
    cairo
    pangocairo
    gdk-pixbuf-2.0
    libsystemd
    libpulse
    libudev
    xkbcommon
    sqlite3
    libcurl
    libqrencode
    nlohmann_json
)

pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
pkg_get_variable(WL_PROTO_DIR wayland-protocols pkgdatadir)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocols)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

set(PROTO_SRCS)
macro(generate_wayland_proto NAME XML)
    add_custom_command(
        OUTPUT ${GEN_DIR}/${NAME}-protocol.c ${GEN_DIR}/${NAME}-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} private-code ${XML} ${GEN_DIR}/${NAME}-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header ${XML} ${GEN_DIR}/${NAME}-client-protocol.h
        DEPENDS ${XML}
    )
    list(APPEND PROTO_SRCS ${GEN_DIR}/${NAME}-protocol.c ${GEN_DIR}/${NAME}-client-protocol.h)
endmacro()

generate_wayland_proto(xdg-shell ${PROTO_DIR}/xdg-shell.xml)
generate_wayland_proto(wlr-layer-shell-unstable-v1 ${PROTO_DIR}/wlr-layer-shell-unstable-v1.xml)
generate_wayland_proto(tablet-v2 ${WL_PROTO_DIR}/stable/tablet/tablet-v2.xml)
generate_wayland_proto(viewporter ${WL_PROTO_DIR}/stable/viewporter/viewporter.xml)
generate_wayland_proto(cursor-shape-v1 ${WL_PROTO_DIR}/staging/cursor-shape/cursor-shape-v1.xml)
generate_wayland_proto(ext-background-effect-v1 ${WL_PROTO_DIR}/staging/ext-background-effect/ext-background-effect-v1.xml)
generate_wayland_proto(ext-idle-notify-v1 ${WL_PROTO_DIR}/staging/ext-idle-notify/ext-idle-notify-v1.xml)
generate_wayland_proto(ext-session-lock-v1 ${WL_PROTO_DIR}/staging/ext-session-lock/ext-session-lock-v1.xml)

file(GLOB_RECURSE CORE_SRCS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER CORE_SRCS EXCLUDE REGEX ".*/src/main\\.cpp$")

# Everything but the entry point, shared by the shell and the tests.
add_library(island-core STATIC ${CORE_SRCS} ${PROTO_SRCS})
target_include_directories(island-core PUBLIC
    ${GEN_DIR}
    ${DEPS_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(island-core PUBLIC -Wall -Wextra -Wno-missing-field-initializers
    $<$<CONFIG:Release>:-O2>)
target_link_libraries(island-core PUBLIC ${DEPS_LIBRARIES} Threads::Threads m rt pam)

add_executable(niri-island src/main.cpp)
target_link_libraries(niri-island PRIVATE island-core)


install(TARGETS niri-island RUNTIME DESTINATION bin)

# Tests: cmake --build build && ctest --test-dir build --output-on-failure
enable_testing()
file(GLOB TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(niri-island-tests ${TEST_SRCS})
target_include_directories(niri-island-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(niri-island-tests PRIVATE island-core)
add_test(NAME unit COMMAND niri-island-tests)
