cmake_minimum_required(VERSION 3.21)
project(EGtoolsFrontend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Console subsystem (no WIN32 flag) on purpose: the app drives CLI child
# processes and offers a --selftest mode whose stdout is used for headless QA.
add_executable(EGtoolsFrontend
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/pdfview.h
    src/pdfview.cpp
    src/clirunner.h
    src/clirunner.cpp
    src/selftest.h
    src/selftest.cpp
    src/manualdialog.h
    src/manualdialog.cpp
    src/types.h
)

# Embed the in-app manual as a Qt resource so it travels inside the binary.
# NOTE: must come AFTER add_executable (the target must already exist).
qt_add_resources(EGtoolsFrontend "manual"
    PREFIX "/"
    BASE "res"
    FILES res/manual.html
)

target_link_libraries(EGtoolsFrontend PRIVATE Qt6::Widgets)

if(MSVC)
    target_compile_options(EGtoolsFrontend PRIVATE /utf-8)
endif()

if(WIN32)
    target_compile_definitions(EGtoolsFrontend PRIVATE EGTOOLS_WIN32)
endif()