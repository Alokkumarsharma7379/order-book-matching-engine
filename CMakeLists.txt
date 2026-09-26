cmake_minimum_required(VERSION 3.21)

project(OrderBookEngine VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
option(ORDERBOOK_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ORDERBOOK_BUILD_PYTHON "Build the pybind11 extension" OFF)
option(ORDERBOOK_BUILD_BENCHMARKS "Build the standalone benchmark" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(orderbook_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()

add_library(orderbook_sanitizers INTERFACE)
if(ORDERBOOK_ENABLE_SANITIZERS)
    if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "This sanitizer configuration requires GCC or Clang with GNU-style flags")
    endif()
    target_compile_options(orderbook_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(orderbook_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all)
endif()

add_library(orderbook_domain INTERFACE)
target_include_directories(orderbook_domain INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/include")
target_compile_features(orderbook_domain INTERFACE cxx_std_20)

add_library(orderbook_core STATIC
    engine/src/order_book.cpp
    engine/src/matching_engine.cpp)
target_link_libraries(orderbook_core PUBLIC orderbook_domain orderbook_sanitizers)
set_target_properties(orderbook_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
orderbook_warnings(orderbook_core)

if(ORDERBOOK_BUILD_PYTHON)
    find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
    set(PYBIND11_FINDPYTHON ON)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(_core NO_EXTRAS engine/src/bindings.cpp)
    target_link_libraries(_core PRIVATE orderbook_core)
    orderbook_warnings(_core)
    install(TARGETS _core LIBRARY DESTINATION orderbook RUNTIME DESTINATION orderbook)
    if(MINGW)
        # A local LLVM-MinGW wheel must carry its C++ runtime alongside the extension.
        get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(runtime IN ITEMS libc++.dll libunwind.dll)
            if(EXISTS "${compiler_dir}/${runtime}")
                install(FILES "${compiler_dir}/${runtime}" DESTINATION orderbook)
            endif()
        endforeach()
    endif()
endif()

if(ORDERBOOK_BUILD_BENCHMARKS)
    add_executable(orderbook_benchmark engine/benchmarks/benchmark.cpp)
    target_link_libraries(orderbook_benchmark PRIVATE orderbook_core)
    orderbook_warnings(orderbook_benchmark)
endif()

if(BUILD_TESTING)
    foreach(check IN ITEMS build_check order_book_check matching_check order_management_check)
        add_executable(${check} engine/tools/${check}.cpp)
        target_link_libraries(${check} PRIVATE orderbook_core)
        orderbook_warnings(${check})
        add_test(NAME ${check} COMMAND ${check})
        set_tests_properties(${check} PROPERTIES LABELS "smoke")
    endforeach()
    add_subdirectory(engine/tests)
endif()
