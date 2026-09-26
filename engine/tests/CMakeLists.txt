include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

FetchContent_Declare(Catch2
    URL https://codeload.github.com/catchorg/Catch2/tar.gz/refs/tags/v3.8.1
    URL_HASH SHA256=18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79)
FetchContent_MakeAvailable(Catch2)

add_executable(engine_tests
    matching_tests.cpp
    order_management_tests.cpp
    validation_tests.cpp
    order_book_tests.cpp
    reference_tests.cpp)
target_link_libraries(engine_tests PRIVATE orderbook_core Catch2::Catch2WithMain)
orderbook_warnings(engine_tests)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(engine_tests
    TEST_PREFIX "unit::"
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "unit")

add_executable(allocation_failure_check allocation_failure_check.cpp)
target_link_libraries(allocation_failure_check PRIVATE orderbook_core)
orderbook_warnings(allocation_failure_check)
add_test(NAME allocation_failure_check COMMAND allocation_failure_check)
set_tests_properties(allocation_failure_check PROPERTIES LABELS "allocation")
