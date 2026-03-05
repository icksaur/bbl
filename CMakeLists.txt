cmake_minimum_required(VERSION 3.14)
project(bbl LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(lz4 STATIC vendor/lz4/lz4.c vendor/lz4/lz4frame.c vendor/lz4/lz4hc.c vendor/lz4/xxhash.c)
set_target_properties(lz4 PROPERTIES C_STANDARD 99)
target_include_directories(lz4 PUBLIC vendor/lz4)

add_library(bbl_lib STATIC bbl.cpp compiler.cpp vm.cpp disasm.cpp jit.cpp)
target_include_directories(bbl_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bbl_lib PUBLIC Threads::Threads lz4)

add_executable(bbl main.cpp)
target_link_libraries(bbl PRIVATE bbl_lib)

install(TARGETS bbl RUNTIME DESTINATION bin)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(bbl_tests tests/test_bbl.cpp)
    target_link_libraries(bbl_tests PRIVATE bbl_lib)
endif()
