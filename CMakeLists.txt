cmake_minimum_required(VERSION 3.24)
project(myb VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(MYB_BUILD_INTERNAL_TESTS "Build MYB internal protocol tests and fault seams" OFF)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FindMybTools)

# Immutable sources and licenses are part of the checkout, never downloaded here.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/manifest.json" MYB_DEPENDENCY_MANIFEST)
string(JSON MYB_DEPENDENCY_COUNT LENGTH "${MYB_DEPENDENCY_MANIFEST}" dependencies)
math(EXPR MYB_DEPENDENCY_LAST "${MYB_DEPENDENCY_COUNT} - 1")
foreach(dep RANGE 0 ${MYB_DEPENDENCY_LAST})
    string(JSON dependency_root GET "${MYB_DEPENDENCY_MANIFEST}" dependencies ${dep} root)
    string(JSON file_count LENGTH "${MYB_DEPENDENCY_MANIFEST}" dependencies ${dep} files)
    math(EXPR file_last "${file_count} - 1")
    foreach(file_index RANGE 0 ${file_last})
        string(JSON relative GET "${MYB_DEPENDENCY_MANIFEST}" dependencies ${dep} files ${file_index} path)
        string(JSON expected GET "${MYB_DEPENDENCY_MANIFEST}" dependencies ${dep} files ${file_index} sha256)
        set(source_path "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dependency_root}/${relative}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_path}")
        file(SHA256 "${source_path}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Third-party snapshot hash mismatch: ${dependency_root}/${relative}")
        endif()
    endforeach()
endforeach()

