cmake_minimum_required(VERSION 3.20)
project(ArenaCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "build type" FORCE)
endif()

# /Zc:__cplusplus makes __cplusplus report the real standard instead of 199711L.
# /permissive- turns on conformant mode; /utf-8 keeps source and execution charset UTF-8.
set(ARENA_CXX_FLAGS /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc)

# ---------------------------------------------------------------- core library
add_library(arena_base STATIC
    src/net/codec.cpp
    src/net/iocp_service.cpp
    src/net/acceptor.cpp
    src/net/session.cpp
    src/net/session_manager.cpp
    src/base/log.cpp
    src/base/resp_client.cpp
    src/game/room.cpp
    src/game/aoi.cpp
    src/game/snapshot_codec.cpp
)
target_include_directories(arena_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(arena_base PRIVATE ${ARENA_CXX_FLAGS})
# MSVC 会把 fopen 这类 C 函数标成 C4996「不安全」，这是它的历史包袱不是真缺陷，
# 关掉这条噪音，好让 /W4 下剩下的警告都是真问题。
target_compile_definitions(arena_base PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(arena_base PUBLIC ws2_32)

# ---------------------------------------------------------------- unit tests
enable_testing()
add_executable(arena_tests tests/test_main.cpp)
target_link_libraries(arena_tests PRIVATE arena_base)
target_compile_options(arena_tests PRIVATE ${ARENA_CXX_FLAGS})
add_test(NAME arena_unit COMMAND arena_tests)

# ---------------------------------------------------------------- smoke tools
add_executable(iocp_smoke tools/iocp_service_smoke.cpp)
target_link_libraries(iocp_smoke PRIVATE arena_base)
target_compile_options(iocp_smoke PRIVATE ${ARENA_CXX_FLAGS})

add_executable(echo_smoke tools/echo_server_smoke.cpp)
target_link_libraries(echo_smoke PRIVATE arena_base)
target_compile_options(echo_smoke PRIVATE ${ARENA_CXX_FLAGS})

add_executable(echo_client tools/client_smoke.cpp)
target_link_libraries(echo_client PRIVATE arena_base)
target_compile_options(echo_client PRIVATE ${ARENA_CXX_FLAGS})

add_executable(logic_frame_smoke tools/logic_frame_smoke.cpp)
target_link_libraries(logic_frame_smoke PRIVATE arena_base)
target_compile_options(logic_frame_smoke PRIVATE ${ARENA_CXX_FLAGS})

add_executable(aoi_smoke tools/aoi_smoke.cpp)
target_link_libraries(aoi_smoke PRIVATE arena_base)
target_compile_options(aoi_smoke PRIVATE ${ARENA_CXX_FLAGS})

add_executable(combat_smoke tools/combat_smoke.cpp)
target_link_libraries(combat_smoke PRIVATE arena_base)
target_compile_options(combat_smoke PRIVATE ${ARENA_CXX_FLAGS})

add_executable(snapshot_smoke tools/snapshot_smoke.cpp)
target_link_libraries(snapshot_smoke PRIVATE arena_base)
target_compile_options(snapshot_smoke PRIVATE ${ARENA_CXX_FLAGS})

add_executable(game_server tools/game_server_smoke.cpp)
target_link_libraries(game_server PRIVATE arena_base)
target_compile_options(game_server PRIVATE ${ARENA_CXX_FLAGS})

add_executable(bot_client tools/bot_client.cpp)
target_link_libraries(bot_client PRIVATE arena_base)
target_compile_options(bot_client PRIVATE ${ARENA_CXX_FLAGS})

add_executable(ring_lab tools/ring_lab.cpp)
target_link_libraries(ring_lab PRIVATE arena_base)
target_compile_options(ring_lab PRIVATE ${ARENA_CXX_FLAGS})

add_executable(redis_demo tools/redis_demo.cpp)
target_link_libraries(redis_demo PRIVATE arena_base)
target_compile_options(redis_demo PRIVATE ${ARENA_CXX_FLAGS})

add_executable(hist_lab tools/hist_lab.cpp)
target_link_libraries(hist_lab PRIVATE arena_base)
target_compile_options(hist_lab PRIVATE ${ARENA_CXX_FLAGS})
