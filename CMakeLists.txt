cmake_minimum_required(VERSION 3.16)
project(datatableviewer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# ---- SeerSdk (plugin host interface) ----
include(FetchContent)
message(STATUS "Fetching third-party dependencies... seer-sdk")
FetchContent_Declare(SeerSdk
    GIT_REPOSITORY https://github.com/ccseer/Seer-sdk.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SeerSdk)

# ---- Sources ----
set(CORE_SOURCES
    src/core/table_data.h
    src/core/itable_parser.h
    src/core/type_inferrer.h
    src/core/type_inferrer.cpp
    src/core/parser_registry.h
    src/core/parser_registry.cpp
)

set(PARSER_SOURCES
    src/parsers/csv_parser.h
    src/parsers/csv_parser.cpp
    src/parsers/sqlite_parser.h
    src/parsers/sqlite_parser.cpp
)

set(WORKER_SOURCES
    src/workers/background_thread.h
    src/workers/table_worker.h
    src/workers/table_worker.cpp
)

set(UI_SOURCES
    src/ui/search_bar.h
    src/ui/search_bar.cpp
    src/ui/status_bar.h
    src/ui/status_bar.cpp
    src/ui/style_assets.h
    src/ui/table_model.h
    src/ui/table_model.cpp
    src/ui/table_filter_proxy.h
    src/ui/table_filter_proxy.cpp
    src/ui/table_renderer.h
    src/ui/table_renderer.cpp
    src/ui/table_picker.h
    src/ui/table_picker.cpp
    src/ui/data_table_viewer.h
    src/ui/data_table_viewer.cpp
)

# ---- DataTableViewer DLL ----
add_library(datatableviewer SHARED
    ${CORE_SOURCES}
    ${PARSER_SOURCES}
    ${WORKER_SOURCES}
    ${UI_SOURCES}
    ${seersdk_SOURCE_DIR}/seer/viewerbase.h
)

target_include_directories(datatableviewer PRIVATE src/)

target_link_libraries(datatableviewer PRIVATE
    SeerSdk::SeerSdk
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
    unofficial::sqlite3::sqlite3
)

set_target_properties(datatableviewer PROPERTIES
    OUTPUT_NAME "datatableviewer" SUFFIX ".dll")

# Copy plugin.json to output dir (Seer plugin discovery needs it alongside the DLL)
add_custom_command(TARGET datatableviewer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/bin/plugin.json"
        "$<TARGET_FILE_DIR:datatableviewer>/plugin.json")

# ---- Seer Integration Test ----
add_executable(datatableviewer_test WIN32 src/test.cpp)
target_include_directories(datatableviewer_test PRIVATE src/)
target_link_libraries(datatableviewer_test PRIVATE
    datatableviewer SeerSdk::SeerSdk Qt6::Core Qt6::Gui Qt6::Widgets)
set_target_properties(datatableviewer_test PROPERTIES
    VS_STARTUP_PROJECT ON)

# ---- Tests ----
option(BUILD_TESTS "Build parser unit tests" ON)

if(BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)

    set(FIXTURES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    set(TABLE_TEST_TARGETS)

    function(add_table_test NAME SOURCE)
        add_executable(${NAME} ${SOURCE})
        target_include_directories(${NAME} PRIVATE src/)
        target_link_libraries(${NAME} PRIVATE Qt6::Test Qt6::Svg unofficial::sqlite3::sqlite3)
        target_sources(${NAME} PRIVATE
            ${CORE_SOURCES}
            ${PARSER_SOURCES}
        )
        target_compile_definitions(${NAME} PRIVATE
            FIXTURES_DIR="${FIXTURES_DIR}")
        add_test(NAME ${NAME} COMMAND ${NAME})
        set(TABLE_TEST_TARGETS ${TABLE_TEST_TARGETS} ${NAME} PARENT_SCOPE)
    endfunction()

    add_table_test(test_csv_parser    tests/test_csv_parser.cpp)
    add_table_test(test_type_inferrer tests/test_type_inferrer.cpp)
    add_table_test(test_sqlite_parser  tests/test_sqlite_parser.cpp)

    add_executable(test_table_model tests/test_table_model.cpp)
    target_include_directories(test_table_model PRIVATE src/)
    target_link_libraries(test_table_model PRIVATE Qt6::Test Qt6::Core)
    target_sources(test_table_model PRIVATE
        src/ui/table_model.h
        src/ui/table_model.cpp
    )
    add_test(NAME test_table_model COMMAND test_table_model)
    list(APPEND TABLE_TEST_TARGETS test_table_model)

    add_custom_target(run_all_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                $<$<BOOL:$<CONFIG>>:-C>
                $<$<BOOL:$<CONFIG>>:$<CONFIG>>
        DEPENDS ${TABLE_TEST_TARGETS}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Building and running all table tests"
        USES_TERMINAL
    )
endif()
