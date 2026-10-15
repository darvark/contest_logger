cmake_minimum_required(VERSION 3.21)

project(ContestLogger
    VERSION 0.1.0
    DESCRIPTION "Cross-platform amateur radio contest logger"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Widgets Sql Network)
find_package(yaml-cpp CONFIG REQUIRED)

option(LOGGER_ENABLE_HAMLIB "Build Hamlib radio and CW support when available" ON)
if(LOGGER_ENABLE_HAMLIB)
    find_path(HAMLIB_INCLUDE_DIR hamlib/rig.h)
    find_library(HAMLIB_LIBRARY NAMES hamlib)
endif()

add_library(logger_core
    include/logger/cluster/dx_cluster_service.h
    include/logger/contest/contest_definition_loader.h
    include/logger/contest/contest_engine.h
    include/logger/cw/cw_keyer_service.h
    include/logger/data/log_exporter.h
    include/logger/data/support_data_parser.h
    include/logger/radio/radio_service.h
    include/logger/radio/band_plan.h
    include/logger/ui/operator_dialog.h
    src/contest/contest_definition_loader.cpp
    src/contest/contest_engine.cpp
    src/cw/cw_keyer_service.cpp
    src/data/log_exporter.cpp
    src/data/support_data_parser.cpp
    src/cluster/dx_cluster_service.cpp
    src/data/sqlite_repository.cpp
    src/radio/radio_service.cpp
    src/radio/band_plan.cpp
    src/ui/operator_dialog.cpp
)
target_include_directories(logger_core PUBLIC include)
target_link_libraries(logger_core
    PUBLIC
        Qt6::Core
        Qt6::Sql
        Qt6::Widgets
        Qt6::Network
        yaml-cpp::yaml-cpp
)
if(HAMLIB_INCLUDE_DIR AND HAMLIB_LIBRARY)
    target_compile_definitions(logger_core PUBLIC LOGGER_WITH_HAMLIB)
    target_include_directories(logger_core PRIVATE ${HAMLIB_INCLUDE_DIR})
    target_link_libraries(logger_core PRIVATE ${HAMLIB_LIBRARY})
endif()

add_library(logger_ui STATIC
    include/logger/ui/main_window.h
    src/ui/main_window.cpp
    resources/contests.qrc
)
target_link_libraries(logger_ui PUBLIC logger_core)

add_executable(contest_logger
    src/main.cpp
)
target_link_libraries(contest_logger PRIVATE logger_ui)

install(TARGETS contest_logger
    RUNTIME DESTINATION bin
)
install(DIRECTORY contests/examples/
    DESTINATION share/contest-logger/contests
)
install(FILES README.md TODO.md
    DESTINATION share/doc/contest-logger
)

set(CPACK_PACKAGE_NAME "ContestLogger")
set(CPACK_PACKAGE_VENDOR "Contest Logger contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Cross-platform amateur radio contest logger")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Test)

    add_executable(logger_tests
        tests/test_sqlite_repository.cpp
    )
    add_executable(contest_tests
        tests/test_contest_definitions.cpp
    )
    add_executable(radio_tests
        tests/test_radio_service.cpp
    )
    add_executable(band_plan_tests
        tests/test_band_plan.cpp
    )
    add_executable(cluster_tests
        tests/test_dx_cluster.cpp
    )
    add_executable(cw_tests
        tests/test_cw_keyer.cpp
    )
    add_executable(export_tests
        tests/test_log_exporter.cpp
    )
    add_executable(support_data_tests
        tests/test_support_data_parser.cpp
    )
    add_executable(ui_tests
        tests/test_main_window_ui.cpp
    )
    target_link_libraries(ui_tests PRIVATE logger_ui Qt6::Test)
    add_test(NAME ui_tests COMMAND ui_tests)
    set_tests_properties(ui_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    target_compile_definitions(contest_tests PRIVATE
        CONTEST_LOGGER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(logger_tests PRIVATE logger_core Qt6::Test)
    target_link_libraries(contest_tests
        PRIVATE
            logger_core
            Qt6::Test
    )
    target_link_libraries(radio_tests PRIVATE logger_core Qt6::Test)
    target_link_libraries(band_plan_tests PRIVATE logger_core Qt6::Test)
    target_link_libraries(cluster_tests PRIVATE logger_core Qt6::Test)
    target_link_libraries(cw_tests PRIVATE logger_core Qt6::Test)
    target_link_libraries(export_tests PRIVATE logger_core Qt6::Test)
    target_link_libraries(support_data_tests PRIVATE logger_core Qt6::Test)
    add_test(NAME logger_tests COMMAND logger_tests)
    add_test(NAME contest_tests COMMAND contest_tests)
    add_test(NAME radio_tests COMMAND radio_tests)
    add_test(NAME band_plan_tests COMMAND band_plan_tests)
    add_test(NAME cluster_tests COMMAND cluster_tests)
    add_test(NAME cw_tests COMMAND cw_tests)
    add_test(NAME export_tests COMMAND export_tests)
    add_test(NAME support_data_tests COMMAND support_data_tests)
    set_tests_properties(logger_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    set_tests_properties(contest_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    set_tests_properties(cluster_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    set_tests_properties(cw_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    set_tests_properties(export_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    set_tests_properties(support_data_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endif()