add_library(myb_fast_float INTERFACE)
target_include_directories(myb_fast_float SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fast_float/include")
add_library(myb_decimal_ryu STATIC third_party/ryu/ryu/d2s.c)
target_include_directories(myb_decimal_ryu SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ryu")

set(MYB_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MYB_GENERATED_DIR}")
BISON_TARGET(MybParser src/frontend/parser.y "${MYB_GENERATED_DIR}/parser.cpp"
    DEFINES_FILE "${MYB_GENERATED_DIR}/parser.hpp")
FLEX_TARGET(MybScanner src/frontend/lexer.l "${MYB_GENERATED_DIR}/lexer.cpp")
ADD_FLEX_BISON_DEPENDENCY(MybScanner MybParser)
add_custom_target(myb_frontend_generated DEPENDS ${BISON_MybParser_OUTPUTS} ${FLEX_MybScanner_OUTPUTS})
# Generated warnings are isolated from handwritten target warning policy.
add_library(myb_generated OBJECT ${BISON_MybParser_OUTPUTS} ${FLEX_MybScanner_OUTPUTS})
target_include_directories(myb_generated PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${MYB_GENERATED_DIR}")
target_compile_options(myb_generated PRIVATE -fno-fast-math -ffp-contract=off -finput-charset=UTF-8 -fexec-charset=UTF-8)

add_library(myb_core STATIC
    src/runtime/toolchain_contract.cpp
    src/runtime/decimal_codec.cpp
    src/source/utf8.cpp
    src/source/source.cpp
    src/diagnostic/failure.cpp
    src/diagnostic/diagnostic.cpp
    src/platform/windows_io.cpp
    src/runtime/roots.cpp
    src/runtime/heap.cpp
    src/runtime/text_builder.cpp
    src/runtime/value_ops.cpp
    src/runtime/sequence_ops.cpp
    src/runtime/graph_copy.cpp
    src/runtime/display.cpp
    src/ast/nodes.cpp
    src/frontend/flex_allocations.cpp
    src/frontend/scanner.cpp
    src/frontend/driver.cpp
    src/semantic/checker.cpp
    src/bytecode/program.cpp
    src/bytecode/binding_layout.cpp
    src/bytecode/compiler.cpp
    src/bytecode/verify.cpp
    src/builtins/registry.cpp
    src/builtins/io_builtins.cpp
    src/builtins/array_builtins.cpp
    src/builtins/scalar_builtins.cpp
    src/builtins/text_builtins.cpp
    src/vm/vm.cpp
    src/vm/execution_index.cpp
    src/vm/value_store.cpp
    src/vm/globals.cpp
    src/vm/call.cpp
    src/vm/iteration.cpp
    src/cli/options.cpp
    src/cli/session.cpp
    $<TARGET_OBJECTS:myb_generated>
)
add_dependencies(myb_core myb_frontend_generated)
target_include_directories(myb_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" PRIVATE "${MYB_GENERATED_DIR}")
target_link_libraries(myb_core PRIVATE myb_fast_float myb_decimal_ryu)
target_compile_options(myb_core PRIVATE -Wall -Wextra -Wpedantic -fno-fast-math -ffp-contract=off -finput-charset=UTF-8 -fexec-charset=UTF-8)

add_executable(myb src/main.cpp)
target_link_libraries(myb PRIVATE myb_core)
target_compile_options(myb PRIVATE -Wall -Wextra -Wpedantic -fno-fast-math -ffp-contract=off -finput-charset=UTF-8 -fexec-charset=UTF-8)
if(WIN32)
    target_link_options(myb PRIVATE -municode)
endif()

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    get_filename_component(MYB_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(MYB_RUNTIME_COPY_COMMANDS)
    foreach(library IN ITEMS libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        set(runtime_source "${MYB_COMPILER_BIN}/${library}")
        if(NOT EXISTS "${runtime_source}")
            message(FATAL_ERROR "Missing UCRT64 runtime library: ${runtime_source}")
        endif()
        list(APPEND MYB_RUNTIME_COPY_COMMANDS
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime_source}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${library}")
    endforeach()
    # 两个可执行目标共用一次复制；增量构建也恢复被移走的运行库。
    add_custom_target(myb_runtime_files ${MYB_RUNTIME_COPY_COMMANDS} VERBATIM)
    add_dependencies(myb myb_runtime_files)
endif()

if(MYB_BUILD_INTERNAL_TESTS)
    target_compile_definitions(myb_generated PRIVATE MYB_TEST_HOOKS)
    target_include_directories(myb_generated PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/internal")
    target_compile_definitions(myb_core PRIVATE MYB_TEST_HOOKS)
    target_include_directories(myb_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/internal")
    target_sources(myb PRIVATE tests/internal/test_hooks.cpp)
    target_compile_definitions(myb PRIVATE MYB_TEST_HOOKS)
    target_include_directories(myb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/internal")
    add_executable(myb_internal_tests
        tests/internal/main.cpp
        tests/internal/test_hooks.cpp
        tests/internal/toolchain_cases.cpp
        tests/internal/decimal_cases.cpp
        tests/internal/source_cases.cpp
        tests/internal/failure_cases.cpp
        tests/internal/failure_integration_cases.cpp
        tests/internal/diagnostic_cases.cpp
        tests/internal/heap_cases.cpp
        tests/internal/frontend_cases.cpp
        tests/internal/value_ops_cases.cpp
        tests/internal/bytecode_smoke_cases.cpp
        tests/internal/fixtures.cpp
        tests/internal/smoke_cases.cpp
        tests/internal/native_handoff_cases.cpp
        tests/internal/vm_scalar_cases.cpp
        tests/internal/vm_call_cases.cpp
        tests/internal/execution_index_cases.cpp
        tests/internal/vm_failure_cases.cpp
        tests/internal/checker_cases.cpp
        tests/internal/bytecode_cases.cpp
        tests/internal/vm_object_cases.cpp
        tests/internal/graph_display_cases.cpp
        tests/internal/host_io_cases.cpp
        tests/internal/builtin_cases.cpp
        tests/internal/vm_iteration_cases.cpp
    )
    target_compile_definitions(myb_internal_tests PRIVATE MYB_TEST_HOOKS)
    target_include_directories(myb_internal_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/internal" "${MYB_GENERATED_DIR}")
    target_link_libraries(myb_internal_tests PRIVATE myb_core myb_fast_float myb_decimal_ryu)
    target_compile_options(myb_internal_tests PRIVATE -Wall -Wextra -Wpedantic -fno-fast-math -ffp-contract=off -finput-charset=UTF-8 -fexec-charset=UTF-8)
    if(TARGET myb_runtime_files)
        add_dependencies(myb_internal_tests myb_runtime_files)
    endif()
endif()
