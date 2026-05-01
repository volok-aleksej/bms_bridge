find_package(GTest REQUIRED)
include(GoogleTest)

# Per-protocol static libs of production sources, kept separate from
# bms_emulator so test binaries don't drag in main() / threads / BLE stack.

add_library(bms_jk_protocol STATIC
    ${CMAKE_SOURCE_DIR}/src/jk_protocol.cpp
)
target_include_directories(bms_jk_protocol PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_library(bms_pylontech STATIC
    ${CMAKE_SOURCE_DIR}/src/pylontech.cpp
)
target_include_directories(bms_pylontech PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(jk_protocol_test jk_protocol_test.cpp)
target_link_libraries(jk_protocol_test PRIVATE
    bms_jk_protocol
    GTest::gtest_main
)
gtest_discover_tests(jk_protocol_test)

add_executable(pylontech_test pylontech_test.cpp)
target_link_libraries(pylontech_test PRIVATE
    bms_pylontech
    GTest::gtest_main
)
gtest_discover_tests(pylontech_test)
