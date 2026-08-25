cmake_minimum_required(VERSION 3.20)
project(edgenode_host_tests LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(EDGENODE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

if(NANOPB_ROOT)
    add_library(edgenode_nanopb STATIC
        "${NANOPB_ROOT}/pb_common.c"
        "${NANOPB_ROOT}/pb_decode.c"
        "${NANOPB_ROOT}/pb_encode.c"
        "${EDGENODE_ROOT}/generated/edge.pb.c")
    target_include_directories(edgenode_nanopb PUBLIC
        "${NANOPB_ROOT}"
        "${EDGENODE_ROOT}/generated")
else()
    find_package(nanopb CONFIG REQUIRED)
    add_library(edgenode_nanopb STATIC
        "${EDGENODE_ROOT}/generated/edge.pb.c")
    target_include_directories(edgenode_nanopb PUBLIC
        "${EDGENODE_ROOT}/generated")
    target_link_libraries(edgenode_nanopb PUBLIC nanopb::protobuf-nanopb-static)
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # Flexible array members are standard C99, but MSVC reports C4200.
    add_compile_options(/W4 /WX /permissive- /wd4200)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

enable_testing()

add_executable(edge_protocol_test
    edge_protocol_test.c
    "${EDGENODE_ROOT}/src/edge_protocol.c")
target_include_directories(edge_protocol_test PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edge_protocol_test PRIVATE edgenode_nanopb)
add_test(NAME edge_protocol COMMAND edge_protocol_test)

add_executable(edge_config_test
    edge_config_test.c
    "${EDGENODE_ROOT}/src/edge_runtime_config.c")
target_include_directories(edge_config_test PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edge_config_test PRIVATE edgenode_nanopb)
add_test(NAME edge_config COMMAND edge_config_test)

add_executable(edge_runtime_test
    edge_runtime_test.c
    "${EDGENODE_ROOT}/src/edge_device_runtime.c"
    "${EDGENODE_ROOT}/src/edge_modbus.c"
    "${EDGENODE_ROOT}/src/edge_s7.c")
target_include_directories(edge_runtime_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_runtime COMMAND edge_runtime_test)

add_executable(edge_outbox_test
    edge_outbox_test.c
    "${EDGENODE_ROOT}/src/edge_outbox.c")
target_include_directories(edge_outbox_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_outbox COMMAND edge_outbox_test)

add_executable(edge_retry_test
    edge_retry_test.c
    "${EDGENODE_ROOT}/src/edge_retry.c")
target_include_directories(edge_retry_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_retry COMMAND edge_retry_test)

add_executable(edge_migration_test edge_migration_test.c)
target_compile_definitions(edge_migration_test PRIVATE
    EDGENODE_MIGRATION_SCRIPT="${EDGENODE_ROOT}/files/95-edgenode-platform")
add_test(NAME edge_migration COMMAND edge_migration_test)

add_executable(edge_interface_test
    edge_interface_test.c
    "${EDGENODE_ROOT}/src/edge_interface.c")
target_include_directories(edge_interface_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_interface COMMAND edge_interface_test)

add_executable(edge_firmware_policy_test edge_firmware_policy_test.c)
target_include_directories(edge_firmware_policy_test PRIVATE "${EDGENODE_ROOT}/src")
add_test(NAME edge_firmware_policy COMMAND edge_firmware_policy_test)

add_executable(edgenode_stability_x64
    edgenode_stability_test.c
    "${EDGENODE_ROOT}/src/edge_protocol.c"
    "${EDGENODE_ROOT}/src/edge_runtime_config.c"
    "${EDGENODE_ROOT}/src/edge_device_runtime.c"
    "${EDGENODE_ROOT}/src/edge_modbus.c"
    "${EDGENODE_ROOT}/src/edge_s7.c"
    "${EDGENODE_ROOT}/src/edge_outbox.c"
    "${EDGENODE_ROOT}/src/edge_retry.c")
target_include_directories(edgenode_stability_x64 PRIVATE "${EDGENODE_ROOT}/src")
target_link_libraries(edgenode_stability_x64 PRIVATE edgenode_nanopb)
if(WIN32)
    target_link_libraries(edgenode_stability_x64 PRIVATE psapi)
endif()
set_target_properties(edgenode_stability_x64 PROPERTIES
    OUTPUT_NAME "edgenode-stability-x64")
add_test(NAME edgenode_stability COMMAND edgenode_stability_x64 --iterations 20000)

if(UNIX)
    add_executable(edge_terminal_test
        edge_terminal_test.c
        "${EDGENODE_ROOT}/src/edge_terminal.c")
    target_compile_definitions(edge_terminal_test PRIVATE EDGENODE_TERMINAL_TEST=1)
    target_include_directories(edge_terminal_test PRIVATE "${EDGENODE_ROOT}/src")
    target_link_libraries(edge_terminal_test PRIVATE edgenode_nanopb)
    find_library(UTIL_LIBRARY util)
    if(UTIL_LIBRARY)
        target_link_libraries(edge_terminal_test PRIVATE "${UTIL_LIBRARY}")
    endif()
    add_test(NAME edge_terminal COMMAND edge_terminal_test)

    add_executable(edge_process_test
        edge_process_test.c
        "${EDGENODE_ROOT}/src/edge_process.c")
    target_include_directories(edge_process_test PRIVATE "${EDGENODE_ROOT}/src")
    add_test(NAME edge_process COMMAND edge_process_test)

    add_executable(edge_acquisition_test
        edge_acquisition_test.c
        "${EDGENODE_ROOT}/src/edge_acquisition.c"
        "${EDGENODE_ROOT}/src/edge_runtime_config.c"
        "${EDGENODE_ROOT}/src/edge_device_runtime.c"
        "${EDGENODE_ROOT}/src/edge_modbus.c"
        "${EDGENODE_ROOT}/src/edge_s7.c"
        "${EDGENODE_ROOT}/src/edge_protocol.c"
        "${EDGENODE_ROOT}/src/edge_process.c"
        "${EDGENODE_ROOT}/src/log.c")
    target_include_directories(edge_acquisition_test PRIVATE "${EDGENODE_ROOT}/src")
    target_link_libraries(edge_acquisition_test PRIVATE edgenode_nanopb m)
    add_test(NAME edge_acquisition COMMAND edge_acquisition_test)
endif()
