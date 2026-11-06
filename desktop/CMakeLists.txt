cmake_minimum_required(VERSION 3.28)

project(GmSelfTrackerQt VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

option(GMS_BUILD_TESTS "Build disposable native desktop tests" ${BUILD_TESTING})
option(GMS_ENABLE_PACKAGING "Enable portable Windows packaging support" OFF)
option(GMS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Sql)
find_package(WebP CONFIG QUIET)

if(TARGET WebP::webp)
    set(GMS_HAS_WEBP TRUE)
elseif(TARGET WebP::webpdecoder)
    set(GMS_HAS_WEBP TRUE)
else()
    set(GMS_HAS_WEBP FALSE)
endif()

if(GMS_ENABLE_PACKAGING AND NOT GMS_HAS_WEBP)
    message(FATAL_ERROR
        "Portable packaging requires libwebp so WebP remains supported. "
        "Install vcpkg, set VCPKG_ROOT, and configure through tools/make.ps1 or pass its toolchain file.")
endif()

add_library(gmst_core STATIC
    src/domain/game.h
    src/infrastructure/database/migration_runner.cpp
    src/infrastructure/database/migration_runner.h
    src/infrastructure/database/game_repository.cpp
    src/infrastructure/database/game_repository.h
    src/infrastructure/assets/image_decoder.cpp
    src/infrastructure/assets/image_decoder.h
    resources/resources.qrc
)
target_include_directories(gmst_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gmst_core PUBLIC Qt6::Core Qt6::Gui Qt6::Sql)
if(TARGET WebP::webp)
    target_link_libraries(gmst_core PUBLIC WebP::webp)
elseif(TARGET WebP::webpdecoder)
    target_link_libraries(gmst_core PUBLIC WebP::webpdecoder)
endif()
target_compile_definitions(gmst_core PUBLIC GMST_HAS_WEBP=$<BOOL:${GMS_HAS_WEBP}>)
target_compile_definitions(gmst_core PUBLIC
    GMST_VERSION="${PROJECT_VERSION}"
    GMST_ORGANIZATION="GmSelfTracker"
    GMST_APPLICATION_NAME="GmSelfTrackerQt"
)

add_executable(GmSelfTrackerQt WIN32
    src/main.cpp
    src/app/main_window.cpp
    src/app/main_window.h
    src/app/theme.cpp
    src/app/theme.h
    src/presentation/widgets/object_history_dialog.cpp
    src/presentation/widgets/sky_banner.cpp
    src/presentation/widgets/sky_banner.h
)
target_link_libraries(GmSelfTrackerQt PRIVATE gmst_core Qt6::Gui Qt6::Widgets)

if(MSVC)
    target_compile_options(gmst_core PRIVATE /W4 /permissive-)
    target_compile_options(GmSelfTrackerQt PRIVATE /W4 /permissive-)
    if(GMS_WARNINGS_AS_ERRORS)
        target_compile_options(gmst_core PRIVATE /WX)
        target_compile_options(GmSelfTrackerQt PRIVATE /WX)
    endif()
else()
    target_compile_options(gmst_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(GmSelfTrackerQt PRIVATE -Wall -Wextra -Wpedantic)
    if(GMS_WARNINGS_AS_ERRORS)
        target_compile_options(gmst_core PRIVATE -Werror)
        target_compile_options(GmSelfTrackerQt PRIVATE -Werror)
    endif()
endif()

if(WIN32)
    install(TARGETS GmSelfTrackerQt RUNTIME DESTINATION bin)
endif()

if(GMS_BUILD_TESTS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_executable(gmst_database_tests tests/database_tests.cpp)
    target_link_libraries(gmst_database_tests PRIVATE gmst_core Qt6::Test)
    add_test(NAME gmst_database_tests COMMAND gmst_database_tests)
    set_tests_properties(gmst_database_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 30
    )

    add_executable(gmst_ui_tests
        tests/ui_tests.cpp
        src/app/main_window.cpp
        src/app/main_window.h
        src/app/theme.cpp
        src/app/theme.h
        src/presentation/widgets/object_history_dialog.cpp
        src/presentation/widgets/sky_banner.cpp
        src/presentation/widgets/sky_banner.h
    )
    target_link_libraries(gmst_ui_tests PRIVATE gmst_core Qt6::Widgets Qt6::Test)
    add_test(NAME gmst_ui_tests COMMAND gmst_ui_tests)
    set_tests_properties(gmst_ui_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 30
    )
endif()

if(GMS_ENABLE_PACKAGING AND NOT WIN32)
    message(FATAL_ERROR "Portable deployment scripts currently target Windows.")
endif()
