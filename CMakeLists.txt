cmake_minimum_required(VERSION 3.25)

project(ArkhamHorrorLinux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(QT_QML_GENERATE_QMLLS_INI ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Svg WebSockets)

qt_standard_project_setup(REQUIRES 6.8)

add_library(arkham_foundation STATIC
    src/AppCommand.cpp
    src/AppCommand.h
    src/ServerProfile.cpp
    src/ServerProfile.h
)

target_include_directories(arkham_foundation PUBLIC src)
target_link_libraries(arkham_foundation PUBLIC Qt6::Core)

qt_add_executable(arkham-horror
    src/main.cpp
)

qt_add_qml_module(arkham-horror
    URI ArkhamHorror
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(arkham-horror PRIVATE
    arkham_foundation
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
    Qt6::WebSockets
)

install(TARGETS arkham-horror
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    qt_add_executable(arkham-foundation-tests
        tests/FoundationTests.cpp
    )
    target_link_libraries(arkham-foundation-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME foundation COMMAND arkham-foundation-tests)
endif()
