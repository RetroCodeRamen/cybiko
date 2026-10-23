cmake_minimum_required(VERSION 3.16)
project(cybiko-emulator C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(cybiko_core STATIC
    src/core/memory.c
    src/core/hd66421.c
    src/core/timer8.c
    src/core/timer16.c
    src/core/rtc.c
    src/core/speaker.c
    src/core/keyboard.c
    src/core/h8s_cpu.c
    src/core/address_bus.c
    src/core/cfs.c
    src/core/emulator.c
    src/radio/radio_link.c
    src/radio/radio_coproc.c
    src/radio/radio_sim.c
    src/radio/sci2_xt.c
    src/radio/radio_lora.c
    src/radio/radio_serial_phy.c
)
target_include_directories(cybiko_core PUBLIC src src/radio)

option(CYBIKO_TRACE "Enable CPU instruction tracing" OFF)
if(CYBIKO_TRACE)
    target_compile_definitions(cybiko_core PUBLIC CYBIKO_TRACE=1)
endif()

find_package(SDL2 QUIET)
if(SDL2_FOUND)
    add_executable(cybiko-emu src/linux/main.c)
    target_link_libraries(cybiko-emu cybiko_core SDL2::SDL2)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    add_executable(cybiko-emu src/linux/main.c)
    target_link_libraries(cybiko-emu cybiko_core ${SDL2_LIBRARIES})
    target_include_directories(cybiko-emu PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

# Tests
enable_testing()
set(TEST_MODULES memory cfs keyboard timer8 timer16 speaker hd66421 h8s_cpu radio sci2 cyid)
foreach(mod ${TEST_MODULES})
    add_executable(test_${mod} tests/test_${mod}.c)
    target_link_libraries(test_${mod} cybiko_core)
    target_include_directories(test_${mod} PRIVATE tests)
    add_test(NAME ${mod} COMMAND test_${mod})
endforeach()

if(EXISTS "${CMAKE_SOURCE_DIR}/../cybiko-lilygo/cybiko-emu/tdeck_keymap.c")
    add_executable(test_tdeck_keymap
        tests/test_tdeck_keymap.c
        ../cybiko-lilygo/cybiko-emu/tdeck_keymap.c)
    target_include_directories(test_tdeck_keymap PRIVATE
        tests ../cybiko-lilygo/cybiko-emu)
    add_test(NAME tdeck_keymap COMMAND test_tdeck_keymap)
endif()
