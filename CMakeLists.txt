cmake_minimum_required(VERSION 3.20)
project(bms_bridge
    VERSION 2.0.0
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CONFUSE  REQUIRED IMPORTED_TARGET libconfuse)
pkg_check_modules(SQLITE3  REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(CJSON    REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(LIBEVENT          REQUIRED IMPORTED_TARGET libevent)
pkg_check_modules(LIBEVENT_PTHREADS REQUIRED IMPORTED_TARGET libevent_pthreads)

find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SimpleBLE/simpleble)

file(GLOB_RECURSE BMS_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(REMOVE_ITEM BMS_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)
file(GLOB_RECURSE BMS_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)

# All production code as one static lib, shared between the main binary and
# the test binary. main.cpp is the only thing kept out so the test target
# doesn't fight gtest_main for the entry point.
add_library(bms_core STATIC ${BMS_CORE_SOURCES} ${BMS_HEADERS})
target_include_directories(bms_core PUBLIC include)
target_link_libraries(bms_core PUBLIC
    PkgConfig::CONFUSE
    PkgConfig::SQLITE3
    spdlog::spdlog
    Threads::Threads
)
target_link_libraries(bms_core PRIVATE
    PkgConfig::CJSON
    PkgConfig::LIBEVENT
    PkgConfig::LIBEVENT_PTHREADS
)
# simpleble is referenced only from ble_client.cpp. Keeping it PRIVATE means
# the test binary (which never pulls ble_client.o into its link) doesn't
# need to link the BLE stack either.
target_link_libraries(bms_core PRIVATE simpleble::simpleble)

add_executable(bms_bridge src/main.cpp)
target_link_libraries(bms_bridge PRIVATE
    bms_core
    simpleble::simpleble
)

include(GNUInstallDirs)
install(TARGETS bms_bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/bms_bridge.conf
        RENAME bms_bridge.conf.template
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/bms_bridge)
install(DIRECTORY config/www/
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/bms_bridge/www)
install(PROGRAMS scripts/migrate-db.sh
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/bms_bridge)

enable_testing()
add_subdirectory(tests)

if(DEFINED ENV{BMS_TESTS})
    set(_test_stamp ${CMAKE_BINARY_DIR}/tests/run_tests.stamp)
    add_custom_command(
        OUTPUT  ${_test_stamp}
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        COMMAND ${CMAKE_COMMAND} -E touch ${_test_stamp}
        DEPENDS bms_tests
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running unit tests"
        VERBATIM
    )
    add_custom_target(run_tests ALL DEPENDS ${_test_stamp})
endif()
