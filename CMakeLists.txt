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
generate_wayland_proto(linux-dmabuf-v1 ${WL_PROTO_DIR}/stable/linux-dmabuf/linux-dmabuf-v1.xml)

# waywallen's renderer bridge (MIT, third_party/waywallen-bridge): its wire
# codec, linked in (web wallpapers: we talk to waywallen's CEF renderer as its
# daemon would); and the whole library, which that renderer loads.
set(WWB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/waywallen-bridge)
add_library(ww-codec STATIC ${WWB_DIR}/src/ipc_v3.c ${WWB_DIR}/src/bridge.c ${WWB_DIR}/src/drm_fourcc.c)
target_include_directories(ww-codec PUBLIC ${WWB_DIR}/include PRIVATE ${WWB_DIR}/src)
target_compile_definitions(ww-codec PRIVATE _GNU_SOURCE)
set_target_properties(ww-codec PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
pkg_check_modules(WWB_DEPS IMPORTED_TARGET egl gbm vulkan)
if(WWB_DEPS_FOUND)
    file(GLOB WWB_SRCS ${WWB_DIR}/src/*.c)
    add_library(waywallen-bridge SHARED ${WWB_SRCS})
    target_include_directories(waywallen-bridge PRIVATE ${WWB_DIR}/include ${WWB_DIR}/src)
    target_compile_definitions(waywallen-bridge PRIVATE _GNU_SOURCE)
    target_link_libraries(waywallen-bridge PRIVATE PkgConfig::WWB_DEPS Threads::Threads)
    target_link_options(waywallen-bridge PRIVATE -Wl,--version-script=${WWB_DIR}/waywallen-bridge.map)
    set_target_properties(waywallen-bridge PROPERTIES C_STANDARD 99)
    install(TARGETS waywallen-bridge LIBRARY DESTINATION lib/niri-island)
endif()

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
target_link_libraries(island-core PUBLIC ${DEPS_LIBRARIES} Threads::Threads m rt pam ww-codec)

add_executable(niri-island src/main.cpp)
target_link_libraries(niri-island PRIVATE island-core)


install(TARGETS niri-island RUNTIME DESTINATION bin)

# The wallpaper engine (Rust, engine/): Wallpaper Engine scenes and videos;
# and the Steam Workshop helper.
option(NIRI_ISLAND_ENGINE "Build niri-island-engine (needs cargo)" ON)
find_program(CARGO cargo)
if(NIRI_ISLAND_ENGINE AND CARGO)
    set(ENGINE_OUT ${CMAKE_CURRENT_BINARY_DIR}/engine/release)
    add_custom_target(engine ALL
        COMMAND ${CARGO} build --release --workspace --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/engine/Cargo.toml
                --target-dir ${CMAKE_CURRENT_BINARY_DIR}/engine
        # the Steam helper finds libsteam_api.so next to itself
        COMMAND ${CMAKE_COMMAND} -DOUT=${ENGINE_OUT} -P ${CMAKE_CURRENT_SOURCE_DIR}/engine/copy-steam-lib.cmake
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/engine
        USES_TERMINAL
        COMMENT "cargo: niri-island-engine, niri-island-steam")
    install(PROGRAMS ${ENGINE_OUT}/niri-island-engine ${ENGINE_OUT}/niri-island-engine-vk ${ENGINE_OUT}/niri-island-engine-still
            ${ENGINE_OUT}/niri-island-steam DESTINATION bin)
    install(FILES ${ENGINE_OUT}/libsteam_api.so DESTINATION bin)
endif()

# Tests: cmake --build build && ctest --test-dir build --output-on-failure
enable_testing()
file(GLOB TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(niri-island-tests ${TEST_SRCS})
target_include_directories(niri-island-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(niri-island-tests PRIVATE island-core)
add_test(NAME unit COMMAND niri-island-tests)
