cmake_minimum_required(VERSION 3.20)
project(SplitASIO VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC CRT (/MT) so released binaries have zero VC++ Redistributable dependencies
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Main Windows x64 ASIO COM DLL (provides both "SplitASIO" and "SplitASIO Tap")
add_library(split_asio SHARED
    src/dll_main.cpp
    src/bridge_driver.cpp
    src/tap_driver.cpp
    src/shared_tap.cpp
    src/wasapi_sender.cpp
    src/asio_host.cpp
    src/asio_registry.cpp
    src/config.cpp
    src/control_panel.cpp
    src/exports.def
    src/version.rc
)
target_include_directories(split_asio PRIVATE src)
target_link_libraries(split_asio PRIVATE ole32 advapi32 user32 gdi32 shell32 comctl32 avrt uuid)
target_compile_definitions(split_asio PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(split_asio PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
endif()

# Standalone GUI settings launcher (Start Menu / post-install configuration)
add_executable(split_asio_config WIN32
    src/tools/config_app.cpp
    src/wasapi_sender.cpp
    src/asio_host.cpp
    src/asio_registry.cpp
    src/config.cpp
    src/control_panel.cpp
    src/app.rc
)
target_include_directories(split_asio_config PRIVATE src)
target_link_libraries(split_asio_config PRIVATE ole32 advapi32 user32 gdi32 shell32 comctl32 avrt uuid)
target_compile_definitions(split_asio_config PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(split_asio_config PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
endif()

# Diagnostic probe tool for installed ASIO drivers
add_executable(asio_probe
    src/tools/asio_probe.cpp
    src/asio_registry.cpp
)
target_include_directories(asio_probe PRIVATE src)
target_link_libraries(asio_probe PRIVATE ole32 advapi32 user32)
target_compile_definitions(asio_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# End-to-end integration test loading split_asio.dll via COM (tests both SplitASIO and SplitASIO Tap)
add_executable(bridge_test
    src/tools/bridge_test.cpp
    src/shared_tap.cpp
)
target_include_directories(bridge_test PRIVATE src)
target_link_libraries(bridge_test PRIVATE ole32 user32)
target_compile_definitions(bridge_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_dependencies(bridge_test split_asio)

# Hardware-free unit test suite (runs in CI via CTest)
enable_testing()
add_executable(split_asio_tests
    tests/unit_tests.cpp
    src/shared_tap.cpp
)
target_include_directories(split_asio_tests PRIVATE src)
target_link_libraries(split_asio_tests PRIVATE ole32 user32)
target_compile_definitions(split_asio_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(split_asio_tests PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
endif()
add_dependencies(split_asio_tests split_asio)
add_test(
    NAME split_asio_unit_tests
    COMMAND split_asio_tests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:split_asio>
)
