cmake_minimum_required(VERSION 3.14)
project(bbl LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(bbl_lib STATIC bbl.cpp)
target_include_directories(bbl_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(bbl main.cpp)
target_link_libraries(bbl PRIVATE bbl_lib)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(bbl_tests tests/test_bbl.cpp)
    target_link_libraries(bbl_tests PRIVATE bbl_lib)
endif()
