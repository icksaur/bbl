cmake_minimum_required(VERSION 3.14)
project(bbl LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(lz4 STATIC vendor/lz4/lz4.c vendor/lz4/lz4frame.c vendor/lz4/lz4hc.c vendor/lz4/xxhash.c)
set_target_properties(lz4 PROPERTIES C_STANDARD 99)
target_include_directories(lz4 PUBLIC vendor/lz4)

add_library(bbl_lib STATIC src/bbl.cpp src/compiler.cpp src/disasm.cpp src/jit.cpp src/dap.cpp)
target_include_directories(bbl_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bbl_lib PUBLIC Threads::Threads lz4 yyjson)
set_source_files_properties(src/jit.cpp PROPERTIES COMPILE_OPTIONS "-fomit-frame-pointer")

add_library(linenoise STATIC vendor/linenoise/linenoise.c)
set_target_properties(linenoise PROPERTIES C_STANDARD 99)
target_include_directories(linenoise PUBLIC vendor/linenoise)

add_executable(bbl src/main.cpp src/lsp.cpp)
target_link_libraries(bbl PRIVATE bbl_lib linenoise)

install(TARGETS bbl RUNTIME DESTINATION bin)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(bbl_tests tests/test_bbl.cpp)
    target_link_libraries(bbl_tests PRIVATE bbl_lib)

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_executable(bbl_fuzz tests/fuzz.cpp)
        target_link_libraries(bbl_fuzz PRIVATE bbl_lib)
        target_compile_options(bbl_fuzz PRIVATE -fsanitize=fuzzer,address)
        target_link_options(bbl_fuzz PRIVATE -fsanitize=fuzzer,address)
    else()
        add_executable(bbl_fuzz tests/fuzz_stdin.cpp)
        target_link_libraries(bbl_fuzz PRIVATE bbl_lib)
    endif()
endif()

add_library(yyjson STATIC vendor/yyjson/yyjson.c)
set_target_properties(yyjson PROPERTIES C_STANDARD 99)
target_include_directories(yyjson PUBLIC vendor/yyjson)
