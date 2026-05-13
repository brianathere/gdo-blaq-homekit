cmake_minimum_required(VERSION 3.20)

project(gdo_blaq_host_tests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HOST_SANITIZERS "Build host tests with AddressSanitizer and UBSan" ON)
option(HOST_FUZZING "Build libFuzzer targets" ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(HOST_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/mocks/include"
    "${CMAKE_CURRENT_LIST_DIR}/mocks"
    "${REPO_ROOT}/main"
    "${REPO_ROOT}/components/gdolib"
    "${REPO_ROOT}/components/gdolib/include"
    "${REPO_ROOT}/esp-homekit-sdk/components/homekit/esp_hap_core/include"
    "${REPO_ROOT}/esp-homekit-sdk/components/homekit/esp_hap_core/src/priv_includes"
    "${REPO_ROOT}/esp-homekit-sdk/components/homekit/hkdf-sha/upstream"
)

set(HOST_WARNING_FLAGS
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

set(HOST_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
set(HOST_FUZZER_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)

add_library(host_crypto
    "${REPO_ROOT}/esp-homekit-sdk/components/homekit/hkdf-sha/upstream/sha224-256.c"
    "${REPO_ROOT}/esp-homekit-sdk/components/homekit/hkdf-sha/upstream/usha.c"
)
target_include_directories(host_crypto PUBLIC ${HOST_INCLUDE_DIRS})
target_compile_options(host_crypto PRIVATE ${HOST_WARNING_FLAGS})

add_library(host_mocks
    mocks/host_mocks.cpp
    mocks/gdo_host.cpp
)
target_include_directories(host_mocks PUBLIC ${HOST_INCLUDE_DIRS})
target_compile_options(host_mocks PRIVATE ${HOST_WARNING_FLAGS})
target_link_libraries(host_mocks PUBLIC host_crypto)

add_library(host_app_logic
    "${REPO_ROOT}/components/gdolib/secplus.c"
    "${REPO_ROOT}/esp-homekit-sdk/components/homekit/esp_hap_core/src/esp_hap_pair_common.c"
    "${REPO_ROOT}/main/app_events.cpp"
    "${REPO_ROOT}/main/app_settings.cpp"
)
target_include_directories(host_app_logic PUBLIC ${HOST_INCLUDE_DIRS})
target_compile_options(host_app_logic PRIVATE ${HOST_WARNING_FLAGS})
target_link_libraries(host_app_logic PUBLIC host_mocks)

if(HOST_SANITIZERS)
    target_compile_options(host_crypto PRIVATE ${HOST_SANITIZER_FLAGS})
    target_link_options(host_crypto PRIVATE ${HOST_SANITIZER_FLAGS})
    target_compile_options(host_mocks PRIVATE ${HOST_SANITIZER_FLAGS})
    target_link_options(host_mocks PRIVATE ${HOST_SANITIZER_FLAGS})
    target_compile_options(host_app_logic PRIVATE ${HOST_SANITIZER_FLAGS})
    target_link_options(host_app_logic PRIVATE ${HOST_SANITIZER_FLAGS})
endif()

add_executable(host_unit_tests
    host_unit_tests.cpp
)
target_include_directories(host_unit_tests PRIVATE ${HOST_INCLUDE_DIRS})
target_compile_options(host_unit_tests PRIVATE ${HOST_WARNING_FLAGS})
target_link_libraries(host_unit_tests PRIVATE host_app_logic)

add_executable(host_sim_smoke
    host_sim_smoke.cpp
)
target_include_directories(host_sim_smoke PRIVATE ${HOST_INCLUDE_DIRS})
target_compile_options(host_sim_smoke PRIVATE ${HOST_WARNING_FLAGS})
target_link_libraries(host_sim_smoke PRIVATE host_app_logic)

if(HOST_SANITIZERS)
    target_compile_options(host_unit_tests PRIVATE ${HOST_SANITIZER_FLAGS})
    target_link_options(host_unit_tests PRIVATE ${HOST_SANITIZER_FLAGS})
    target_compile_options(host_sim_smoke PRIVATE ${HOST_SANITIZER_FLAGS})
    target_link_options(host_sim_smoke PRIVATE ${HOST_SANITIZER_FLAGS})
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME host_unit_tests COMMAND host_unit_tests)
    add_test(NAME host_sim_smoke COMMAND host_sim_smoke)
endif()

if(HOST_FUZZING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(host_secplus_fuzz
        "${REPO_ROOT}/components/gdolib/secplus.c"
        "${REPO_ROOT}/tests/fuzz/secplus_fuzz.c"
    )
    target_include_directories(host_secplus_fuzz PRIVATE ${HOST_INCLUDE_DIRS})
    target_compile_options(host_secplus_fuzz PRIVATE ${HOST_WARNING_FLAGS} ${HOST_FUZZER_FLAGS})
    target_link_options(host_secplus_fuzz PRIVATE ${HOST_FUZZER_FLAGS})

    add_executable(host_hap_tlv_fuzz
        fuzz/host_hap_tlv_fuzz.cpp
    )
    target_include_directories(host_hap_tlv_fuzz PRIVATE ${HOST_INCLUDE_DIRS})
    target_compile_options(host_hap_tlv_fuzz PRIVATE ${HOST_WARNING_FLAGS} ${HOST_FUZZER_FLAGS})
    target_link_options(host_hap_tlv_fuzz PRIVATE ${HOST_FUZZER_FLAGS})
    target_link_libraries(host_hap_tlv_fuzz PRIVATE host_app_logic)

    add_executable(host_app_events_fuzz
        fuzz/host_app_events_fuzz.cpp
    )
    target_include_directories(host_app_events_fuzz PRIVATE ${HOST_INCLUDE_DIRS})
    target_compile_options(host_app_events_fuzz PRIVATE ${HOST_WARNING_FLAGS} ${HOST_FUZZER_FLAGS})
    target_link_options(host_app_events_fuzz PRIVATE ${HOST_FUZZER_FLAGS})
    target_link_libraries(host_app_events_fuzz PRIVATE host_app_logic)
endif()
