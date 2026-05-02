cmake_minimum_required(VERSION 3.20)
project(bms_bridge
    VERSION 1.0.1
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CONFUSE REQUIRED IMPORTED_TARGET libconfuse)

find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SimpleBLE/simpleble)

file(GLOB_RECURSE BMS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE BMS_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)

add_executable(bms_bridge ${BMS_SOURCES} ${BMS_HEADERS})

target_include_directories(bms_bridge PRIVATE include)

target_link_libraries(bms_bridge PRIVATE
    PkgConfig::CONFUSE
    spdlog::spdlog
    simpleble::simpleble
    Threads::Threads
)

include(GNUInstallDirs)
install(TARGETS bms_bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/bms_bridge.conf
        RENAME bms_bridge.conf.template
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/bms_bridge)

enable_testing()
add_subdirectory(tests)

if(DEFINED ENV{BMS_TESTS})
    set(_test_stamp ${CMAKE_BINARY_DIR}/tests/run_tests.stamp)
    add_custom_command(
        OUTPUT  ${_test_stamp}
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND ${CMAKE_COMMAND} -E touch ${_test_stamp}
        DEPENDS jk_protocol_test pylontech_test
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running unit tests"
        VERBATIM
    )
    add_custom_target(run_tests ALL DEPENDS ${_test_stamp})
endif()
