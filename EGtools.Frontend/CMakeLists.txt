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
    src/homepage.h
    src/homepage.cpp
    src/schemes.h
    src/schemes.cpp
    src/schemepage.h
    src/schemepage.cpp
    src/types.h
    res/app_icon.rc
)

# Embed the in-app manual + the window/taskbar icon as Qt resources so they
# travel inside the binary.
# NOTE: must come AFTER add_executable (the target must already exist).
qt_add_resources(EGtoolsFrontend "appres"
    PREFIX "/"
    BASE "res"
    FILES res/manual.html
          res/app_icon.png
)

target_link_libraries(EGtoolsFrontend PRIVATE Qt6::Widgets)

# Embed the EXE icon (Explorer/taskbar/Start menu) on Windows.
if(WIN32)
    enable_language(RC)
endif()

if(MSVC)
    target_compile_options(EGtoolsFrontend PRIVATE /utf-8)
endif()

if(WIN32)
    target_compile_definitions(EGtoolsFrontend PRIVATE EGTOOLS_WIN32)
endif()